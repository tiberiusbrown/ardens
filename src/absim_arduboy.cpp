#include "absim.hpp"

#include <algorithm>
#include <iostream>

#include <bitsery/bitsery.h>
#include <bitsery/brief_syntax.h>
#include <bitsery/brief_syntax/array.h>
#include <bitsery/brief_syntax/string.h>
#include <bitsery/brief_syntax/tuple.h>
#include <bitsery/brief_syntax/vector.h>
#include <bitsery/brief_syntax/map.h>
#include <bitsery/ext/std_map.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/vector.h>

#include "absim_atmega32u4.hpp"
#include "absim_ssd1306.hpp"

namespace absim
{

void arduboy_t::update_game_hash()
{
    // FNV-1a 64-bit
    constexpr uint64_t OFFSET = 0xcbf29ce484222325;
    constexpr uint64_t PRIME = 0x100000001b3;
    uint64_t h = OFFSET;
    for(auto byte : cpu.prog)
    {
        h ^= byte;
        h *= PRIME;
    }
    for(auto byte : fx.data)
    {
        h ^= byte;
        h *= PRIME;
    }

    game_hash = h;
}

void arduboy_t::load_savedata(std::istream& f)
{
    using StreamAdapter = bitsery::InputStreamAdapter;
    bitsery::Deserializer<StreamAdapter> ar(f);
    savedata.clear();
    ar(savedata);
    if(savedata.game_hash != game_hash)
    {
        savedata.clear();
        return;
    }

    // overwrite eeprom / fx with saved data

    auto const& d = savedata;
    if(d.eeprom.size() == cpu.eeprom.size())
        memcpy(cpu.eeprom.data(), d.eeprom.data(), sizeof(cpu.eeprom));

    for(auto const& kv : d.fx_sectors)
    {
        uint32_t sector = kv.first;
        auto const& sdata = kv.second;
        if(sector >= fx.NUM_SECTORS) continue;
        memcpy(&fx.data[sector * 4096], sdata.data(), 4096);
    }
}

void arduboy_t::save_savedata(std::ostream& f)
{
    if(!savedata_dirty) return;
    using StreamAdapter = bitsery::OutputStreamAdapter;
    bitsery::Serializer<StreamAdapter> ar(f);
    savedata.game_hash = game_hash;
    ar(savedata);
    savedata_dirty = false;
}

void arduboy_t::reset()
{
    profiler_reset();
    frame_cpu_usage.clear();
    total_frames = 0;
    cpu.reset();
    display.reset();
    fx.reset();
    paused = false;
    break_step = 0xffffffff;

    if(breakpoints.test(0))
        paused = true;
}

static elf_data_symbol_t const* symbol_for_addr_helper(
    elf_data_t::map_type const& syms, uint16_t addr)
{
    for(auto const& kv : syms)
    {
        auto const& sym = kv.second;
        if(addr >= sym.addr && addr < sym.addr + sym.size)
            return &sym;
    }
    auto it = syms.find(addr);
    if(it != syms.end()) return &it->second;
    return nullptr;
}

elf_data_symbol_t const* arduboy_t::symbol_for_prog_addr(uint16_t addr)
{
    if(!elf) return nullptr;
    return symbol_for_addr_helper(elf->text_symbols, addr);
}

elf_data_symbol_t const* arduboy_t::symbol_for_data_addr(uint16_t addr)
{
    if(!elf) return nullptr;
    return symbol_for_addr_helper(elf->data_symbols, addr);
}

void arduboy_t::profiler_reset()
{
    memset(&profiler_counts, 0, sizeof(profiler_counts));
    memset(&profiler_hotspots, 0, sizeof(profiler_hotspots));
    profiler_hotspots_symbol.clear();
    num_hotspots = 0;
    profiler_total = 0;
    profiler_total_with_sleep = 0;
    prev_profiler_total = 0;
    prev_profiler_total_with_sleep = 0;
    profiler_enabled = false;
    frame_bytes = 0;
}

void arduboy_t::profiler_build_hotspots()
{
    if(!cpu.decoded) return;
    if(cpu.num_instrs <= 0) return;

    // group symbol hotspots
    profiler_hotspots_symbol.clear();
    if(elf)
    {
        for(auto const& kv : elf->text_symbols)
        {
            auto const& sym = kv.second;
            if(sym.size == 0) continue;
            if(sym.weak) continue;
            if(sym.notype) continue;
            if(sym.object) continue;
            hotspot_t h;
            h.begin = cpu.addr_to_disassembled_index(sym.addr);
            h.end = cpu.addr_to_disassembled_index(sym.addr + sym.size - 1);
            h.count = 0;
            for(uint32_t i = sym.addr / 2; i < (sym.addr + sym.size) / 2; ++i)
            {
                if(i >= profiler_counts.size()) break;
                h.count += profiler_counts[i];
            }
            if(h.count == 0) continue;
            profiler_hotspots_symbol.push_back(h);
        }
    }
    std::sort(
        profiler_hotspots_symbol.begin(),
        profiler_hotspots_symbol.end(),
        [](auto const& a, auto const& b) { return a.count > b.count; }
    );

    //
    // WARNING: extremely messy hacky heuristics here
    //

    std::bitset<NUM_INSTRS> starts;
    starts.set(cpu.num_instrs - 1);

    // set starts at beginning of each func symbol
    if(elf)
    {
        for(auto const& kv : elf->text_symbols)
        {
            auto const& sym = kv.second;
            if(sym.object) continue;
            auto i = cpu.addr_to_disassembled_index(sym.addr);
            if(i < starts.size()) starts.set(i);
        }
    }

    num_hotspots = 0;

    // identify hotspot starts
    uint32_t index = 0;
    for(uint32_t index = 0; index < cpu.num_instrs; ++index)
    {
        auto const& d = cpu.disassembled_prog[index];
        auto const& i = cpu.decoded_prog[d.addr / 2];
        
        // don't split on jumps/branches that are never taken
        if(profiler_counts[d.addr / 2] == 0)
            continue;

        bool call = (
            i.func == INSTR_CALL ||
            i.func == INSTR_RCALL ||
            i.func == INSTR_ICALL);
        bool conditional = false;

        if(index > 0)
        {
            auto const& dprev = cpu.disassembled_prog[index - 1];
            auto const& iprev = cpu.decoded_prog[dprev.addr / 2];
            switch(iprev.func)
            {
            case INSTR_SBRS:
            case INSTR_SBRC:
            case INSTR_SBIS:
            case INSTR_SBIC:
            case INSTR_CPSE:
                conditional = true;
                break;
            case INSTR_BRBC:
            case INSTR_BRBS:
                // previous instruction is a branch .+2 or .+4 (skip)
                conditional = (iprev.word == 1 || iprev.word == 2);
                break;
            default:
                break;
            }
        }

        if(i.func == INSTR_BRBS || i.func == INSTR_BRBC)
            conditional = true;

        if((i.func == INSTR_RJMP || i.func == INSTR_JMP) && i.word <= 4)
            conditional = true;

        int size = 1;
        int32_t target = -1;

        switch(i.func)
        {
        case INSTR_JMP:
        case INSTR_CALL:
            size = 2;
            target = i.word;
            break;
        case INSTR_RJMP:
        case INSTR_RCALL:
        case INSTR_BRBS:
        case INSTR_BRBC:
            if(i.func == INSTR_RCALL)
                target = d.addr / 2 + 1 + (int16_t)i.word;
            else
                target = 0;
            break;
        case INSTR_IJMP:
        case INSTR_RET:
        case INSTR_RETI:
            target = 0;
            break;
        default:
            break;
        }

        if(target < 0)
            continue;

        if(conditional)
            continue;

        if(!call)
            starts.set(index + size);
        if(target > 0)
            starts.set(target);
    }

    // now collect hotspots
    for(uint32_t start = 0, index = 1; index < cpu.num_instrs; ++index)
    {
        if(starts.test(index))
        {
            auto& h = profiler_hotspots[num_hotspots++];
            h.begin = start;
            h.end = index - 1;
            h.count = 0;
            for(int32_t i = h.begin; i <= h.end; ++i)
            {
                uint16_t addr = cpu.disassembled_prog[i].addr;
                h.count += profiler_counts[addr / 2];
            }
            if(h.count == 0) --num_hotspots;

            constexpr int LOW_COUNT_NUM = 1;
            constexpr int LOW_COUNT_DENOM = 256;
            
            // trim low-counts from beginning
            for(int32_t i = h.begin; i <= h.end; ++i)
            {
                uint16_t addr = cpu.disassembled_prog[i].addr;
                uint64_t c = h.count * LOW_COUNT_NUM / LOW_COUNT_DENOM;
                if(profiler_counts[addr / 2] <= c)
                {
                    ++h.begin;
                    h.count -= profiler_counts[addr / 2];
                }
                else
                    break;
            }

            // trim low-counts from end
            for(int32_t i = h.end; i >= h.begin; --i)
            {
                uint16_t addr = cpu.disassembled_prog[i].addr;
                uint64_t c = h.count * LOW_COUNT_NUM / LOW_COUNT_DENOM;
                if(profiler_counts[addr / 2] <= c)
                {
                    --h.end;
                    h.count -= profiler_counts[addr / 2];
                }
                else
                    break;
            }

            // trim from middle: N+ consecutive zero-counts
            constexpr int N = 4;
            for(int32_t ns, n = 0, i = h.begin; i <= h.end; ++i)
            {
                uint16_t addr = cpu.disassembled_prog[i].addr;
                if(profiler_counts[addr / 2] == 0)
                    ++n;
                else if(n >= N)
                {
                    auto& hn = profiler_hotspots[num_hotspots++];
                    hn.begin = h.begin;
                    hn.end = ns - 1;
                    hn.count = 0;
                    for(int32_t j = hn.begin; j <= hn.end; ++j)
                    {
                        uint16_t hnaddr = cpu.disassembled_prog[j].addr;
                        hn.count += profiler_counts[hnaddr / 2];
                    }
                    h.count -= hn.count;
                    h.begin = i;
                    n = 0;
                }
                else
                    n = 0;
                if(n == 1) ns = i;
            }

            start = index;
        }
    }

    std::sort(
        profiler_hotspots.begin(),
        profiler_hotspots.begin() + num_hotspots,
        [](auto const& a, auto const& b) { return a.count > b.count; }
    );
}

ABSIM_FORCEINLINE uint32_t arduboy_t::cycle()
{
    assert(cpu.decoded);

    bool vsync = false;
    uint8_t portd = cpu.data[0x2b];
    uint32_t cycles = cpu.advance_cycle();

    // TODO: model SPI connection more precisely?
    // send SPI commands and data to display
    fx.set_enabled((portd & (1 << 1)) == 0);

    if(cpu.spi_done_shifting)
    {
        uint8_t byte = cpu.spi_data_byte;

        // display enabled?
        if(!(portd & (1 << 6)))
        {
            if(portd & (1 << 4))
            {
                if(frame_bytes_total != 0 && ++frame_bytes >= frame_bytes_total)
                {
                    frame_bytes = 0;
                    vsync = true;
                }
                display.send_data(byte);
            }
            else
                display.send_command(byte);
        }

        cpu.spi_datain_byte = fx.spi_transceive(byte);
        cpu.spi_done_shifting = false;
    }

    profiler_total_with_sleep += cycles;
    if(cpu.active || cpu.wakeup_cycles != 0)
    {
        profiler_total += cycles;
        if(profiler_enabled && cpu.executing_instr_pc < profiler_counts.size())
        {
            profiler_counts[cpu.executing_instr_pc] += cycles;
        }
    }

    bool actual_vsync = display.advance(cycles * CYCLE_PS);
    fx.advance(cycles * CYCLE_PS);

    if(frame_bytes_total == 0)
        vsync |= actual_vsync;
    if(vsync)
    {
        // vsync occurred and we are profiling: store frame cpu usage
        uint64_t frame_total = profiler_total - prev_profiler_total;
        uint64_t frame_sleep = profiler_total_with_sleep - prev_profiler_total_with_sleep;
        prev_profiler_total = profiler_total;
        prev_profiler_total_with_sleep = profiler_total_with_sleep;
        double f = frame_sleep ? double(frame_total) / double(frame_sleep) : 0.0;
        frame_cpu_usage.push_back((float)f);
        prev_frame_cycles = frame_sleep;
        ++total_frames;

        // limit memory usage
        if(frame_cpu_usage.size() >= 65536)
        {
            frame_cpu_usage.erase(
                frame_cpu_usage.begin(),
                frame_cpu_usage.begin() + 32768);
        }
    }

    return cycles;
}

void arduboy_t::advance_instr()
{
    if(!cpu.decoded) return;
    int n = 0;
    auto oldpc = cpu.pc;
    cpu.no_merged = true;
    ps_rem = 0;
    do
    {
        paused = false;
        cycle();
        cpu.update_all();
        paused = true;
    } while(++n < 65536 && cpu.pc == oldpc);
}

void arduboy_t::advance(uint64_t ps)
{
    ps += ps_rem;
    ps_rem = 0;

    if(!cpu.decoded) return;
    if(paused) return;

    bool any_breakpoints =
        allow_nonstep_breakpoints && (
        breakpoints.any() ||
        breakpoints_rd.any() ||
        breakpoints_wr.any()) ||
        break_step != 0xffffffff;

    cpu.no_merged = profiler_enabled || any_breakpoints;
    cpu.stack_overflow = false;

    while(ps >= PS_BUFFER)
    {
        uint32_t prev_pc = cpu.pc;

        uint32_t cycles = cycle();

        ps -= cycles * CYCLE_PS;

        if(any_breakpoints)
        {
            if(cpu.pc == break_step || allow_nonstep_breakpoints && (
                cpu.pc < breakpoints.size() && breakpoints.test(cpu.pc) ||
                cpu.just_read < breakpoints_rd.size() && breakpoints_rd.test(cpu.just_read) ||
                cpu.just_written < breakpoints_wr.size() && breakpoints_wr.test(cpu.just_written)))
            {
                paused = true;
                break;
            }
        }

        if(cpu.stack_overflow && cpu.enable_stack_break)
        {
            paused = true;
            break;
        }

    }

    cpu.update_all();

    // track remainder
    if(!paused)
        ps_rem = ps;

    if(!display.enable_filter)
    {
        memcpy(
            display.filtered_pixels.data(),
            display.pixels[0].data(),
            sizeof(display.filtered_pixels));
    }

    // update savedata
    if(cpu.eeprom_dirty)
    {
        savedata.eeprom.resize(cpu.eeprom.size());
        memcpy(savedata.eeprom.data(), cpu.eeprom.data(), sizeof(savedata.eeprom));
        cpu.eeprom_dirty = false;
        savedata_dirty = true;
    }
    if(fx.sectors_dirty)
    {
        for(size_t i = 0; i < fx.sectors_modified.size(); ++i)
        {
            if(!fx.sectors_modified.test(i)) continue;
            auto& s = savedata.fx_sectors[(uint32_t)i];
            memcpy(s.data(), &fx.data[i * 4096], 4096);
        }
        fx.sectors_dirty = false;
        savedata_dirty = true;
    }
}

}