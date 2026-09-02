CC := gcc
CFLAGS := -O3 -Wall -Wextra -Wpedantic -std=c11

SRC_DIR := src
BUILD_DIR := build

all: sender receiver

sender: $(BUILD_DIR)/sender.o $(BUILD_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

receiver: $(BUILD_DIR)/receiver.o $(BUILD_DIR)/common.o
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) sender receiver

.PHONY: all clean
