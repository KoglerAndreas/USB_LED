 
.SILENT:

all:
	g++ -std=c++17 -O3 -o usb_led usb_led.cpp -DUSING_WIRING_PI -lwiringPi

no_pi:
	g++ -std=c++17 -O3 -o usb_led usb_led.cpp

run:
	sudo ./usb_led -logging -period 100ms -max 7kbps -min 4kbps -pin 17 -off 10% -help

clean:
	-rm usb_led