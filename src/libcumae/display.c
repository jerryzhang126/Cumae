/*
 * MIT License
 *
 * Copyright (c) 2016 Michelangelo De Simone <michel@ngelo.eu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <cumae/base.h>
#include <cumae/display.h>
#include <cumae/config.h>

#include <avr/io.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <avr/pgmspace.h>


#define cm_display_delay()  cm_delay_ms(_cm_dis.stage_time_ms * _cm_dis.tf);

/*
 * Prepare a Frame Data array to be pushed onto the G2.
 */
static void cm_display_prepare_frame_line(cm_byte_t *,
                                             const cm_byte_t *,
                                             cm_byte_t);

/*
 * Stages functions.
 */
static cm_byte_t cm_display_stage_compensate_byte(const cm_byte_t);
static cm_byte_t cm_display_stage_white_byte(const cm_byte_t);
static cm_byte_t cm_display_stage_invert_byte(const cm_byte_t);

static struct cm_display_context_s _cm_dis;

cm_err_t cm_display_init(const struct cm_display_context_s *cd)
{
    if (cd->type != CM_DISPLAY_144)
        return -EUNSUPPORTED;

    memset(&_cm_dis, 0, sizeof(_cm_dis));
    memcpy(&_cm_dis, cd, sizeof(_cm_dis));

    _cm_dis.line_buf = (cm_byte_t *)malloc(_cm_dis.line_buf_len);
    if (!_cm_dis.line_buf)
        return -ENOMEM;

    return ENONE;
}

void cm_display_send_command(cm_byte_t idx, cm_byte_t dat)
{
    PORTB &= ~(1 << PB2);
    cm_spi_w1r1(0x70);
    cm_spi_w1r1(idx);
    PORTB |= 1 << PB2;

    cm_delay_ms(1);

    PORTB &= ~(1 << PB2);
    cm_spi_w1r1(0x72);
    cm_spi_w1r1(dat);
    PORTB |= 1 << PB2;
}

cm_byte_t cm_display_spi_xfer(cm_byte_t *data, size_t len)
{
    PORTB &= ~(1 << PB2);

    size_t biter = 0;
    cm_byte_t tempdata = 0;
    for (; biter < len;  ++biter)
        tempdata = cm_spi_w1r1(data[biter]);

    PORTB |= (1 << PB2);

    return tempdata;
}

inline void cm_display_send_data(cm_byte_t idx,
                                    cm_byte_t *data,
                                    size_t data_len)
{
    PORTB &= ~(1 << PB2);

    cm_spi_w1r1(0x70);
    cm_spi_w1r1(idx);

    PORTB |= (1 << PB2);
    cm_delay_ms(1);
    PORTB &= ~(1 << PB2);

    cm_spi_w1r1(0x72);

    size_t biter;
    for(biter = 0; biter < data_len; ++biter)
        cm_spi_w1r1(data[biter]);

    PORTB |= (1 << PB2);
}

inline cm_byte_t cm_display_spi_read(cm_byte_t c_idx)
{
    PORTB &= ~(1 << PB2);

    cm_spi_w1r1(0x70);
    cm_spi_w1r1(c_idx);

    PORTB |= (1 << PB2);
    cm_delay_ms(1);
    PORTB &= ~(1 << PB2);

    cm_spi_w1r1(0x73);
    cm_byte_t read_data = cm_spi_w1r1(0x00);

    PORTB |= (1 << PB2);

    return read_data;
}

/*
 * TODO: Refactor to be more generic.
 */
void cm_display_power_up(void)
{
    cm_byte_t pseq[] = { 0x71, 0x00 };

    /* BUSY as INPUT */
    DDRB &= ~(1 << PB0);

    /* RSTN as OUTPUT, init to low. */
    DDRD |= 1 << PD7;
    PORTD &= ~(1 << PD7);

    /* Display Vcc/Vdd as OUTPUT, init to low. */
    DDRD |= 1 << PD4;
    PORTD &= ~(1 << PD4);

    /* DCHARGE as OUTPUT, init to low. */
    DDRD |= 1 << PD6;
    PORTD &= ~(1 << PD6);

    /* CS as OUTPUT, init to low. */
    DDRB |= 1 << PB2;
    PORTB &= ~(1  << PB2);

    cm_spi_init();

    cm_delay_ms(6);

    /* Vcc/Vdd up. */
    PORTD |= 1 << PD4;
    cm_delay_ms(11);

    /* RSTN and SS up. */
    PORTD |= 1 << PD7;
    PORTB |= 1 << PB2;
    cm_delay_ms(6);

    /* RSTN low. */
    PORTD &= ~(1 << PD7);
    cm_delay_ms(6);

    /* RSTN up. */
    PORTD |= 1 << PD7;
    cm_delay_ms(6);

    while ((PINB & PB0) == 1) {
        D("Waiting for BUSY...\r\n");
        cm_delay_ms(10);
    }

    cm_delay_ms(6);
    if (cm_display_spi_xfer(pseq, 2) != 0x12) {
        if (_cm_dis.cb && _cm_dis.cb->cm_display_error)
            _cm_dis.cb->cm_display_error(ENODEVICE);

        return;
    }

    /* Disable OE. */
    cm_display_send_command(0x02, 0x40);
    if ((cm_display_spi_read(0x0F) & 0x80) != 0x80) {
        if (_cm_dis.cb && _cm_dis.cb->cm_display_error)
            _cm_dis.cb->cm_display_error(EUNKNOWN);

        return;
    }

    /* Power saving mode. */
    cm_display_send_command(0x0B, 0x02);

    /* Channel data. */
    cm_byte_t cmd_idx_cdata[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0x00 };
    cm_display_send_data(0x01, cmd_idx_cdata, 8);

    /* High power mode. */
    cm_display_send_command(0x07, 0xD1);

    /* Power setting. */
    cm_display_send_command(0x08, 0x02);

    /* Vcom level. */
    cm_display_send_command(0x09, 0xC2);

    /* Power setting. */
    cm_display_send_command(0x04, 0x03);

    /* Driver latch on. */
    cm_display_send_command(0x03, 0x01);

    /* Driver latch off. */
    cm_display_send_command(0x03, 0x00);

    cm_delay_ms(6);

    cm_byte_t charge_pump;
    for(charge_pump = 0; charge_pump < 4; ++charge_pump) {

        /* Start chargepump positive voltage. */
        cm_display_send_command(0x05, 0x01);
        cm_delay_ms(155);

        /* Start chargepump negative voltage. */
        cm_display_send_command(0x05, 0x03);
        cm_delay_ms(95);

        /* Start chargepump Vcom on. */
        cm_display_send_command(0x05, 0x0F);

        cm_delay_ms(45);

        if ((cm_display_spi_read(0x0F) & 0x40) == 0x40) {

            /* Output enable to disable. */
            cm_display_send_command(0x02, 0x06);
            break;
        }

        charge_pump++;
    }

    if (_cm_dis.cb && _cm_dis.cb->cm_display_ready)
        _cm_dis.cb->cm_display_ready();
}

void cm_display_power_off(void)
{
    /*
     * To save bandwidth we write ONE nothing line but
     * scan it all over the 96 lines.
     */
    cm_byte_t line_nothing[] = {
        /* Nothing Data (16 bytes). */
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,

        /* Scan everywhere (24 bytes). */
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,

        /* Nothing Data (16 bytes). */
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,

        0x00 /* Border byte. */
    };

    cm_byte_t border_dummy_line[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0xAA /* Dummy border. */
    };

    /*
     * This writes one "virtual" nothing frame.
     * It sends one line of nothing with all scan bytes active.
     */
    cm_display_send_data(0x0A, line_nothing, 73);
    cm_display_send_command(0x02, 0x07);

    /* Write border dummy line. */
    cm_display_send_data(0x0A, border_dummy_line, 73);
    cm_display_send_command(0x02, 0x07);

    cm_delay_ms(200);

    /* Power saving mode? */
    cm_display_send_command(0x0B, 0x00);

    /* Latch reset turn on. */
    cm_display_send_command(0x03, 0x01);

    /* Power off chargepump. */
    cm_display_send_command(0x05, 0x03);

    /* Power off chargepump negative voltage. */
    cm_display_send_command(0x05, 0x01);

    /* Discharge internal. */
    cm_display_send_command(0x04, 0x80);

    /* Power off chargepump positive voltage. */
    cm_display_send_command(0x05, 0x00);

    /* Turn off oscillator. */
    cm_display_send_command(0x07, 0x01);

    cm_delay_ms(55);

    /*
     * TODO: Turn off SPI here.
     */
}

void cm_display_push_frame_data(const cm_byte_t *f)
{
    assert(f);

    cm_byte_t line_counter;

    for(line_counter = 0; line_counter < _cm_dis.lines; ++line_counter) {

        /* Send line_buffer. */
        cm_display_prepare_frame_line(_cm_dis.line_buf, f, line_counter);
        cm_display_send_data(0x0A, _cm_dis.line_buf, _cm_dis.line_buf_len);
        cm_display_send_command(0x02, 0x07);
    }
}

cm_err_t cm_display_get_default_context(cm_display_type_t t,
                                        struct cm_display_context_s *ctx)
{
    if (t != CM_DISPLAY_144)
        return -EUNSUPPORTED;

    memset(ctx, 0, sizeof(struct cm_display_context_s));

    ctx->type = CM_DISPLAY_144;
    ctx->columns = CM_DISPLAY_144_COLMN;
    ctx->lines = CM_DISPLAY_144_LINES;
    ctx->line_buf_len = CM_DISPLAY_144_LINE_BUFFER_LEN;
    ctx->stage_time_ms = CM_DISPLAY_144_STAGE_TIME;
    ctx->tf = CM_DISPLAY_144_TF;
    ctx->ghost_iter = CM_DISPLAY_144_GHOST_ITERS;

    return ENONE;
}

static void cm_display_prepare_frame_line(cm_byte_t *display_line,
                                             const cm_byte_t *frame_line,
                                             cm_byte_t line_no)
{
    cm_byte_t bc;
    cm_byte_t sync_offset, sync_index;

    /* Prepare line_buffer. */
    memset(display_line, 0, _cm_dis.line_buf_len);
    for (bc = 0; bc < 16; ++bc)
        display_line[bc] = pgm_read_byte(&frame_line[(line_no * 32) + bc]);

    /* Sync byte. */
    sync_offset = 23 - (line_no / 4);
    sync_index = line_no % 4;
    display_line[16 + sync_offset] = 0x3 << (sync_index * 2);

    for (bc = 0; bc < 16; ++bc)
        display_line[40 + bc] = pgm_read_byte(&frame_line[(line_no * 32) + 16 + bc]);
}

static cm_byte_t cm_display_stage_compensate_byte(const cm_byte_t p)
{
    volatile cm_byte_t res = 0;

    if ((p & 0xC0) == 0xC0)
        res |= 0x80;
    if ((p & 0xC0) == 0x80)
        res |= 0xC0;

    if ((p & 0x30) == 0x30)
        res |= 0x20;
    if ((p & 0x30) == 0x20)
        res |= 0x30;

    if ((p & 0xC) == 0xC)
        res |= 0x8;
    if ((p & 0xC) == 0x8)
        res |= 0xC;

    if ((p & 0x3) == 0x3)
        res |= 0x2;
    if ((p & 0x3) == 0x2)
        res |= 0x3;

    return res;
}

static cm_byte_t cm_display_stage_white_byte(const cm_byte_t p)
{
    volatile cm_byte_t res = 0;

    if ((p & 0xC0) == 0x80)
        res |= 0x80;

    if ((p & 0x30) == 0x20)
        res |= 0x20;

    if  ((p & 0xC) == 0x8)
        res |= 0x8;

    if ((p & 0x3) == 0x2)
        res |= 0x2;

    return res;
}

static cm_byte_t cm_display_stage_invert_byte(const cm_byte_t p)
{
    volatile cm_byte_t res = 0;

    if ((p & 0xC0) == 0x80)
        res |= 0xC0;

    if ((p & 0x30) == 0x20)
        res |= 0x30;

    if ((p & 0xC) == 0x8)
        res |= 0xC;

    if ((p & 0x3) == 0x2)
        res |= 0x3;

    return res;
}

void cm_display_stage_update(const cm_byte_t *previous,
                                const cm_byte_t *next)
{
    assert(previous);
    assert(next);

    cm_byte_t line_counter;
    cm_byte_t ghost_iter;
    cm_byte_t b;

    /* Stage 1: Compensate (previous) */
    for (ghost_iter = 0; ghost_iter < _cm_dis.ghost_iter; ++ghost_iter) {
        for(line_counter = 0; line_counter < _cm_dis.lines; ++line_counter) {

            /* Let's prepare the line as usual, before compensating it. */
            cm_display_prepare_frame_line(_cm_dis.line_buf, previous, line_counter);

            /* Compensate Odd and Even */
            for (b = 0; b < 16; b++) {
                _cm_dis.line_buf[b] = cm_display_stage_compensate_byte(_cm_dis.line_buf[b]);
                _cm_dis.line_buf[b + 40] = cm_display_stage_compensate_byte(_cm_dis.line_buf[b + 40]);
            }

            cm_display_send_data(0x0A, _cm_dis.line_buf, _cm_dis.line_buf_len);
            cm_display_send_command(0x02, 0x07);
        }
    }

    cm_display_delay();

    /* Stage 2: White (previous) */
    for (ghost_iter = 0; ghost_iter < _cm_dis.ghost_iter; ++ghost_iter) {
        for(line_counter = 0; line_counter < _cm_dis.lines; ++line_counter) {

            /* Let's prepare the line as usual, before white-ining it. */
            cm_display_prepare_frame_line(_cm_dis.line_buf, previous, line_counter);

            /* White Odd and Even */
            for (b = 0; b < 16; b++) {
                _cm_dis.line_buf[b] = cm_display_stage_white_byte(_cm_dis.line_buf[b]);
                _cm_dis.line_buf[b + 40] = cm_display_stage_white_byte(_cm_dis.line_buf[b + 40]);
            }

            cm_display_send_data(0x0A, _cm_dis.line_buf, _cm_dis.line_buf_len);
            cm_display_send_command(0x02, 0x07);
        }
    }

    cm_display_delay();

    /* Stage 3: Inverse (next) */
    for (ghost_iter = 0; ghost_iter < _cm_dis.ghost_iter; ++ghost_iter) {
        for(line_counter = 0; line_counter < _cm_dis.lines; ++line_counter) {

            /* Let's prepare the line as usual, before inverting it. */
            cm_display_prepare_frame_line(_cm_dis.line_buf, next, line_counter);

            /* Inverse Odd and Even */
            for (b = 0; b < 16; b++) {
                _cm_dis.line_buf[b] = cm_display_stage_invert_byte(_cm_dis.line_buf[b]);
                _cm_dis.line_buf[b + 40] = cm_display_stage_invert_byte(_cm_dis.line_buf[b + 40]);
            }

            cm_display_send_data(0x0A, _cm_dis.line_buf, _cm_dis.line_buf_len);
            cm_display_send_command(0x02, 0x07);
        }
    }

    cm_display_delay();

    /*
     * Stage 4: Normal (next)
     *
     * We rewrite the frame 'ghost_iter' times to totally avoid any ghosting
     * in the final frame.
     *
     * TODO: 'ghost_iter' should be calculated from the actual temperature.
     */
    for (ghost_iter = 0; ghost_iter < _cm_dis.ghost_iter * 2; ++ghost_iter) {
        for(line_counter = 0; line_counter < _cm_dis.lines; ++line_counter) {

            cm_display_prepare_frame_line(_cm_dis.line_buf, next, line_counter);
            cm_display_send_data(0x0A, _cm_dis.line_buf, _cm_dis.line_buf_len);
            cm_display_send_command(0x02, 0x07);
        }
    }

    cm_display_delay();

    if (_cm_dis.cb && _cm_dis.cb->cm_display_stage_updated)
        _cm_dis.cb->cm_display_stage_updated(previous, next);
}
