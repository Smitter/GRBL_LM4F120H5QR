/*
  report.c - reporting and messaging methods
  Part of Grbl

  Copyright (c) 2012 Sungeun K. Jeon

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

/*
  This file functions as the primary feedback interface for Grbl. Any outgoing data, such
  as the protocol status messages, feedback messages, and status reports, are stored here.
  For the most part, these functions primarily are called from protocol.c methods. If a
  different style feedback is desired (i.e. JSON), then a user can change these following
  methods to accomodate their needs.
*/

#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "uartstdio.h"

#include "report.h"
#include "print.h"
#include "settings.h"
#include "nuts_bolts.h"
#include "gcode.h"
#include "coolant_control.h"


// Handles the primary confirmation protocol response for streaming interfaces and human-feedback.
// For every incoming line, this method responds with an 'ok' for a successful command or an
// 'error:'  to indicate some error event with the line or some critical system error during
// operation. Errors events can originate from the g-code parser, settings module, or asynchronously
// from a critical error, such as a triggered hard limit. Interface should always monitor for these
// responses.
// NOTE: In silent mode, all error codes are greater than zero.
// TODO: Install silent mode to return only numeric values, primarily for GUIs.
void report_status_message(uint8_t status_code)
{
  if (status_code == 0) { // STATUS_OK
    UARTprintf("ok\r\n");
  } else {
    UARTprintf("error: ");
    switch(status_code) {
      case STATUS_BAD_NUMBER_FORMAT:
      UARTprintf("Bad number format"); break;
      case STATUS_EXPECTED_COMMAND_LETTER:
      UARTprintf("Expected command letter"); break;
      case STATUS_UNSUPPORTED_STATEMENT:
      UARTprintf("Unsupported statement"); break;
      case STATUS_ARC_RADIUS_ERROR:
      UARTprintf("Invalid radius"); break;
      case STATUS_MODAL_GROUP_VIOLATION:
      UARTprintf("Modal group violation"); break;
      case STATUS_INVALID_STATEMENT:
      UARTprintf("Invalid statement"); break;
      case STATUS_SETTING_DISABLED:
      UARTprintf("Setting disabled"); break;
      case STATUS_SETTING_VALUE_NEG:
      UARTprintf("Value < 0.0"); break;
      case STATUS_SETTING_STEP_PULSE_MIN:
      UARTprintf("Value < 3 usec"); break;
      case STATUS_SETTING_READ_FAIL:
      UARTprintf("EEPROM read fail. Using defaults"); break;
      case STATUS_IDLE_ERROR:
      UARTprintf("Busy or queued"); break;
      case STATUS_ALARM_LOCK:
      UARTprintf("Alarm lock"); break;
    }
    UARTprintf("\r\n");
  }
}

// Prints alarm messages.
void report_alarm_message(int8_t alarm_code)
{
  UARTprintf("ALARM: ");
  switch (alarm_code) {
    case ALARM_HARD_LIMIT:
    UARTprintf("Hard limit"); break;
    case ALARM_ABORT_CYCLE:
    UARTprintf("Abort during cycle"); break;
  }
  UARTprintf(". MPos?\r\n");
  delay_ms(500); // Force delay to ensure message clears serial write buffer.
}

// Prints feedback messages. This serves as a centralized method to provide additional
// user feedback for things that are not of the status/alarm message protocol. These are
// messages such as setup warnings, switch toggling, and how to exit alarms.
// NOTE: For interfaces, messages are always placed within brackets. And if silent mode
// is installed, the message number codes are less than zero.
// TODO: Install silence feedback messages option in settings
void report_feedback_message(uint8_t message_code)
{
  UARTprintf("[");
  switch(message_code) {
    case MESSAGE_CRITICAL_EVENT:
    UARTprintf("Reset to continue"); break;
    case MESSAGE_ALARM_LOCK:
    UARTprintf("'$H'|'$X' to unlock"); break;
    case MESSAGE_ALARM_UNLOCK:
    UARTprintf("Caution: Unlocked"); break;
    case MESSAGE_ENABLED:
    UARTprintf("Enabled"); break;
    case MESSAGE_DISABLED:
    UARTprintf("Disabled"); break;
  }
  UARTprintf("]\r\n");
}


// Welcome message
void report_init_message()
{
  UARTprintf("\r\nGrbl " GRBL_VERSION " ['$' for help]\r\n");
}

// Grbl help message
void report_grbl_help() {
/*  UARTprintf("$$ (view Grbl settings)\r\n"
                      "$# (view # parameters)\r\n"
                      "$G (view parser state)\r\n"
                      "$N (view startup blocks)\r\n"
                      "$x=value (save Grbl setting)\r\n"
                      "$Nx=line (save startup block)\r\n"
                      "$C (check gcode mode)\r\n"
                      "$X (kill alarm lock)\r\n"
                      "$H (run homing cycle)\r\n"
                      "~ (cycle start)\r\n"
                      "! (feed hold)\r\n"
                      "? (current status)\r\n"
                      "ctrl-x (reset Grbl)\r\n");*/

  UARTprintf( "$$ (view Grbl settings)\r\n" );
  UARTprintf( "$# (view # parameters)\r\n" );
  UARTprintf( "$G (view parser state)\r\n" );
  UARTprintf( "$N (view startup blocks)\r\n" );
  UARTprintf( "$x=value (save Grbl setting)\r\n" );
  UARTprintf( "$Nx=line (save startup block)\r\n" );
  UARTprintf( "$C (check gcode mode)\r\n" );
  UARTprintf( "$X (kill alarm lock)\r\n" );
  UARTprintf( "$H (run homing cycle)\r\n" );
  UARTprintf( "~ (cycle start)\r\n" );
  UARTprintf( "! (feed hold)\r\n" );
  UARTprintf( "? (current status)\r\n" );
  UARTprintf( "ctrl-x (reset Grbl)\r\n");
}

// Grbl global settings print out.
// NOTE: The numbering scheme here must correlate to storing in settings.c
void report_grbl_settings() {
  UARTprintf("$0="); printFloat(settings.steps_per_mm[X_AXIS]);
  UARTprintf(" (x, step/mm)\r\n$1="); printFloat(settings.steps_per_mm[Y_AXIS]);
  UARTprintf(" (y, step/mm)\r\n$2="); printFloat(settings.steps_per_mm[Z_AXIS]);
  UARTprintf(" (z, step/mm)\r\n$3="); printInteger(settings.pulse_microseconds);
  UARTprintf(" (step pulse, usec)\r\n$4="); printFloat(settings.default_feed_rate);
  UARTprintf(" (default feed, mm/min)\r\n$5="); printFloat(settings.default_seek_rate);
  UARTprintf(" (default seek, mm/min)\r\n$6="); printInteger(settings.invert_mask);
  UARTprintf(" (step port invert mask, int:"); print_uint8_base2(settings.invert_mask);
  UARTprintf(")\r\n$7="); printInteger(settings.stepper_idle_lock_time);
  UARTprintf(" (step idle delay, msec)\r\n$8="); printFloat(settings.acceleration/(60*60)); // Convert from mm/min^2 for human readability
  UARTprintf(" (acceleration, mm/sec^2)\r\n$9="); printFloat(settings.junction_deviation);
  UARTprintf(" (junction deviation, mm)\r\n$10="); printFloat(settings.mm_per_arc_segment);
  UARTprintf(" (arc, mm/segment)\r\n$11="); printInteger(settings.n_arc_correction);
  UARTprintf(" (n-arc correction, int)\r\n$12="); printInteger(settings.decimal_places);
  UARTprintf(" (n-decimals, int)\r\n$13="); printInteger( bit_istrue(settings.flags,BITFLAG_REPORT_INCHES) && 1 );
  UARTprintf(" (report inches, bool)\r\n$14="); printInteger(bit_istrue(settings.flags,BITFLAG_AUTO_START) && 1);
  UARTprintf(" (auto start, bool)\r\n$15="); printInteger(bit_istrue(settings.flags,BITFLAG_INVERT_ST_ENABLE) && 1);
  UARTprintf(" (invert step enable, bool)\r\n$16="); printInteger(bit_istrue(settings.flags,BITFLAG_HARD_LIMIT_ENABLE) && 1);
  UARTprintf(" (hard limits, bool)\r\n$17="); printInteger(bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE) && 1);
  UARTprintf(" (homing cycle, bool)\r\n$18="); printInteger(settings.homing_dir_mask);
  UARTprintf(" (homing dir invert mask, int:"); print_uint8_base2(settings.homing_dir_mask);
  UARTprintf(")\r\n$19="); printFloat(settings.homing_feed_rate);
  UARTprintf(" (homing feed, mm/min)\r\n$20="); printFloat(settings.homing_seek_rate);
  UARTprintf(" (homing seek, mm/min)\r\n$21="); printInteger(settings.homing_debounce_delay);
  UARTprintf(" (homing debounce, msec)\r\n$22="); printFloat(settings.homing_pulloff);
  UARTprintf(" (homing pull-off, mm)\r\n");
}


// Prints gcode coordinate offset parameters
void report_gcode_parameters()
{
  float coord_data[N_AXIS];
  uint8_t coord_select, i;
  for (coord_select = 0; coord_select <= SETTING_INDEX_NCOORD; coord_select++) {
    if (!(settings_read_coord_data(coord_select,coord_data))) {
      report_status_message(STATUS_SETTING_READ_FAIL);
      return;
    }
    UARTprintf("[G");
    switch (coord_select) {
      case 0: UARTprintf("54:"); break;
      case 1: UARTprintf("55:"); break;
      case 2: UARTprintf("56:"); break;
      case 3: UARTprintf("57:"); break;
      case 4: UARTprintf("58:"); break;
      case 5: UARTprintf("59:"); break;
      case 6: UARTprintf("28:"); break;
      case 7: UARTprintf("30:"); break;
      // case 8: UARTprintf("92:")); break; // G92.2, G92.3 not supported. Hence not stored.
    }
    for (i=0; i<N_AXIS; i++) {
      if (bit_istrue(settings.flags,BITFLAG_REPORT_INCHES)) { printFloat(coord_data[i]*INCH_PER_MM); }
      else { printFloat(coord_data[i]); }
      if (i < (N_AXIS-1)) { UARTprintf(","); }
      else { UARTprintf("]\r\n"); }
    }
  }
  UARTprintf("[G92:"); // Print G92,G92.1 which are not persistent in memory
  for (i=0; i<N_AXIS; i++) {
    if (bit_istrue(settings.flags,BITFLAG_REPORT_INCHES)) { printFloat(gc.coord_offset[i]*INCH_PER_MM); }
    else { printFloat(gc.coord_offset[i]); }
    if (i < (N_AXIS-1)) { UARTprintf(","); }
    else { UARTprintf("]\r\n"); }
  }
}


// Print current gcode parser mode state
void report_gcode_modes()
{
  switch (gc.motion_mode) {
    case MOTION_MODE_SEEK : UARTprintf("[G0"); break;
    case MOTION_MODE_LINEAR : UARTprintf("[G1"); break;
    case MOTION_MODE_CW_ARC : UARTprintf("[G2"); break;
    case MOTION_MODE_CCW_ARC : UARTprintf("[G3"); break;
    case MOTION_MODE_CANCEL : UARTprintf("[G80"); break;
  }

  UARTprintf(" G");
  printInteger(gc.coord_select+54);

  if (gc.plane_axis_0 == X_AXIS) {
    if (gc.plane_axis_1 == Y_AXIS) { UARTprintf(" G17"); }
    else { UARTprintf(" G18"); }
  } else { UARTprintf(" G19"); }

  if (gc.inches_mode) { UARTprintf(" G20"); }
  else { UARTprintf(" G21"); }

  if (gc.absolute_mode) { UARTprintf(" G90"); }
  else { UARTprintf(" G91"); }

  if (gc.inverse_feed_rate_mode) { UARTprintf(" G93"); }
  else { UARTprintf(" G94"); }

  switch (gc.program_flow) {
    case PROGRAM_FLOW_RUNNING : UARTprintf(" M0"); break;
    case PROGRAM_FLOW_PAUSED : UARTprintf(" M1"); break;
    case PROGRAM_FLOW_COMPLETED : UARTprintf(" M2"); break;
  }

  switch (gc.spindle_direction) {
    case 1 : UARTprintf(" M3"); break;
    case -1 : UARTprintf(" M4"); break;
    case 0 : UARTprintf(" M5"); break;
  }

  switch (gc.coolant_mode) {
    case COOLANT_DISABLE : UARTprintf(" M9"); break;
    case COOLANT_FLOOD_ENABLE : UARTprintf(" M8"); break;
    #ifdef ENABLE_M7
      case COOLANT_MIST_ENABLE : UARTprintf(" M7"); break;
    #endif
  }

  UARTprintf(" T");
  printInteger(gc.tool);

  UARTprintf(" F");
  if (gc.inches_mode) { printFloat(gc.feed_rate*INCH_PER_MM); }
  else { printFloat(gc.feed_rate); }

  UARTprintf("]\r\n");
}

// Prints specified startup line
void report_startup_line(uint8_t n, char *line)
{
  UARTprintf("$N"); printInteger(n);
  UARTprintf("="); UARTprintf(line);
  UARTprintf("\r\n");
}

 // Prints real-time data. This function grabs a real-time snapshot of the stepper subprogram
 // and the actual location of the CNC machine. Users may change the following function to their
 // specific needs, but the desired real-time data report must be as short as possible. This is
 // requires as it minimizes the computational overhead and allows grbl to keep running smoothly,
 // especially during g-code programs with fast, short line segments and high frequency reports (5-20Hz).
void report_realtime_status()
{
  // **Under construction** Bare-bones status report. Provides real-time machine position relative to
  // the system power on location (0,0,0) and work coordinate position (G54 and G92 applied). Eventually
  // to be added are distance to go on block, processed block id, and feed rate. Also a settings bitmask
  // for a user to select the desired real-time data.
  uint8_t i;
  int32_t current_position[3]; // Copy current state of the system position variable
  memcpy(current_position,sys.position,sizeof(sys.position));
  float print_position[3];

  // Report current machine state
  switch (sys.state) {
    case STATE_IDLE: UARTprintf("<Idle"); break;
//    case STATE_INIT: UARTprintf("[Init")); break; // Never observed
    case STATE_QUEUED: UARTprintf("<Queue"); break;
    case STATE_CYCLE: UARTprintf("<Run"); break;
    case STATE_HOLD: UARTprintf("<Hold"); break;
    case STATE_HOMING: UARTprintf("<Home"); break;
    case STATE_ALARM: UARTprintf("<Alarm"); break;
    case STATE_CHECK_MODE: UARTprintf("<Check"); break;
  }

  // Report machine position
  UARTprintf(",MPos:");
  for (i=0; i<= 2; i++) {
    print_position[i] = current_position[i]/settings.steps_per_mm[i];
    if (bit_istrue(settings.flags,BITFLAG_REPORT_INCHES)) { print_position[i] *= INCH_PER_MM; }
    printFloat(print_position[i]);
    UARTprintf(",");
  }

  // Report work position
  UARTprintf("WPos:");
  for (i=0; i<= 2; i++) {
    if (bit_istrue(settings.flags,BITFLAG_REPORT_INCHES)) {
      print_position[i] -= (gc.coord_system[i]+gc.coord_offset[i])*INCH_PER_MM;
    } else {
      print_position[i] -= gc.coord_system[i]+gc.coord_offset[i];
    }
    printFloat(print_position[i]);
    if (i < 2) { UARTprintf(","); }
  }

  UARTprintf(">\r\n");
}
