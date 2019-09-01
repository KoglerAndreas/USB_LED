 

.SILENT:

all:
	g++ -std=c++17 -O3 -o usb_led usb_led.cpp

clean:
	-rm usb_led