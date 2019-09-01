 

.SILENT:

all:
	g++ -std=c++17 -O3 -o usb_led usb_led.cpp

run:
	sudo ./usb_led

clean:
	-rm usb_led