# Makefile for hdhomerun_tui

# Compiler and flags
CC = gcc
CFLAGS = -Wall -I./libhdhomerun
LDFLAGS = -lncurses -lpthread -lm

# OS Detection
OS := $(shell uname -s)

# Name of the final executable
TARGET = hdhomerun_tui

# Base library sources
LIB_SRCS_BASE = \
	libhdhomerun/hdhomerun_channels.c \
	libhdhomerun/hdhomerun_channelscan.c \
	libhdhomerun/hdhomerun_control.c \
	libhdhomerun/hdhomerun_debug.c \
	libhdhomerun/hdhomerun_device.c \
	libhdhomerun/hdhomerun_device_selector.c \
	libhdhomerun/hdhomerun_discover.c \
	libhdhomerun/hdhomerun_os_posix.c \
	libhdhomerun/hdhomerun_pkt.c \
	libhdhomerun/hdhomerun_sock.c \
	libhdhomerun/hdhomerun_sock_posix.c \
	libhdhomerun/hdhomerun_video.c

# OS-specific sources for getting local IP info
ifeq ($(OS),Linux)
  IF_DETECT_SRC = libhdhomerun/hdhomerun_sock_netlink.c
  LDFLAGS += -lrt
else
  # Default for macOS, BSD, etc.
  IF_DETECT_SRC = libhdhomerun/hdhomerun_sock_getifaddrs.c
endif

# Combine all library sources
LIB_SRCS = $(LIB_SRCS_BASE) $(IF_DETECT_SRC)
LIB_OBJS = $(LIB_SRCS:.c=.o)

# Source files for the TUI application
# hdhomerun_tui.c, l1_details.c and tuner.c use only the common tuner
# interface. Backends: tuner_hdhr.c (the only file that uses libhdhomerun)
# and tuner_cxd28xx.c (USB tuners on the cxd28xx driver, via /dev/dvb and
# /dev/alpraw).
APP_SRCS = hdhomerun_tui.c l1_detail_parser.c l1_details.c tuner.c tuner_hdhr.c tuner_cxd28xx.c
APP_HDRS = l1_detail_parser.h l1_details.h tuner.h atsc3_tables.h alp_raw.h
APP_OBJS = $(APP_SRCS:.c=.o)

# Default target
all: $(TARGET)

# Rule to link the final executable
$(TARGET): $(APP_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(APP_OBJS) $(LIB_OBJS) $(LDFLAGS)

# Rule to compile a .c file into a .o file
%.o: %.c $(APP_HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(TARGET) $(APP_OBJS) $(LIB_OBJS)

# Install target (optional)
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

# Uninstall target (optional)
uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
