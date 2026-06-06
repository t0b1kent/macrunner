CC ?= clang
HB_ROOT = ../../engine/hyperbridge
CFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -I$(HB_ROOT)/include -arch arm64 -mmacosx-version-min=14.0
LDFLAGS = $(HB_ROOT)/libhyperbridge.a

TARGET = hb_x86_probe

all: $(TARGET)

$(TARGET): probe.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

probe.o: probe.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET)
