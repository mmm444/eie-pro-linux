# If KERNELRELEASE is defined, we've been invoked from the
# kernel build system and can use its language.
ifneq ($(KERNELRELEASE),)
	obj-m := eie-pro.o
# Otherwise we were called directly from the command
# line; invoke the kernel build system.
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) CC=$(CC) CFLAGS='$(CFLAGS)' LDFLAGS='$(LDFLAGS)' modules

install: default
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
	depmod -a

live: install
	modprobe eie_pro

uninstall:
	modprobe -r eie_pro
	rm /lib/modules/$(shell uname -r)/updates/eie-pro.ko
	depmod -a

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

cleanup: | uninstall clean

test:
	-sudo rmmod eie_pro
	sudo insmod eie-pro.ko dyndbg==pmft
	-timeout 8 aplay -vv -Dsysdefault:CARD=pro /usr/share/sounds/alsa/Front_Center.wav

check:
	/lib/modules/$(shell uname -r)/source/scripts/checkpatch.pl --no-tree --file eie-pro.c
endif
