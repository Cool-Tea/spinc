ROOT    := $(shell pwd)
SRC_DIR := $(ROOT)/src

CONFIG = $(SRC_DIR)/config.h
SRCS = $(shell find $(SRC_DIR) -name '*.c')
OBJS = $(SRCS:.c=.o)
DEBUG_OBJS = $(SRCS:.c=.o.debug)
DEPS = $(SRCS:.c=.d)
DEBUG_DEPS = $(SRCS:.c=.o.d)
TARGET       = $(ROOT)/spinc
DEBUG_TARGET = $(ROOT)/spinc_debug

DEBUG_CFLAGS  := -I$(SRC_DIR) -Wall -Wextra -std=gnu23 -O0 -g -fsanitize=address -fno-omit-frame-pointer -DLOG_LEVEL=DEBUG -MMD
DEBUG_LDFLAGS := -lcurl -lreadline -luuid -fsanitize=address -fno-omit-frame-pointer
CFLAGS  := -I$(SRC_DIR) -std=gnu23 -O2 -flto=auto -MMD
LDFLAGS := -lcurl -lreadline -luuid -flto=auto

all: $(TARGET) $(DEBUG_TARGET)

release: $(TARGET)

debug: $(DEBUG_TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) 

$(DEBUG_TARGET): $(DEBUG_OBJS)
	$(CC) -o $@ $^ $(DEBUG_LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRC_DIR)/%.o.debug: $(SRC_DIR)/%.c
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

run: $(TARGET)
	@$(TARGET)

run-debug: $(DEBUG_TARGET)
	@$(DEBUG_TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(DEPS) $(DEBUG_OBJS) $(DEBUG_TARGET) $(DEBUG_DEPS)

-include $(DEPS)
-include $(DEBUG_DEPS)

.PHONY: clean release debug run run-debug