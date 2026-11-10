# Makefile for qnx_monitor (QNX SDP 8.0)
#
# Usage (after running qnxsdp-env.bat on Windows, or sourcing
# qnxsdp-env.sh on Linux, so qcc is on your PATH):
#   make                                  # qcc -Vgcc_ntox86_64
#   make QCC_VARIANT=gcc_ntoaarch64le     # 64-bit ARM target
#   make clean
#
# CC can be overridden (make CC=/path/to/qcc). The -V variant flag is only
# added when the compiler is qcc; set TARGET_FLAGS yourself for anything else.
#
# Sponsored by CREIGNIFICENT LLC

QCC_VARIANT ?= gcc_ntox86_64

# Make's built-in default for CC is "cc"; use qcc unless the user set CC.
ifeq ($(origin CC),default)
CC = qcc
endif

ifneq ($(filter qcc%,$(notdir $(CC))),)
TARGET_FLAGS ?= -V$(QCC_VARIANT)
endif

CFLAGS  ?= -O2 -g
CFLAGS  += $(TARGET_FLAGS) -std=gnu11 -Wall -Wextra
LDFLAGS ?=
LDLIBS  ?=

SRC_DIR = src
TARGET  = qnx_monitor
SRCS    = $(SRC_DIR)/qnx_monitor.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean verbose

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Built $(TARGET) with $(CC) $(TARGET_FLAGS)"

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

# Show the exact compiler invocation
verbose:
	$(CC) $(CFLAGS) -vv -o $(TARGET) $(SRCS) $(LDLIBS)
