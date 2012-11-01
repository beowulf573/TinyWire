/**
 * Example sketch for writing to and reading from a slave in transactional manner
 *
 * NOTE: You must not use delay() or I2C communications will fail, use tws_delay() instead (or preferably some smarter timing system)
 *
 * On write the first byte received is considered the register addres to modify/read
 * On each byte sent or read the register address is incremented (and it will loop back to 0)
 *
 * You can try this with the Arduino I2C REPL sketch at https://github.com/rambo/I2C/blob/master/examples/i2crepl/i2crepl.ino 
 * If you have bus-pirate remember that the older revisions do not like the slave streching the clock, this leads to all sorts of weird behaviour
 *
 * To read third value (register number 2 since counting starts at 0) send "[ 8 2 [ 9 r ]", value read should be 0xBE
 * If you then send "[ 9 r r r ]" you should get 0xEF 0xDE 0xAD as response (demonstrating the register counter looping back to zero)
 *
 * You need to have at least 8MHz clock on the ATTiny for this to work (and in fact I have so far tested it only on ATTiny85 @8MHz using internal oscillator)
 * Remember to "Burn bootloader" to make sure your chip is in correct mode 
 */


/**
 * Pin notes by Suovula, see also http://hlt.media.mit.edu/?p=1229
 *
 * DIP and SOIC have same pinout, however the SOIC chips are much cheaper, especially if you buy more than 5 at a time
 * For nice breakout boards see https://github.com/rambo/attiny_boards
 *
 * Basically the arduino pin numbers map directly to the PORTB bit numbers.
 *
// I2C
arduino pin 0 = not(OC1A) = PORTB <- _BV(0) = SOIC pin 5 (I2C SDA, PWM)
arduino pin 2 =           = PORTB <- _BV(2) = SOIC pin 7 (I2C SCL, Analog 1)
// Timer1 -> PWM
arduino pin 1 =     OC1A  = PORTB <- _BV(1) = SOIC pin 6 (PWM)
arduino pin 3 = not(OC1B) = PORTB <- _BV(3) = SOIC pin 2 (Analog 3)
arduino pin 4 =     OC1B  = PORTB <- _BV(4) = SOIC pin 3 (Analog 2)
 */
#define I2C_SLAVE_ADDRESS 0x4 // the 7-bit address (remember to change this when adapting this example)
// Get this from https://github.com/rambo/TinyWire
#include <TinyWireS.h>
// The default buffer size, Can't recall the scope of defines right now
#ifndef TWI_RX_BUFFER_SIZE
#define TWI_RX_BUFFER_SIZE ( 16 )
#endif
// Get this library from http://bleaklow.com/files/2010/Task.tar.gz 
// and read http://bleaklow.com/2010/07/20/a_very_simple_arduino_task_manager.html for instructions
#include <Task.h>
#include <TaskScheduler.h>


/**
 * BEGIN: Blinker task copied from the Task library example
 */
// Timed task to blink a LED.
class Blinker : public TimedTask
{
public:
    // Create a new blinker for the specified pin and rate.
    Blinker(uint8_t _pin, uint32_t _rate);
    virtual void run(uint32_t now);
private:
    uint8_t pin;      // LED pin.
    uint32_t rate;    // Blink rate.
    bool on;          // Current state of the LED.
};

Blinker::Blinker(uint8_t _pin, uint32_t _rate)
: TimedTask(millis()),
  pin(_pin),
  rate(_rate),
  on(false)
{
    pinMode(pin, OUTPUT);     // Set pin for output.
}

void Blinker::run(uint32_t now)
{
    // If the LED is on, turn it off and remember the state.
    if (on) {
        digitalWrite(pin, LOW);
        on = false;
    // If the LED is off, turn it on and remember the state.
    } else {
        digitalWrite(pin, HIGH);
        on = true;
    }
    // Run again in the required number of milliseconds.
    incRunTime(rate);
}
/**
 * END: Blinker task copied from the Task library example
 */
/**
 * BEGIN: I2C Stop flag checker
 */
#define I2CStopCheck_YIELD_TICKS 4
// Task to echo serial input.
class I2CStopCheck : public Task
{
public:
    I2CStopCheck();
    virtual void run(uint32_t now);
    virtual bool canRun(uint32_t now);
private:
    uint8_t yield_counter; // Incremented on each canRun call, used to yield to other tasks.
};

I2CStopCheck::I2CStopCheck()
: Task()
{
}

// We're always ready to run this task-
bool I2CStopCheck::canRun(uint32_t now)
{
    yield_counter++;
    return ((yield_counter % I2CStopCheck_YIELD_TICKS) == 1);
}

void I2CStopCheck::run(uint32_t now)
{
    TinyWireS_stop_check();
}
/**
 * END: I2C Stop flag checker
 */

// Create the tasks.
Blinker blinker(3, 100);
I2CStopCheck checker;

// Tasks are in priority order, only one task is run per tick
Task *tasks[] = { &checker, &blinker, };
TaskScheduler sched(tasks, NUM_TASKS(tasks));

// The I2C registers
volatile uint8_t i2c_regs[] =
{
    0xDE, 
    0xAD, 
    0xBE, 
    0xEF, 
};


volatile byte reg_position;
void requestEvent()
{  
    TinyWireS.send(i2c_regs[reg_position]);
    // Increment the reg position on each read, and loop back to zero
    reg_position = (reg_position+1) % sizeof(i2c_regs);
}

/**
 * The I2C data received -handler
 *
 * This needs to complete before the next incoming transaction (start, data, restart/stop) does 
 */
void receiveEvent(uint8_t howMany)
{
    if (howMany < 1)
    {
        // Sanity-check
        return;
    }
    if (howMany > TWI_RX_BUFFER_SIZE)
    {
        // Also insane number
        return;
    }

    reg_position = TinyWireS.receive();
    howMany--;
    if (!howMany)
    {
        // This write was only to set the buffer for next read
        return;
    }
    while(howMany--)
    {
        i2c_regs[reg_position%sizeof(i2c_regs)] = TinyWireS.receive();
        reg_position++;
    }
}


void setup()
{
    // TODO: Tri-state this and wait for input voltage to stabilize 
    pinMode(3, OUTPUT); // OC1B-, Arduino pin 3, ADC
    digitalWrite(3, LOW); // Note that this makes the led turn on, it's wire this way to allow for the voltage sensing above.

    pinMode(1, OUTPUT); // OC1A, also The only HW-PWM -pin supported by the tiny core analogWrite

    /**
     * Reminder: taking care of pull-ups is the masters job
     */

    TinyWireS.begin(I2C_SLAVE_ADDRESS);
    TinyWireS.onReceive(receiveEvent);
    TinyWireS.onRequest(requestEvent);

    
    // Whatever other setup routines ?
    
    digitalWrite(3, HIGH);
}

void loop()
{
    // Run the scheduler - never returns.
    sched.run();
}
