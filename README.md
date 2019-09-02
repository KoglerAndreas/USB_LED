# USB_LED
A small c++17 program to generate a LED PWM based on the current overall USB traffic.

## FEATURES

### PWM-Periode
The PWM-Periode can be set in milliseconds and seconds. This given period will then be used to generate the PWM signal to power the specified GPIO pin.

The maximum transfer rate can be set by the "-period value[s|ms]" flag.

### Enforced Off Periode
To keep the LED blinking at the maximum transfer rate, the user can specify a enforced off period in percent where the LED is for sure powered off.

The off period can be set by the "-off value[%]" flag.

### Maximum Transfer Rate
The maximum USB transfer led corresponds to the maximum PWM duty cycle.

The maximum transfer rate can be set by the "-max value[Mpbs|Kpbs]" flag.

### Minimum Transfer Rate
The minimum transfer threshold of the USB device. If the USB traffic is less then this threshold the PWM is shut down.

The minimum transfer rate can be set by the "-min value[Mpbs|Kpbs]" flag.

### GPIO Pin
This is the configured GPIO pin to use as driving output. In other words, the pin where the LED is connected.

The GPIO pin can be set by the "-gpio value" flag.

### Logging
Shows current debug information:
<pre>
Rate:     7.910 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     7.998 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     8.262 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     7.734 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     8.086 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     7.559 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     8.086 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     7.646 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     8.428 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     7.734 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     7.822 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     7.207 kb/s   PWM:  0.099 s   [H:  0.089 s   L: 0.010 s]
Rate:     2.725 kb/s   PWM:  0.099 s   [H:  0.028 s   L: 0.071 s]
Rate:     3.604 kb/s   PWM:  0.099 s   [H:  0.043 s   L: 0.056 s]
Rate:     2.461 kb/s   PWM:  0.099 s   [H:  0.024 s   L: 0.075 s]
Rate:     4.570 kb/s   PWM:  0.099 s   [H:  0.059 s   L: 0.040 s]
Rate:     3.604 kb/s   PWM:  0.099 s   [H:  0.043 s   L: 0.056 s]
Rate:     4.395 kb/s   PWM:  0.099 s   [H:  0.056 s   L: 0.043 s]
Rate:     3.779 kb/s   PWM:  0.099 s   [H:  0.046 s   L: 0.053 s]
Rate:     3.779 kb/s   PWM:  0.099 s   [H:  0.046 s   L: 0.053 s]
Rate:     3.340 kb/s   PWM:  0.099 s   [H:  0.039 s   L: 0.060 s]
Rate:     3.779 kb/s   PWM:  0.099 s   [H:  0.046 s   L: 0.053 s]
Rate:     3.516 kb/s   PWM:  0.099 s   [H:  0.042 s   L: 0.057 s]
</pre>

Field | Meaning
------------ | -------------
Rate | Shows the extrapolated transfer rate in kilobyte per second
PWM  | Shows the current PWM period of the LED in seconds
H    | Shows the high time of the LED in seconds
L    | Shows the low time of the LED in seconds

Logging can be enabled by specifying "-logging" at the command line.

### Help
A help message can be printed by specifying the "-help" flag at the command line.

## Build
A Makefile with two targets is provided:

### all
Build the USB led PWM program with the "wiringpi" library

### no_pi
Build the USB led PWM program without the "wiringpi" library