rpi_pcm_ws281x
==============

## Intro

This library is using the PCM hardware to drive the neopixels instead of the
PWM hardware which is used by the other neopixel libraries.
The starting point of the code was the PWM version as included in the Pimoroni
unicorn hat libraty (https://github.com/pimoroni/unicorn-hat/tree/master/library/rpi-ws281x)

Differences with the PWM version:
- Only one channel available instead of two.
- Uses the PCM_DOUT pin (pin 40 on the 40-pin connector, GPIO21-ALT0)
  instead of the PWM0 pin (pin 12 on the 40-pin connector, GPIO18-ALT5)
- Since the PCM hardware is used, no I2S audio (hifiberry, pHAT DAC, etc.)
- However analog audio via the headphone jack can be used simultaneously

###Background:

The BCM2835 in the Raspberry Pi has a PCM module that can be used to
drive individually controllable WS281X LEDs.  Using the DMA, and PCM 
transmit FIFO, it's possible to control almost any number
of WS281X LEDs in a chain connected to the PCM output pin.

This library and test program set the clock rate of the PCM controller to
3X the desired output frequency and creates a bit pattern in RAM from an
array of colors where each bit is represented by 3 bits for the PCM
controller as follows.

    Bit 1 - 1 1 0
    Bit 0 - 1 0 0


###Hardware:

WS281X LEDs are generally driven at 5V, which requires that the data
signal be at the same level.  Converting the output from a Raspberry
Pi GPIO/PWM to a higher voltage through a level shifter is required.

It is possible to run the LEDs from a 3.3V - 3.6V power source, and
connect the GPIO directly at a cost of brightness, but this isn't
recommended.

The test program is designed to drive a 8x8 grid of LEDs from Adafruit
(http://www.adafruit.com/products/1487).  Please see the Adafruit
website for more information.

###Build the C library and C test program

- Make sure to adjust the parameters in main.c to suit your hardare.
  - Signal rate (400kHz to 800kHz).  Default 800kHz.
  - ledstring.invert=1 if using a inverting level shifter.
  - Width and height of LED matrix (height=1 for LED string).
- Goto the C library source: rpi-pcm-ws281x/lib
- 'make lib' to build the library 'libws2811-pcm.a'
- 'make test' to build the test program.

###Running the C test program:

- Type 'sudo ./test'.
- That's it.  You should see a moving rainbow scroll across the
  display.


###Build and install the Python bindings

- Goto 'rpi-pcm-ws281x' 
- 'sudo python ./setup.bpy build'
- 'sudo python ./setup.py install'

The top level Python module is 'neopixel_pcm'

###Usage:

The API is very simple.  Make sure to create and initialize the ws2811_t
structure as seen in main.c.  From there it can be initialized
by calling ws2811_init().  LEDs are changed by modifying the color in
the .led[index] array and calling ws2811_render().  The rest is handled
by the library, which creates the DMA memory and starts the DMA/PCM.

Make sure to hook a signal handler for SIGKILL to do cleanup.  From the
handler make sure to call ws2811_fini().  It'll make sure that the DMA
is finished before program execution stops.

