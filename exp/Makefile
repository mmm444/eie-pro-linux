CFLAGS:=$(shell pkg-config --cflags libusb-1.0 sndfile) -Wall -g -O2
LDLIBS:=$(shell pkg-config --libs libusb-1.0 sndfile) -lm

run: pokus
	./r

pokus: pokus.o

pokus.o: pokus.c

clean:
	rm pokus pokus.o

.PHONY: run clean