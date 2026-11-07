# QNX 8.0 Cross-Compilation Makefile
# Targets the free QNX Everywhere / SDP 8.0 toolchain.
#
# Usage:
#   source ~/qnx800/qnxsdp-env.sh   # or your SDP path
#   make
#   make clean

# Architecture: ntox86_64 is ideal for QEMU (most common for quick demos).
# Switch to gcc_ntoaarch64le for Raspberry Pi 4/5 or other ARM targets.
QCC_VARIANT ?= gcc_ntox86_64

CC      = qcc
CFLAGS  = -V$(QCC_VARIANT) -Wall -Wextra -O2 -g
LDFLAGS =

SRC_DIR = src
TARGET  = qnx_monitor

SRCS    = $(SRC_DIR)/qnx_monitor.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built $(TARGET) for $(QCC_VARIANT)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

# Optional: show the exact compiler invocation
verbose:
	$(CC) $(CFLAGS) -vv -o $(TARGET) $(SRCS)
