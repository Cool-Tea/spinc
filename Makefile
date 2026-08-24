ROOT    := $(shell pwd)
SRC_DIR := $(ROOT)/src

SRCS = $(shell find $(SRC_DIR) -name '*.c')
OBJS = $(SRCS:.c=.o)
DEBUG_OBJS = $(SRCS:.c=.o.debug)
TARGET       = $(ROOT)/spinc
DEBUG_TARGET = $(ROOT)/spinc_debug

DEBUG_CFLAGS  := -Wall -Wextra -std=gnu23 -O0 -g -fsanitize=address -fno-omit-frame-pointer
DEBUG_LDFLAGS := -lcurl -lreadline -fsanitize=address -fno-omit-frame-pointer
CFLAGS  := -std=gnu23 -O2
LDFLAGS := -lcurl -lreadline

all: $(TARGET) $(DEBUG_TARGET)

release: $(TARGET)

debug: $(DEBUG_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(DEBUG_TARGET): $(DEBUG_OBJS)
	$(CC) $(DEBUG_LDFLAGS) -o $@ $^

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRC_DIR)/%.o.debug: $(SRC_DIR)/%.c
	$(CC) $(DEBUG_CFLAGS) -c -o $@ $<

run: $(TARGET)
	@$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(DEBUG_OBJS) $(DEBUG_TARGET)

.PHONY: clean release debug run