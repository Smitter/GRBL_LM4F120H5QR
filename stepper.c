/*
  stepper.c - stepper motor driver: executes motion plans using stepper motors
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

/* The timer calculations of this module informed by the 'RepRap cartesian firmware' by Zack Smith
   and Philipp Tiefenbacher. */

///#include <avr/interrupt.h>
//#include "inc/hw_gpio.h"
#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "driverlib/interrupt.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "driverlib/gpio.h"

#include "stepper.h"
#include "config.h"
#include "settings.h"
#include "planner.h"

// Some useful constants
#define TICKS_PER_MICROSECOND (F_CPU/1000000) ///16 on avr, 80 on arm
///#define CYCLES_PER_ACCELERATION_TICK (F_CPU/ACCELERATION_TICKS_PER_SECOND)
#define CYCLES_PER_ACCELERATION_TICK ((TICKS_PER_MICROSECOND*1000000)/ACCELERATION_TICKS_PER_SECOND) ///320000 on AVR, same on ARM

// Stepper state variable. Contains running data and trapezoid variables.
typedef struct {
  // Used by the bresenham line algorithm
  int32_t counter_x,        // Counter variables for the bresenham line tracer
          counter_y,
          counter_z;
  uint32_t event_count;
  uint32_t step_events_completed;  // The number of step events left in current motion

  // Used by the trapezoid generator
  uint32_t cycles_per_step_event;        // The number of machine cycles between each step event
  uint32_t trapezoid_tick_cycle_counter; // The cycles since last trapezoid_tick. Used to generate ticks at a steady
                                              // pace without allocating a separate timer
  uint32_t trapezoid_adjusted_rate;      // The current rate of step_events according to the trapezoid generator
  uint32_t min_safe_rate;  // Minimum safe rate for full deceleration rate reduction step. Otherwise halves step_rate.
} stepper_t;

static stepper_t st;
static block_t *current_block;  // A pointer to the block currently being traced

// Used by the stepper driver interrupt
///static uint8_t step_pulse_time; // Step pulse reset time after step rise
static uint32_t step_pulse_time; // Step pulse reset time after step rise
static uint32_t out_bits;        // The next stepping-bits to be output
static volatile uint8_t busy;   // True when SIG_OUTPUT_COMPARE1A is being serviced. Used to avoid retriggering that handler.

#if STEP_PULSE_DELAY > 0
  static uint8_t step_bits;  // Stores out_bits output to complete the step pulse delay
#endif

//         __________________________
//        /|                        |\     _________________         ^
//       / |                        | \   /|               |\        |
//      /  |                        |  \ / |               | \       s
//     /   |                        |   |  |               |  \      p
//    /    |                        |   |  |               |   \     e
//   +-----+------------------------+---+--+---------------+----+    e
//   |               BLOCK 1            |      BLOCK 2          |    d
//
//                           time ----->
//
//  The trapezoid is the shape the speed curve over time. It starts at block->initial_rate, accelerates by block->rate_delta
//  during the first block->accelerate_until step_events_completed, then keeps going at constant speed until
//  step_events_completed reaches block->decelerate_after after which it decelerates until the trapezoid generator is reset.
//  The slope of acceleration is always +/- block->rate_delta and is applied at a constant rate following the midpoint rule
//  by the trapezoid generator, which is called ACCELERATION_TICKS_PER_SECOND times per second.

static void set_step_events_per_minute(uint32_t steps_per_minute);

// Stepper state initialization. Cycle should only start if the st.cycle_start flag is
// enabled. Startup init and limits call this function but shouldn't start the cycle.
void st_wake_up()
{
  // Enable steppers by resetting the stepper disable port
  if (bit_istrue(settings.flags,BITFLAG_INVERT_ST_ENABLE)) {
///    STEPPERS_DISABLE_PORT |= (1<<STEPPERS_DISABLE_BIT);
    GPIOPinWrite( STEPPERS_DISABLE_PORT, STEPPERS_DISABLE_BIT, 0xFF );
  } else {
///    STEPPERS_DISABLE_PORT &= ~(1<<STEPPERS_DISABLE_BIT);
    GPIOPinWrite( STEPPERS_DISABLE_PORT, STEPPERS_DISABLE_BIT, 0x00 );
  }
  if (sys.state == STATE_CYCLE) {
    // Initialize stepper output bits
    out_bits = (0) ^ (settings.invert_mask);
    // Initialize step pulse timing from settings. Here to ensure updating after re-writing.
    #ifdef STEP_PULSE_DELAY
      // Set total step pulse time after direction pin set. Ad hoc computation from oscilloscope.
      step_pulse_time = -(((settings.pulse_microseconds+STEP_PULSE_DELAY-2)*TICKS_PER_MICROSECOND) >> 3);
      // Set delay between direction pin write and step command.
///todo      OCR2A = -(((settings.pulse_microseconds)*TICKS_PER_MICROSECOND) >> 3);
///      TimerLoadSet( TIMER0_BASE, TIMER_B, (((settings.pulse_microseconds)*TICKS_PER_MICROSECOND) >> 3) );
    #else // Normal operation
      // Set step pulse time. Ad hoc computation from oscilloscope. Uses two's complement.
      ///step_pulse_time = -(((settings.pulse_microseconds-2)*TICKS_PER_MICROSECOND) >> 3);
      step_pulse_time = ( settings.pulse_microseconds - 2 ) * TICKS_PER_MICROSECOND;
    #endif
    // Enable stepper driver interrupt
    ///TIMSK1 |= (1<<OCIE1A);
    TimerLoadSet( TIMER2_BASE, TIMER_A, step_pulse_time );
    TimerEnable( TIMER1_BASE, TIMER_A );
  }
}

// Stepper shutdown
void st_go_idle()
{
  // Disable stepper driver interrupt
  ///TIMSK1 &= ~(1<<OCIE1A); ///Disable bit 'Timer/Counter1, Output Compare A Match Interrupt Enable' in interrupt mask register
  /// If we disable interrupt, the timer will continue to work. When you will switch on it again, what value will it have?
  ///Maybe it is better to disconnect the clock source?
  TimerDisable( TIMER1_BASE, TIMER_A );
  /// No function to write value into the timer, though the timer supports this! Texas Instruments, are you crazy?
///todo  HWREG( TIMER0_BASE + 0x0050 ) = (uint32_t) 0;
  // Disable steppers only upon system alarm activated or by user setting to not be kept enabled.
  if ((settings.stepper_idle_lock_time != 0xff) || bit_istrue(sys.execute,EXEC_ALARM)) {
    // Force stepper dwell to lock axes for a defined amount of time to ensure the axes come to a complete
    // stop and not drift from residual inertial forces at the end of the last movement.
    delay_ms(settings.stepper_idle_lock_time);
    if (bit_istrue(settings.flags,BITFLAG_INVERT_ST_ENABLE)) {
///      STEPPERS_DISABLE_PORT &= ~(1<<STEPPERS_DISABLE_BIT);
      GPIOPinWrite( STEPPERS_DISABLE_PORT, STEPPERS_DISABLE_BIT, 0 );
    } else {
///      STEPPERS_DISABLE_PORT |= (1<<STEPPERS_DISABLE_BIT);
      GPIOPinWrite( STEPPERS_DISABLE_PORT, STEPPERS_DISABLE_BIT, 0xFF );
    }
  }
}

// This function determines an acceleration velocity change every CYCLES_PER_ACCELERATION_TICK by
// keeping track of the number of elapsed cycles during a de/ac-celeration. The code assumes that
// step_events occur significantly more often than the acceleration velocity iterations.
inline static uint8_t iterate_trapezoid_cycle_counter()
{
  st.trapezoid_tick_cycle_counter += st.cycles_per_step_event;
  if(st.trapezoid_tick_cycle_counter > CYCLES_PER_ACCELERATION_TICK) {
    st.trapezoid_tick_cycle_counter -= CYCLES_PER_ACCELERATION_TICK;
    return(true);
  } else {
    return(false);
  }
}

// "The Stepper Driver Interrupt" - This timer interrupt is the workhorse of Grbl. It is executed at the rate set with
// config_step_timer. It pops blocks from the block_buffer and executes them by pulsing the stepper pins appropriately.
// It is supported by The Stepper Port Reset Interrupt which it uses to reset the stepper port after each pulse.
// The bresenham line tracer algorithm controls all three stepper outputs simultaneously with these two interrupts.
///ISR(TIMER1_COMPA_vect)
void timer1_compare_interrupt( void )
{
  TimerIntClear( TIMER1_BASE, TIMER_TIMA_TIMEOUT ); /// clear interrupt flag

  if (busy) { return; } // The busy-flag is used to avoid reentering this interrupt

  // Set the direction pins a couple of nanoseconds before we step the steppers
  ///STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (out_bits & DIRECTION_MASK);
  GPIOPinWrite( STEPPING_PORT, DIRECTION_MASK, out_bits );
  // Then pulse the stepping pins
  #ifdef STEP_PULSE_DELAY
    step_bits = (STEPPING_PORT & ~STEP_MASK) | out_bits; // Store out_bits to prevent overwriting.
  #else  // Normal operation
///    STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | out_bits;
    GPIOPinWrite( STEPPING_PORT, STEP_MASK, out_bits );
  #endif
  // Enable step pulse reset timer so that The Stepper Port Reset Interrupt can reset the signal after
  // exactly 'settings.pulse_microseconds' microseconds, independent of the main Timer1 prescaler.
  ///TCNT2 = step_pulse_time; // Reload timer counter
  ///         HWREG( TIMER0_BASE + 0x54 ) = step_pulse_time;
  ///TCCR2B = (1<<CS21); // Begin timer2. Full speed, 1/8 prescaler
  ///TimerPrescaleSet( TIMER0_BASE, TIMER_B, 8 );
//  TimerLoadSet( TIMER0_BASE, TIMER_B, step_pulse_time );
  TimerEnable( TIMER2_BASE, TIMER_A );

  busy = true;
  // Re-enable interrupts to allow ISR_TIMER2_OVERFLOW to trigger on-time and allow serial communications
  // regardless of time in this handler. The following code prepares the stepper driver for the next
  // step interrupt compare and will always finish before returning to the main program.
  ///sei();
///  IntMasterEnable();

  // If there is no current block, attempt to pop one from the buffer
  if (current_block == NULL) {
    // Anything in the buffer? If so, initialize next motion.
    current_block = plan_get_current_block();
    if (current_block != NULL) {
      if (sys.state == STATE_CYCLE) {
        // During feed hold, do not update rate and trap counter. Keep decelerating.
        st.trapezoid_adjusted_rate = current_block->initial_rate;
        set_step_events_per_minute(st.trapezoid_adjusted_rate); // Initialize cycles_per_step_event
        st.trapezoid_tick_cycle_counter = CYCLES_PER_ACCELERATION_TICK/2; // Start halfway for midpoint rule.
      }
      st.min_safe_rate = current_block->rate_delta + (current_block->rate_delta >> 1); // 1.5 x rate_delta
      st.counter_x = -(current_block->step_event_count >> 1);
      st.counter_y = st.counter_x;
      st.counter_z = st.counter_x;
      st.event_count = current_block->step_event_count;
      st.step_events_completed = 0;
    } else {
      st_go_idle();
      bit_true(sys.execute,EXEC_CYCLE_STOP); // Flag main program for cycle end
    }
  }

  if (current_block != NULL) {
    // Execute step displacement profile by bresenham line algorithm
    out_bits = current_block->direction_bits;
    st.counter_x += current_block->steps_x;
    if (st.counter_x > 0) {
      out_bits |= (1<<X_STEP_BIT);
      st.counter_x -= st.event_count;
      if (out_bits & (1<<X_DIRECTION_BIT)) { sys.position[X_AXIS]--; }
      else { sys.position[X_AXIS]++; }
    }
    st.counter_y += current_block->steps_y;
    if (st.counter_y > 0) {
      out_bits |= (1<<Y_STEP_BIT);
      st.counter_y -= st.event_count;
      if (out_bits & (1<<Y_DIRECTION_BIT)) { sys.position[Y_AXIS]--; }
      else { sys.position[Y_AXIS]++; }
    }
    st.counter_z += current_block->steps_z;
    if (st.counter_z > 0) {
      out_bits |= (1<<Z_STEP_BIT);
      st.counter_z -= st.event_count;
      if (out_bits & (1<<Z_DIRECTION_BIT)) { sys.position[Z_AXIS]--; }
      else { sys.position[Z_AXIS]++; }
    }

    st.step_events_completed++; // Iterate step events

    // While in block steps, check for de/ac-celeration events and execute them accordingly.
    if (st.step_events_completed < current_block->step_event_count) {
      if (sys.state == STATE_HOLD) {
        // Check for and execute feed hold by enforcing a steady deceleration from the moment of
        // execution. The rate of deceleration is limited by rate_delta and will never decelerate
        // faster or slower than in normal operation. If the distance required for the feed hold
        // deceleration spans more than one block, the initial rate of the following blocks are not
        // updated and deceleration is continued according to their corresponding rate_delta.
        // NOTE: The trapezoid tick cycle counter is not updated intentionally. This ensures that
        // the deceleration is smooth regardless of where the feed hold is initiated and if the
        // deceleration distance spans multiple blocks.
        if ( iterate_trapezoid_cycle_counter() ) {
          // If deceleration complete, set system flags and shutdown steppers.
          if (st.trapezoid_adjusted_rate <= current_block->rate_delta) {
            // Just go idle. Do not NULL current block. The bresenham algorithm variables must
            // remain intact to ensure the stepper path is exactly the same. Feed hold is still
            // active and is released after the buffer has been reinitialized.
            st_go_idle();
            bit_true(sys.execute,EXEC_CYCLE_STOP); // Flag main program that feed hold is complete.
          } else {
            st.trapezoid_adjusted_rate -= current_block->rate_delta;
            set_step_events_per_minute(st.trapezoid_adjusted_rate);
          }
        }

      } else {
        // The trapezoid generator always checks step event location to ensure de/ac-celerations are
        // executed and terminated at exactly the right time. This helps prevent over/under-shooting
        // the target position and speed.
        // NOTE: By increasing the ACCELERATION_TICKS_PER_SECOND in config.h, the resolution of the
        // discrete velocity changes increase and accuracy can increase as well to a point. Numerical
        // round-off errors can effect this, if set too high. This is important to note if a user has
        // very high acceleration and/or feedrate requirements for their machine.
        if (st.step_events_completed < current_block->accelerate_until) {
          // Iterate cycle counter and check if speeds need to be increased.
          if ( iterate_trapezoid_cycle_counter() ) {
            st.trapezoid_adjusted_rate += current_block->rate_delta;
            if (st.trapezoid_adjusted_rate >= current_block->nominal_rate) {
              // Reached nominal rate a little early. Cruise at nominal rate until decelerate_after.
              st.trapezoid_adjusted_rate = current_block->nominal_rate;
            }
            set_step_events_per_minute(st.trapezoid_adjusted_rate);
          }
        } else if (st.step_events_completed >= current_block->decelerate_after) {
          // Reset trapezoid tick cycle counter to make sure that the deceleration is performed the
          // same every time. Reset to CYCLES_PER_ACCELERATION_TICK/2 to follow the midpoint rule for
          // an accurate approximation of the deceleration curve. For triangle profiles, down count
          // from current cycle counter to ensure exact deceleration curve.
          if (st.step_events_completed == current_block->decelerate_after) {
            if (st.trapezoid_adjusted_rate == current_block->nominal_rate) {
              st.trapezoid_tick_cycle_counter = CYCLES_PER_ACCELERATION_TICK/2; // Trapezoid profile
            } else {
              st.trapezoid_tick_cycle_counter = CYCLES_PER_ACCELERATION_TICK-st.trapezoid_tick_cycle_counter; // Triangle profile
            }
          } else {
            // Iterate cycle counter and check if speeds need to be reduced.
            if ( iterate_trapezoid_cycle_counter() ) {
              // NOTE: We will only do a full speed reduction if the result is more than the minimum safe
              // rate, initialized in trapezoid reset as 1.5 x rate_delta. Otherwise, reduce the speed by
              // half increments until finished. The half increments are guaranteed not to exceed the
              // CNC acceleration limits, because they will never be greater than rate_delta. This catches
              // small errors that might leave steps hanging after the last trapezoid tick or a very slow
              // step rate at the end of a full stop deceleration in certain situations. The half rate
              // reductions should only be called once or twice per block and create a nice smooth
              // end deceleration.
              if (st.trapezoid_adjusted_rate > st.min_safe_rate) {
                st.trapezoid_adjusted_rate -= current_block->rate_delta;
              } else {
                st.trapezoid_adjusted_rate >>= 1; // Bit shift divide by 2
              }
              if (st.trapezoid_adjusted_rate < current_block->final_rate) {
                // Reached final rate a little early. Cruise to end of block at final rate.
                st.trapezoid_adjusted_rate = current_block->final_rate;
              }
              set_step_events_per_minute(st.trapezoid_adjusted_rate);
            }
          }
        } else {
          // No accelerations. Make sure we cruise exactly at the nominal rate.
          if (st.trapezoid_adjusted_rate != current_block->nominal_rate) {
            st.trapezoid_adjusted_rate = current_block->nominal_rate;
            set_step_events_per_minute(st.trapezoid_adjusted_rate);
          }
        }
      }
    } else {
      // If current block is finished, reset pointer
      current_block = NULL;
      plan_discard_current_block();
    }
  }
  out_bits ^= settings.invert_mask;  // Apply step and direction invert mask
  busy = false;
}

// This interrupt is set up by ISR_TIMER1_COMPA when it sets the motor port bits. It resets
// the motor port after a short period (settings.pulse_microseconds) completing one step cycle.
// NOTE: Interrupt collisions between the serial and stepper interrupts can cause delays by
// a few microseconds, if they execute right before one another. Not a big deal, but can
// cause issues at high step rates if another high frequency asynchronous interrupt is
// added to Grbl.
///ISR(TIMER2_OVF_vect)
void timer2_overflow_interrupt( void )
{
  TimerIntClear( TIMER2_BASE, TIMER_TIMA_TIMEOUT ); /// clear interrupt flag

  // Reset stepping pins (leave the direction pins)
  ///STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | (settings.invert_mask & STEP_MASK);
  GPIOPinWrite( STEPPING_PORT, STEP_MASK, settings.invert_mask );
  ///TCCR2B = 0; // Disable Timer2 to prevent re-entering this interrupt when it's not needed.
//  TimerDisable( TIMER0_BASE, TIMER_B );
  ///HWREG( TIMER0_BASE + 0x054 ) = (uint32_t) 0;
}

#ifdef STEP_PULSE_DELAY
  // This interrupt is used only when STEP_PULSE_DELAY is enabled. Here, the step pulse is
  // initiated after the STEP_PULSE_DELAY time period has elapsed. The ISR TIMER2_OVF interrupt
  // will then trigger after the appropriate settings.pulse_microseconds, as in normal operation.
  // The new timing between direction, step pulse, and step complete events are setup in the
  // st_wake_up() routine.
///ISR(TIMER2_COMPA_vect)
void timer2_compare_interrupt ( void )
{
///  STEPPING_PORT = step_bits; // Begin step pulse.  <-- BAD CODE. Forgot about other pins on this port?
  GPIOPinWrite( STEPPING_PORT, STEP_MASK, step_bits );
}
#endif

// Reset and clear stepper subsystem variables
void st_reset()
{
  memset(&st, 0, sizeof(st));
  set_step_events_per_minute(MINIMUM_STEPS_PER_MINUTE);
  current_block = NULL;
  busy = false;
}

// Initialize and start the stepper motor subsystem
void st_init()
{
  // Configure directions of interface pins
///  STEPPING_DDR |= STEPPING_MASK;
  SysCtlPeripheralEnable( STEPPING_PERIPH );
  SysCtlDelay(26); ///give time delay 1 microsecond for GPIO module to start
  GPIOPinTypeGPIOOutput( STEPPING_PORT, STEPPING_MASK );
///  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK) | settings.invert_mask;
  GPIOPinWrite( STEPPING_PORT, STEPPING_MASK, settings.invert_mask );
///  STEPPERS_DISABLE_DDR |= 1<<STEPPERS_DISABLE_BIT;
  SysCtlPeripheralEnable( STEPPERS_DISABLE_PERIPH );
  GPIOPinTypeGPIOOutput( STEPPERS_DISABLE_PORT, (1<<STEPPERS_DISABLE_BIT) );

/*
  // waveform generation = 0100 = CTC
  /// Timer1 (16 bit) mode:
  /// CTC - Clear Timer on Compare
  /// Maximum reachable value = OCR1A
  /// TOV1 flag is set (interrupt occurs) on max value (OCR1A)
  /// Timer1 gives interrupt frequency = F_CPU/(prescaler*(1+OCR1A))

  TCCR1B &= ~(1<<WGM13);
  TCCR1B |=  (1<<WGM12);
  TCCR1A &= ~(1<<WGM11);
  TCCR1A &= ~(1<<WGM10);

  // output mode = 00 (disconnected)
  TCCR1A &= ~(3<<COM1A0);
  TCCR1A &= ~(3<<COM1B0);

  // Configure Timer 2 (8 bit)
  /// Maximum reachable value = 0xFF
  /// OCRx is updated immediately
  /// TOV flag is set (interrupt occurs) on max value (0xFF)
  /// Timer2 gives interrupt frequency F_CPU/(prescaler*256)
  TCCR2A = 0; // Normal operation
  TCCR2B = 0; // Disable timer until needed (switch to 'no clock source')
  TIMSK2 |= (1<<TOIE2); // Enable Timer2 Overflow interrupt
  #ifdef STEP_PULSE_DELAY
    TIMSK2 |= (1<<OCIE2A); // Enable Timer2 Compare Match A interrupt
  #endif
*/
  // Configure Timer1
  SysCtlPeripheralEnable( SYSCTL_PERIPH_TIMER1 );
  SysCtlDelay(26); ///give time delay 1 microsecond for timer1 module to start
  TimerConfigure( TIMER1_BASE, TIMER_CFG_PERIODIC_UP );
  IntPrioritySet( INT_TIMER1A, 32 ); //lower priority than for Timer2 (which resets the step-dir signal)
  TimerControlStall( TIMER1_BASE, TIMER_A, true ); //timer1 will stall in debug mode
  TimerIntRegister( TIMER1_BASE, TIMER_A, timer1_compare_interrupt );
  TimerIntClear( TIMER1_BASE, 0xFFFF ); //disable timer1 immediate interrupt (bug of ARM?)
  IntPendClear( INT_TIMER1A );
  TimerIntEnable( TIMER1_BASE, TIMER_TIMA_TIMEOUT );

  // Configure Timer2
  SysCtlPeripheralEnable( SYSCTL_PERIPH_TIMER2 );
  SysCtlDelay(26); // give time delay 1 microsecond for timer2 module to start
  TimerConfigure( TIMER2_BASE, TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_ONE_SHOT_UP );
  IntPrioritySet( INT_TIMER2A, 0 ); // highest priority - higher than for Timer1 (which sets the step-dir output)
  TimerControlStall( TIMER2_BASE, TIMER_A, true ); //timer2 will stall in debug mode
  TimerIntRegister( TIMER2_BASE, TIMER_A, timer2_overflow_interrupt );
  TimerIntClear( TIMER2_BASE, 0xFFFF ); // disable timer2 immediate interrupt (bug of ARM?)
  IntPendClear( INT_TIMER2A );
  TimerIntEnable( TIMER2_BASE, TIMER_TIMA_TIMEOUT );

  // Start in the idle state, but first wake up to check for keep steppers enabled option.
  st_wake_up();
  st_go_idle();
}

// Configures the prescaler and ceiling of timer 1 to produce the given rate as accurately as possible.
// Returns the actual number of cycles per interrupt
static uint32_t config_step_timer(uint32_t cycles)
{
  TimerLoadSet( TIMER1_BASE, TIMER_A, cycles );
  return cycles;
/*  uint16_t ceiling;
  ///uint8_t prescaler;
  uint32_t actual_cycles;
  if (cycles <= 0xffffL) {
    ///timer1 interrupt frequency = 16'000'000/(1+cycles) = 16 MHz ... 244 Hz
    ceiling = cycles;
    ///prescaler = 1; // prescaler: 0
    actual_cycles = ceiling;
    TimerPrescaleSet( TIMER1_BASE, TIMER_A, 1 * 5 ); //prescaler is x5 because ARM is 5x faster (80 MHz)
    TimerLoadSet( TIMER1_BASE, TIMER_A, ceiling );
  } else if (cycles <= 0x7ffffL) { /// <= 524287
    ///timer1 interrupt frequency = 2'000'000/(1+cycles/8) = 244 Hz ... 30,517 Hz
    ceiling = cycles >> 3;
    ///prescaler = 2; // prescaler: 8
    actual_cycles = ceiling * 8L;
    TimerPrescaleSet( TIMER1_BASE, TIMER_A, 8 * 5 );
    TimerLoadSet( TIMER1_BASE, TIMER_A, ceiling );
  } else if (cycles <= 0x3fffffL) { /// <= 4194303
    ///timer1 interrupt frequency = 250'000/(1+cycles/64) = 3,8146 Hz ... 30,514 Hz
    ceiling =  cycles >> 6;
    ///prescaler = 3; // prescaler: 64
    actual_cycles = ceiling * 64L;
    TimerPrescaleSet( TIMER1_BASE, TIMER_A, 64 * 5 );
    TimerLoadSet( TIMER1_BASE, TIMER_A, ceiling );
  } else if (cycles <= 0xffffffL) { /// <= 16777215
    ///timer1 interrupt frequency = 62'500/(1+cycles/256) = 0,95365 Hz ... 3,8145 Hz
    ceiling =  (cycles >> 8);
    ///prescaler = 4; // prescaler: 256
    actual_cycles = ceiling * 256L;
    TimerPrescaleSet( TIMER1_BASE, TIMER_A, 256 * 5 );
    TimerLoadSet( TIMER1_BASE, TIMER_A, ceiling );
  } else if (cycles <= 0x3ffffffL) { /// <= 67108863
    ///timer1 interrupt frequency = 15'625/(1+cycles/1024) =  0,238415 Hz ... 0,95362 Hz
    ceiling = (cycles >> 10);
    ///prescaler = 5; // prescaler: 1024
    actual_cycles = ceiling * 1024L;
    TimerPrescaleSet( TIMER1_BASE, TIMER_A, 1024 * 5 );
    TimerLoadSet( TIMER1_BASE, TIMER_A, ceiling );
  } else {
    // Okay, that was slower than we actually go. Just set the slowest speed
    ///timer1 interrupt frequency = 15'625/(1+65535) = 0,238 Hz     (each 4,194304 sec)
    ///ceiling = 0xffff;
    ///prescaler = 5;
    TimerPrescaleSet( TIMER1_BASE, TIMER_A, 1024 * 5 );
    TimerLoadSet( TIMER1_BASE, TIMER_A, 0xFFFF );
    actual_cycles = 0xffff * 1024;
  }
  // Set prescaler
  ///TCCR1B = (TCCR1B & ~(0x07<<CS10)) | (prescaler<<CS10);
  // Set ceiling
  ///OCR1A = ceiling;
  return(actual_cycles);
  */
}

static void set_step_events_per_minute(uint32_t steps_per_minute)
{
  if (steps_per_minute < MINIMUM_STEPS_PER_MINUTE) { steps_per_minute = MINIMUM_STEPS_PER_MINUTE; }
  ///st.cycles_per_step_event = config_step_timer((TICKS_PER_MICROSECOND*1000000*60)/steps_per_minute);
  st.cycles_per_step_event = config_step_timer((F_CPU/steps_per_minute)*60); ///avoid values more than 4 billion...
}

// Planner external interface to start stepper interrupt and execute the blocks in queue. Called
// by the main program functions: planner auto-start and run-time command execution.
void st_cycle_start()
{
  if (sys.state == STATE_QUEUED) {
    sys.state = STATE_CYCLE;
    st_wake_up();
  }
}

// Execute a feed hold with deceleration, only during cycle. Called by main program.
void st_feed_hold()
{
  if (sys.state == STATE_CYCLE) {
    sys.state = STATE_HOLD;
    sys.auto_start = false; // Disable planner auto start upon feed hold.
  }
}

// Reinitializes the cycle plan and stepper system after a feed hold for a resume. Called by
// runtime command execution in the main program, ensuring that the planner re-plans safely.
// NOTE: Bresenham algorithm variables are still maintained through both the planner and stepper
// cycle reinitializations. The stepper path should continue exactly as if nothing has happened.
// Only the planner de/ac-celerations profiles and stepper rates have been updated.
void st_cycle_reinitialize()
{
  if (current_block != NULL) {
    // Replan buffer from the feed hold stop location.
    plan_cycle_reinitialize(current_block->step_event_count - st.step_events_completed);
    // Update initial rate and timers after feed hold.
    st.trapezoid_adjusted_rate = 0; // Resumes from rest
    set_step_events_per_minute(st.trapezoid_adjusted_rate);
    st.trapezoid_tick_cycle_counter = CYCLES_PER_ACCELERATION_TICK/2; // Start halfway for midpoint rule.
    st.step_events_completed = 0;
    sys.state = STATE_QUEUED;
  } else {
    sys.state = STATE_IDLE;
  }
}
