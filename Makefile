ROOT    := $(shell pwd)
SRC_DIR := $(ROOT)/src

SRCS = $(shell find $(SRC_DIR) -name '*.c')
OBJS = $(SRCS:.c=.o)
TARGET = $(ROOT)/spinc

CFLAGS  := -Wall -Wextra -std=gnu23 -O2 -g -fsanitize=address #-DLOG_LEVEL=INFO
LDFLAGS := -lcurl -fsanitize=address

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	@$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean