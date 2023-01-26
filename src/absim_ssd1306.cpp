#include "absim.hpp"

#include <cmath>

namespace absim
{

void ssd1306_t::send_command(uint8_t byte)
{
    if(!processing_command)
    {
        command_byte_index = 0;
        current_command = byte;
        processing_command = true;
    }
    switch(current_command)
    {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        page_col_start &= 0xf0;
        page_col_start |= current_command;
        processing_command = false;
        break;

    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        page_col_start &= 0x0f;
        page_col_start |= current_command << 4;
        processing_command = false;
        break;

    case 0x81:
        if(command_byte_index == 1)
        {
            contrast = byte;
            processing_command = false;
        }
        break;

    case 0xa4:
        entire_display_on = true;
        processing_command = false;
        break;

    case 0xa5:
        entire_display_on = false;
        processing_command = false;
        break;

    case 0xa6:
        inverse_display = true;
        processing_command = false;
        break;

    case 0xa7:
        inverse_display = false;
        processing_command = false;
        break;

    case 0xae:
        display_on = false;
        processing_command = false;
        break;

    case 0xaf:
        display_on = true;
        processing_command = false;
        break;

    case 0x20:
        if(command_byte_index == 1)
        {
            switch(byte & 0x3)
            {
            case 0: addressing_mode = addr_mode::HORIZONTAL; break;
            case 1: addressing_mode = addr_mode::VERTICAL;   break;
            case 2: addressing_mode = addr_mode::PAGE;       break;
            case 3:                                          break;
            default: break;
            }
            processing_command = false;
        }
        break;

    case 0x21:
        if(command_byte_index == 1)
        {
            col_start = byte & 0x7f;
        }
        if(command_byte_index == 2)
        {
            col_end = byte & 0x7f;
            processing_command = false;
        }
        break;

    case 0x22:
        if(command_byte_index == 1)
        {
            page_start = byte & 0x7;
        }
        if(command_byte_index == 2)
        {
            page_end = byte & 0x7;
            processing_command = false;
        }
        break;

    case 0xb0: case 0xb1 : case 0xb2 : case 0xb3:
    case 0xb4: case 0xb5 : case 0xb6 : case 0xb7:
        page_page_start = byte & 0x7;
        processing_command = false;
        break;

    case 0xa0:
        segment_remap = false;
        processing_command = false;
        break;

    case 0xa1:
        segment_remap = true;
        processing_command = false;
        break;

    case 0xa8:
        if(command_byte_index == 1)
        {
            mux_ratio = byte & 0x3f;
            processing_command = false;
        }
        break;

    case 0xc0:
        com_scan_direction = false;
        processing_command = false;
        break;

    case 0xc8:
        com_scan_direction = true;
        processing_command = false;
        break;

    case 0xd3:
        if(command_byte_index == 1)
        {
            display_offset = byte & 0x3f;
            processing_command = false;
        }
        break;

    case 0xda:
        if(command_byte_index == 1)
        {
            alternative_com = (byte & 0x10) != 0;
            com_remap = (byte & 0x20) != 0;
            processing_command = false;
        }
        break;



    default:

        if(current_command >= 0x40 && current_command < 0x80)
        {
            display_start = current_command & 0x3f;
            processing_command = false;
            break;
        }

        processing_command = false;
        break;
    }
    ++command_byte_index;
}

void ssd1306_t::send_data(uint8_t byte)
{
    size_t i = data_page * 128 + data_col;
    ram[i] = byte;

    switch(addressing_mode)
    {
    case addr_mode::HORIZONTAL:
        if(data_col >= col_end)
        {
            data_col = col_start;
            if(data_page >= page_end)
                data_page = page_start;
            else
                data_page = (data_page + 1) & 0x7;
        }
        else
            data_col = (data_col + 1) & 0x7f;
        break;

    case addr_mode::VERTICAL:
        break;
        
    case addr_mode::PAGE:
        break;

    default:
        break;
    }
}

void ssd1306_t::update_pixels_row()
{
    uint8_t mask = 1 << (row % 8);
    size_t index = row * 128;
    size_t rindex = (row / 8) * 128;
    for(int i = 0; i < 128; ++i, ++index, ++rindex)
    {
        auto p = pixels[index];
        constexpr double F = 0.5;
        p *= (1.0 - F);
        if(ram[rindex] & mask)
            p += F;
        pixels[index] = p;
    }
}

void ssd1306_t::advance(uint64_t ps)
{
    ps += ps_rem;
    ps_rem = 0;

    while(ps >= ps_per_clk)
    {
        if(++row_cycle >= cycles_per_row)
        {
            update_pixels_row();
            row_cycle = 0;
            if(row == mux_ratio)
                row = 0;
            else
                row = (row + 1) % 64;
        }
        ps -= ps_per_clk;
    }

    ps_rem = ps;
}

constexpr std::array<double, 16> FOSC =
{
    // mostly made up
    175.00, 199.38, 223.75, 248.12, 272.50, 296.88, 321.25, 345.62,
    370.00, 394.29, 418.57, 442.86, 467.14, 491.43, 515.71, 540.00,
};

void ssd1306_t::update_internals()
{
    cycles_per_row = phase_1 + phase_2 + 50;
    ps_per_clk = (uint64_t)std::round(1e12 / fosc());
}

void ssd1306_t::reset()
{
    memset(&ram, 0, sizeof(ram));
    memset(&pixels, 0, sizeof(pixels));

    contrast = 0x7f;
    entire_display_on = false;
    inverse_display = false;
    display_on = false;

    addressing_mode = addr_mode::PAGE;

    col_start = 0;
    col_end = 127;
    page_start = 0;
    page_end = 7;

    page_col_start = 0;
    page_page_start = 0;

    mux_ratio = 63;

    display_offset = 0;
    display_start = 0;

    com_scan_direction = false;
    alternative_com = true;
    com_remap = false;
    segment_remap = false;

    fosc_index = 8;
    divide_ratio = 0;
    phase_1 = 2;
    phase_2 = 2;
    vcomh_deselect = 2;

    update_internals();
}

double ssd1306_t::fosc() const
{
    return FOSC[fosc_index % 16] * 1000.0;
}

double ssd1306_t::refresh_rate() const
{
    int D = divide_ratio + 1;
    int K = phase_1 + phase_2 + 50;
    int MUX = mux_ratio + 1;
    return fosc() / double(D * K * MUX);
}

}
