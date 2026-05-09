# MicroFS Makefile
# Usage:
#   make          — build the shell (./microfs)
#   make test     — build and run tests
#   make clean    — remove build artifacts
#   make format   — create a fresh disk image

CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c99 -g -Iinclude
# Add -fsanitize=address to catch memory bugs during dev:
# CFLAGS += -fsanitize=address

SRC_DIR  = src
TEST_DIR = tests
BUILD    = build

# Source files for the main binary
SRCS     = $(SRC_DIR)/main.c \
           $(SRC_DIR)/microfs_core.c \
           $(SRC_DIR)/microfs_dir.c \
           $(SRC_DIR)/microfs_file.c \
           $(SRC_DIR)/microfs_journal.c \
           $(SRC_DIR)/microfs_shell.c

# Source files for the test binary (no main.c, no shell)
TEST_SRCS = $(TEST_DIR)/test_microfs.c \
            $(SRC_DIR)/microfs_core.c \
            $(SRC_DIR)/microfs_dir.c \
            $(SRC_DIR)/microfs_file.c \
            $(SRC_DIR)/microfs_journal.c

OBJS  = $(patsubst $(SRC_DIR)/%.c, $(BUILD)/%.o, $(SRCS))

.PHONY: all test clean format help

all: $(BUILD)/microfs
	@echo ""
	@echo "  Build complete!  →  ./build/microfs"
	@echo "  To start:          ./build/microfs"
	@echo "  To format fresh:   ./build/microfs --format"
	@echo "  To run tests:      make test"
	@echo ""

$(BUILD)/microfs: $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

$(BUILD)/test_microfs: $(TEST_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS)

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_microfs
	@echo "Running MicroFS test suite..."
	@$(BUILD)/test_microfs
	@echo "Done."

format:
	$(BUILD)/microfs --format

clean:
	rm -rf $(BUILD) microfs.disk

help:
	@echo "MicroFS Build System"
	@echo "  make         — build main binary"
	@echo "  make test    — run all tests"
	@echo "  make clean   — remove all build artifacts"
	@echo "  make format  — create/reset disk image"
