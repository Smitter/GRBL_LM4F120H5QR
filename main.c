/*
  main.c - An embedded CNC Controller with rs274/ngc (g-code) support
  Part of Grbl

  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2011-2012 Sungeun K. Jeon

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

/* A big thanks to Alden Hart of Synthetos, supplier of grblshield and TinyG, who has
   been integral throughout the development of the higher level details of Grbl, as well
   as being a consistent sounding board for the future of accessible and free CNC. */

#ifndef UART_BUFFERED
  #define UART_BUFFERED
#endif

#ifndef PART_LM4F120H5QR
  #define PART_LM4F120H5QR
#endif

#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/fpu.h"
#include "utils/uartstdio.h"

#include "config.h"
#include "planner.h"
#include "nuts_bolts.h"
#include "stepper.h"
#include "spindle_control.h"
#include "coolant_control.h"
#include "motion_control.h"
#include "gcode.h"
#include "protocol.h"
#include "limits.h"
#include "report.h"
#include "settings.h"

// Declare system global variable structure
system_t sys;

int main(void)
{
	SysCtlClockSet( SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL | SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN ); //set system clock to 80 MHz
	FPUEnable(); //enable the Floating Point Unit
	FPUStackingEnable(); // Enable stacking for interrupt handlers

  // Enable and Initialize the UART.
  SysCtlPeripheralEnable( SYSCTL_PERIPH_GPIOA );
  GPIOPinConfigure( GPIO_PA0_U0RX );
  GPIOPinConfigure( GPIO_PA1_U0TX );
  GPIOPinTypeUART( GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1 );
  UARTStdioInit( 0 );

  // Initialize system
  settings_init(); // Load grbl settings from EEPROM
  st_init(); // Setup stepper pins and interrupt timers
//  sei(); // Enable interrupts

  memset(&sys, 0, sizeof(sys));  // Clear all system variables
  sys.abort = true;   // Set abort to complete initialization
  sys.state = STATE_INIT;  // Set alarm state to indicate unknown initial position

  for(;;) {

    // Execute system reset upon a system abort, where the main program will return to this loop.
    // Once here, it is safe to re-initialize the system. At startup, the system will automatically
    // reset to finish the initialization process.
    if (sys.abort) {
      // Reset system.
      ///serial_reset_read_buffer(); // Clear serial read buffer
      UARTFlushRx();
      UARTFlushTx( true );

      plan_init(); // Clear block buffer and planner variables
      gc_init(); // Set g-code parser to default state
      protocol_init(); // Clear incoming line data and execute startup lines
      spindle_init();
      coolant_init();
      limits_init();
      st_reset(); // Clear stepper subsystem variables.

      // Sync cleared gcode and planner positions to current system position, which is only
      // cleared upon startup, not a reset/abort.
      sys_sync_current_position();

      // Reset system variables.
      sys.abort = false;
      sys.execute = 0;
      if (bit_istrue(settings.flags,BITFLAG_AUTO_START)) { sys.auto_start = true; }

      // Check for power-up and set system alarm if homing is enabled to force homing cycle
      // by setting Grbl's alarm state. Alarm locks out all g-code commands, including the
      // startup scripts, but allows access to settings and internal commands. Only a homing
      // cycle '$H' or kill alarm locks '$X' will disable the alarm.
      // NOTE: The startup script will run after successful completion of the homing cycle, but
      // not after disabling the alarm locks. Prevents motion startup blocks from crashing into
      // things uncontrollably. Very bad.
      #ifdef HOMING_INIT_LOCK
        if (sys.state == STATE_INIT && bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE)) { sys.state = STATE_ALARM; }
      #endif

      // Check for and report alarm state after a reset, error, or an initial power up.
      if (sys.state == STATE_ALARM) {
        report_feedback_message(MESSAGE_ALARM_LOCK);
      } else {
        // All systems go. Set system to ready and execute startup script.
        sys.state = STATE_IDLE;
        protocol_execute_startup();
      }
    }

    protocol_execute_runtime();
    protocol_process(); // ... process the serial protocol

  }
  ///return 0;   /* never reached */
}
