CC ?= clang
HB_ROOT = ../../engine/hyperbridge
CFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -I$(HB_ROOT)/include -arch arm64 -mmacosx-version-min=14.0
LDFLAGS = $(HB_ROOT)/libhyperbridge.a

all: hb_x86_probe hb_x64_probe

hb_x86_probe: probe_x86.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

hb_x64_probe: probe_x64.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

probe_x86.o: probe.c
	$(CC) $(CFLAGS) -c $< -o $@

probe_x64.o: probe.c
	$(CC) $(CFLAGS) -DMODE_64 -c $< -o $@

clean:
	rm -f *.o hb_x86_probe hb_x64_probe
