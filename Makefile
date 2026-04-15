CC      = gcc
CFLAGS  = -Wall -Wextra -O2
KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

obj-m += monitor.o

.PHONY: all ci clean

all: engine cpu_hog memory_hog io_pulse
	$(MAKE) -C $(KDIR) M=$(PWD) modules

ci: engine cpu_hog memory_hog io_pulse

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ $< -lpthread

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o $@ $<

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o $@ $<

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f engine cpu_hog memory_hob io_pulse
	$(MAKE) -C $(KDIR) M=$(PWD) clean