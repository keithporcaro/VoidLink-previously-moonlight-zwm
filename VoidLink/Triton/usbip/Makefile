# Tier-1 (Windows/WSL) build of the Phase-1 Triton USB/IP C core.
# The iOS app builds these same sources via Xcode (define __APPLE__, exclude triton_main.c).
CC=gcc
CFLAGS=-Wall -DLINUX
LDLIBS=-lpthread

CORE=usbip.c triton_device.c triton_input_queue.c
HDRS=usbip.h triton_device.h triton_report.h triton_report_desc.h triton_input_queue.h

all: triton-server test

triton-server: $(CORE) triton_main.c $(HDRS)
	$(CC) $(CFLAGS) $(CORE) triton_main.c -o triton-server $(LDLIBS)

# Unit-test the BLE->USB seam in isolation (the load-bearing translation + queue contract).
test: test_triton_input_queue.c triton_input_queue.c triton_input_queue.h
	$(CC) $(CFLAGS) -Wextra test_triton_input_queue.c triton_input_queue.c -o test_input_queue $(LDLIBS)
	./test_input_queue

clean:
	rm -f triton-server test_input_queue *.o
