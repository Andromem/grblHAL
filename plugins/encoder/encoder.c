/*
  encoder.c - quadrature encoder plugin

  Part of GrblHAL

  Copyright (c) 2020 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h>
#include <stdlib.h>

#include "encoder.h"

#if QEI_ENABLE

#ifdef ARDUINO
#include "../grbl/grbl.h"
#include "../grbl/report.h"
#else
#include "grbl/grbl.h"
#include "grbl/report.h"
#endif

#include "../uart.h"

#include <stdio.h>
#include <string.h>

#define MIN(a, b) (((a) > (b)) ? (b) : (a))

#ifndef N_ENCODER
#define N_ENCODER QEI_ENABLE
#endif

typedef bool (*mpg_algo_ptr)(uint_fast16_t state, axes_signals_t axes);

typedef union {
    uint8_t events;
    struct {
        uint8_t position_changed :1,
                zero             :1,
                lock             :1,
                reset            :1,
                scale            :1,
                stop             :1;
    };
} mpg_event_t;

typedef union {
    uint8_t all;
    struct {
        uint8_t moving :1,
                zero             :1,
                lock             :1,
                reset            :1;
    };
} mpg_flags_t;

typedef struct {
    int32_t position;
    mpg_event_t event;
    mpg_flags_t flags;
    uint32_t next_event;
    float pos;
    float scale_factor;
    encoder_t *encoder;
    mpg_algo_ptr handler;
} mpg_t;

static bool mode_chg = false;
static char gcode[50];
static int32_t npos[N_ENCODER] = {0};
static mpg_t mpg[N_AXIS] = {0};
static mpg_event_t mpg_events[N_AXIS] = {0};
static encoder_t *override_encoder = NULL; // NULL when no Encoder_Universal available
static axes_signals_t mpg_event = {0};
static volatile bool mpg_spin_lock = false;
static void (*on_realtime_report)(stream_write_ptr stream_write, report_tracking_flags_t report);

static char *append (char *s)
{
    while(*s)
        s++;

    return s;
}

// MPG encoder movement algorithms
// Bind the one to use to the axis MPGs at end of encoder_init(), later this will be made configurable (per axis?)

static bool mpg_move_absolute (uint_fast16_t state, axes_signals_t axes)
{
    static bool is_moving = false;

    int32_t delta;
    uint32_t velocity = 0;
    uint_fast8_t idx = 0;

    strcpy(gcode, "G1");

    while(axes.mask) {

        if(axes.mask & 0x01) {
            if((delta = mpg[idx].position - npos[mpg[idx].encoder->id]) != 0) {
                float pos_delta = (float)delta * mpg[idx].scale_factor / 100.0f;
                mpg[idx].position = npos[idx];
                velocity = velocity == 0 ? mpg[idx].encoder->velocity : MIN(mpg[idx].encoder->velocity, velocity);
                if(!gc_state.modal.distance_incremental)
                    mpg[idx].pos += pos_delta;
                velocity = velocity == 0 ? mpg[idx].encoder->velocity : MIN(mpg[idx].encoder->velocity, velocity);
                sprintf(append(gcode), "%s%.3f", axis_letter[idx], gc_state.modal.distance_incremental ? pos_delta : mpg[idx].pos);
            }
        }

        idx++;
        axes.mask >>= 1;
    }

    if(strlen(gcode) > 2 && velocity > 0) {

        sprintf(append(gcode), "F%lu", velocity);

        is_moving = grbl.protocol_enqueue_gcode(gcode);
#ifdef UART_DEBUG
serialWriteS(gcode);
serialWriteS(" ");
serialWriteS(uitoa(is_moving));
serialWriteS(" ");
serialWriteS(uitoa(delta));
serialWriteS(ASCII_EOL);
#endif
    }

    return is_moving;
}

static bool mpg_jog_relative (uint_fast16_t state, axes_signals_t axes)
{
    static bool is_moving = false;

    int32_t delta;
    uint32_t velocity = 0;
    uint_fast8_t idx = 0;

    strcpy(gcode, "$J=G91");

//   serialWriteS(uitoa(mpg[idx].encoder->position));
//   serialWriteS(ASCII_EOL);

    while(axes.mask) {

        if(axes.mask & 0x01) {
            if((delta = mpg[idx].position - npos[mpg[idx].encoder->id]) != 0) {
                float pos_delta = (float)delta * mpg[idx].scale_factor / 100.0f;
                mpg[idx].position = npos[idx];
                velocity = velocity == 0 ? mpg[idx].encoder->velocity : MIN(mpg[idx].encoder->velocity, velocity);
                sprintf(append(gcode), "%s%.3f", axis_letter[idx], pos_delta);
            }
        }

        idx++;
        axes.mask >>= 1;
    }

    if(strlen(gcode) > 6 && velocity > 0) {

        sprintf(append(gcode), "F%lu", velocity);

        is_moving = grbl.protocol_enqueue_gcode(gcode);

#ifdef UART_DEBUG
serialWriteS(gcode);
serialWriteS(" ");
serialWriteS(uitoa(is_moving));
serialWriteS(ASCII_EOL);
#endif
/*
serialWriteS(itoa(npos[mpg[idx].encoder->id], gcode, 10));
serialWriteS(ASCII_EOL);
*/
    }

    return is_moving;
}

// End MPG encoder movement algorithms

static inline void reset_override (encoder_mode_t mode)
{
    switch(mode) {

        case Encoder_FeedRate:
            hal.stream.enqueue_realtime_command(CMD_OVERRIDE_FEED_RESET);
            break;

        case Encoder_RapidRate:
            hal.stream.enqueue_realtime_command(CMD_OVERRIDE_RAPID_RESET);
            break;

        case Encoder_Spindle_RPM:
            hal.stream.enqueue_realtime_command(CMD_OVERRIDE_SPINDLE_RESET);
            break;

        default:
            break;
    }
}

void encoder_execute_realtime (uint_fast16_t state)
{
	if(mode_chg && override_encoder) {

		switch(override_encoder->mode) {

			case Encoder_FeedRate:
				hal.stream.write("[MSG:Encoder mode feed rate]" ASCII_EOL);
				break;

			case Encoder_RapidRate:
			    hal.stream.write("[MSG:Encoder mode rapid rate]" ASCII_EOL);
				break;

			case Encoder_Spindle_RPM:
			    hal.stream.write("[MSG:Encoder mode spindle RPM]" ASCII_EOL);
                break;

			default:
				break;
		}

		mode_chg = false;
	}

	if(mpg_event.mask && (state == STATE_IDLE || (state & STATE_JOG))) {

	    bool move_action = false, stop_action = false;
	    uint_fast8_t idx = 0;

	    axes_signals_t event, axes;

#ifdef UART_DEBUG
	    serialWriteS("+");
#endif

	    while(mpg_spin_lock);

	    event.mask = axes.mask = mpg_event.mask;

	    mpg_event.mask = 0;
	    for(idx = 0; idx < N_AXIS; idx++)
	        mpg_events[idx].events = mpg[idx].event.events;

	    idx = 0;

	    while(event.mask) {

	        if(event.mask & 0x01) {

                if(mpg_events[idx].zero) {
                    strcpy(gcode, "G90G10L20P0");
                    strcat(gcode, axis_letter[idx]);
                    strcat(gcode, "0");
                    if(grbl.protocol_enqueue_gcode(gcode)) {
                        mpg[idx].event.zero = Off;
                        mpg[idx].position = npos[mpg[idx].encoder->id] = mpg[idx].encoder->position = 0;
                        hal.encoder_reset(mpg[idx].encoder->id);
                    }
                }

                if(mpg_events[idx].scale) {
                    mpg[idx].scale_factor *= 10.0f;
                    if(mpg[idx].scale_factor > 100.0f)
                        mpg[idx].scale_factor = 1.0f;
#ifdef UART_DEBUG
serialWriteS("Distance scale: ");
serialWriteS(ftoa(mpg[idx].scale_factor, 0));
serialWriteS(ASCII_EOL);
#endif
                }

                if(mpg_events[idx].stop) {
                    if((stop_action = mpg[idx].flags.moving && (state & STATE_JOG))) {
                        hal.stream.enqueue_realtime_command(CMD_JOG_CANCEL);
#ifdef UART_DEBUG
serialWriteS("Jog cancel");
serialWriteS(ASCII_EOL);
#endif
                    }
                    mpg[idx].flags.moving = mpg_events[idx].position_changed = Off;
                }

                if(mpg_events[idx].position_changed) {

                    if(!mpg[idx].flags.moving) {
                        float target[N_AXIS];
                        system_convert_array_steps_to_mpos(target, sys_position);
                        mpg[idx].flags.moving = On;
                        mpg[idx].pos = target[idx] - gc_get_offset(idx);
                    }

                    move_action = true;

                    mpg[idx].flags.moving = On;
                    mpg[idx].next_event += 100;
                }
	        }

	        mpg[idx].event.events = 0;

	        idx++;
	        event.mask >>= 1;
	    }

	    if(move_action && !mpg[0].handler(state, axes))
	        mpg_event.mask |= 0; //axes.mask; // gcode was rejected, restore events
	}
}

void encoder_event (encoder_t *encoder, int32_t position)
{
    bool update_position = false;

    if(encoder->event.click) {

        if(encoder->settings->mode == Encoder_Universal) {
            mode_chg = true;
            sys.report.encoder = On;
            encoder->event.click = Off;
            encoder->mode = encoder->mode == Encoder_FeedRate ? Encoder_RapidRate : (encoder->mode == Encoder_RapidRate ? Encoder_Spindle_RPM : Encoder_FeedRate);
        } else if(encoder->settings->mode == Encoder_MPG) {
            if(++encoder->axis == N_AXIS)
                encoder->axis = X_AXIS;
            mpg[encoder->axis].position = npos[encoder->id] = encoder->position = 0;
            mpg[encoder->axis].event.events = encoder->event.events = 0;
            hal.encoder_reset(encoder->id);
        }
    }

	if(encoder->event.position_changed) {

#ifdef UART_DEBUG
	    itoa(position, gcode, 10);
        serialWriteS("Pos: ");
	    serialWriteS(gcode);
        serialWriteS(ASCII_EOL);
#endif

	    int32_t n_count = (position * 100L) / (int32_t)encoder->settings->cpr;

	    encoder->event.position_changed = Off;

        if(n_count != npos[encoder->id] || encoder->velocity == 0) switch(encoder->mode) {

            case Encoder_FeedRate:
                update_position = true;
                if(n_count < npos[encoder->id]) {
                    while(npos[encoder->id]-- != n_count)
                        hal.stream.enqueue_realtime_command(CMD_OVERRIDE_FEED_FINE_MINUS);
                } else {
                    while(npos[encoder->id]++ != n_count)
                        hal.stream.enqueue_realtime_command(CMD_OVERRIDE_FEED_FINE_PLUS);
                }
                break;

            case Encoder_RapidRate:
                update_position = abs(position - encoder->position) >= encoder->settings->cpd;

                if(update_position) switch(sys.override.rapid_rate) {

                    case DEFAULT_RAPID_OVERRIDE:
                        if(position < encoder->position)
                            hal.stream.enqueue_realtime_command(CMD_OVERRIDE_RAPID_MEDIUM);
                        break;

                    case RAPID_OVERRIDE_MEDIUM:
                        if(position < encoder->position)
                            hal.stream.enqueue_realtime_command(CMD_OVERRIDE_RAPID_LOW);
                        else
                            hal.stream.enqueue_realtime_command(CMD_OVERRIDE_RAPID_RESET);
                        break;

                    case RAPID_OVERRIDE_LOW:
                        if(position > encoder->position)
                            hal.stream.enqueue_realtime_command(CMD_OVERRIDE_RAPID_MEDIUM);
                        break;

                    default:
                        break;
                }
                break;

            case Encoder_Spindle_RPM:
                update_position = true;
                if(n_count < npos[encoder->id]) {
                    while(npos[encoder->id]-- != n_count)
                        hal.stream.enqueue_realtime_command(CMD_OVERRIDE_SPINDLE_FINE_MINUS);
                } else {
                    while(npos[encoder->id]++ != n_count)
                        hal.stream.enqueue_realtime_command(CMD_OVERRIDE_SPINDLE_FINE_PLUS);
                }
                break;

            case Encoder_MPG:
            case Encoder_MPG_X:
            case Encoder_MPG_Y:
            case Encoder_MPG_Z:
#if N_AXIS > 3
            case Encoder_MPG_A:
#endif
#if N_AXIS > 4
            case Encoder_MPG_B:
#endif
#if N_AXIS > 5
            case Encoder_MPG_C:
#endif
                update_position = true;

                mpg_spin_lock = true;
                if(mpg[encoder->axis].encoder->velocity == 0) {
                    mpg[encoder->axis].event.stop = On; // mpg[encoder->axis].flags.moving;
                    mpg_event.mask |= (1 << encoder->axis);
                } else {
                    mpg[encoder->axis].event.position_changed = On;
                    mpg_event.mask |= (1 << encoder->axis);
                }
                mpg_spin_lock = false;
                break;

            default:
                break;
        }

        if(update_position) {
            encoder->position = position;
            npos[encoder->id] = n_count;
        }
	}

    if(encoder->event.events) switch(encoder->mode) {

        case Encoder_FeedRate:
        case Encoder_RapidRate:
        case Encoder_Spindle_RPM:
            npos[encoder->id] = encoder->position = 0;
            hal.encoder_reset(encoder->id);
            reset_override(encoder->mode);
            break;

        case Encoder_MPG:
        case Encoder_MPG_X:
        case Encoder_MPG_Y:
        case Encoder_MPG_Z:
#if N_AXIS > 3
        case Encoder_MPG_A:
#endif
#if N_AXIS > 4
        case Encoder_MPG_B:
#endif
#if N_AXIS > 5
        case Encoder_MPG_C:
#endif

            mpg_spin_lock = true;
            if(encoder->event.click) {;
                mpg[encoder->axis].event.scale = On;
                mpg_event.mask |= (1 << encoder->axis);
            }
            if(encoder->event.dbl_click) {
                mpg[encoder->axis].event.zero = On;
                mpg_event.mask |= (1 << encoder->axis);
            }
            mpg_spin_lock = false;
            break;

        default:
            break;
    }

    encoder->event.events = 0;
}

void encoder_rt_report(stream_write_ptr stream_write, report_tracking_flags_t report)
{
    if(override_encoder && report.encoder) {
        stream_write("|Enc:");
        stream_write(uitoa(override_encoder->mode));
    }

    if(on_realtime_report)
        on_realtime_report(stream_write, report);
}

status_code_t encoder_setting (setting_type_t setting, float value, char *svalue)
{
    status_code_t status = Status_Unhandled;

    if (setting >= Setting_EncoderSettingsBase && setting <= Setting_EncoderSettingsMax) {

        // Store encoder configuration. Encoder numbering sequence set by N_ENCODER define.

        uint_fast16_t base_idx = (uint_fast16_t)setting - (uint_fast16_t)Setting_EncoderSettingsBase;
        uint_fast8_t setting_idx = base_idx % ENCODER_SETTINGS_INCREMENT;
        uint_fast8_t encoder_idx = (base_idx - setting_idx) / ENCODER_SETTINGS_INCREMENT;

        if(encoder_idx < N_ENCODER) switch(setting_idx) {

            case Setting_EncoderMode:
                if(isintf(value) && value != NAN && value >= (float)Encoder_Universal && value < (float)Encoder_Spindle_Position) {
                    driver_settings.encoder[encoder_idx].mode = (encoder_mode_t)value;
                    status = Status_OK;
                } else
                    status = Status_InvalidStatement;
                break;

            case Setting_EncoderCPR:
                driver_settings.encoder[encoder_idx].cpr = (uint32_t)value;
                status = Status_OK;
                break;

            case Setting_EncoderCPD:
                driver_settings.encoder[encoder_idx].cpd = (uint32_t)value;
                status = Status_OK;
                break;

            case Setting_EncoderDblClickWindow:
                if(isintf(value) && value != NAN && value >= 100.0f && value <= 900.0f) {
                    driver_settings.encoder[encoder_idx].dbl_click_window = (uint32_t)value;
                    status = Status_OK;
                } else
                    status = Status_InvalidStatement;
                break;

            default:
                break;
        }
    }

    return status;
}

void encoder_settings_report (setting_type_t setting)
{
    if (setting >= Setting_EncoderSettingsBase && setting <= Setting_EncoderSettingsMax) {

        // Report encoder configuration. Encoder numbering sequence set by N_ENCODER define.

        uint_fast16_t base_idx = (uint_fast16_t)setting - (uint_fast16_t)Setting_EncoderSettingsBase;
        uint_fast8_t setting_idx = base_idx % ENCODER_SETTINGS_INCREMENT;
        uint_fast8_t encoder_idx = (base_idx - setting_idx) / ENCODER_SETTINGS_INCREMENT;

        if(encoder_idx < N_ENCODER) switch(setting_idx) {

            case Setting_EncoderMode:
                report_uint_setting(setting, (uint32_t)driver_settings.encoder[encoder_idx].mode);
                break;

            case Setting_EncoderCPR:
                report_uint_setting(setting, driver_settings.encoder[encoder_idx].cpr);
                break;

            case Setting_EncoderCPD:
                report_uint_setting(setting, driver_settings.encoder[encoder_idx].cpd);
                break;

            case Setting_EncoderDblClickWindow:
                report_uint_setting(setting, driver_settings.encoder[encoder_idx].dbl_click_window);
                break;

            default:
                break;
        }
    }
}

void encoder_settings_restore (void)
{
    uint_fast8_t idx;

    for(idx = 0; idx < N_ENCODER; idx++) {
        driver_settings.encoder[idx].mode = Encoder_Universal;
        driver_settings.encoder[idx].cpr = 400;
        driver_settings.encoder[idx].cpd = 4;
        driver_settings.encoder[idx].dbl_click_window = 500; // ms
    }
}

void encoder_init (encoder_t *encoder)
{
    uint_fast8_t idx;

    override_encoder = NULL;

#if COMPATIBILITY_LEVEL <= 1
    on_realtime_report = grbl.on_realtime_report;
    grbl.on_realtime_report = encoder_rt_report;
#endif

    for(idx = 0; idx < N_ENCODER; idx++) {

        encoder[idx].id = idx;
        encoder[idx].axis = 0xFF;
        encoder[idx].mode = driver_settings.encoder[idx].mode;
        encoder[idx].settings = &driver_settings.encoder[idx];

        switch(encoder[idx].settings->mode) {

            case Encoder_Universal:
                encoder[idx].mode = Encoder_FeedRate;
                override_encoder = &encoder[idx];
                break;

            case Encoder_MPG:
                {
                    uint_fast8_t i;
                    encoder[idx].axis = X_AXIS;
                    for(i = 0; i < N_AXIS; i++)
                        mpg[i].encoder = &encoder[idx];
                }
                break;

            case Encoder_MPG_X:
                encoder[idx].axis   = X_AXIS;
                mpg[X_AXIS].encoder = &encoder[idx];
                break;

            case Encoder_MPG_Y:
                encoder[idx].axis   = Y_AXIS;
                mpg[Y_AXIS].encoder = &encoder[idx];
                break;

            case Encoder_MPG_Z:
                encoder[idx].axis   = Z_AXIS;
                mpg[Z_AXIS].encoder = &encoder[idx];
                break;
#if N_AXIS > 3
            case Encoder_MPG_A:
                encoder[idx].axis   = A_AXIS;
                mpg[A_AXIS].encoder = &encoder[idx];
                break;
#endif
#if N_AXIS > 4
            case Encoder_MPG_B:
                encoder[idx].axis   = B_AXIS;
                mpg[B_AXIS].encoder = &encoder[idx];
                break;
#endif
#if N_AXIS > 5
            case Encoder_MPG_C:
                encoder[idx].axis   = C_AXIS;
                mpg[C_AXIS].encoder = &encoder[idx];
                break;
#endif
            default:
                break;
        }

        hal.encoder_reset(idx);
    }

    for(idx = 0; idx < N_AXIS; idx++) {
        mpg[idx].scale_factor = 1.0f;
//        mpg[idx].handler = mpg_move_absolute;
        mpg[idx].handler = mpg_jog_relative;
    }
}

#endif
