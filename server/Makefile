# Simple Makefile for blackjack_server

CC       := gcc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wpedantic
OPT      ?= -O2
CFLAGS   := $(CSTD) $(WARN) $(OPT) -pthread -Isrc -Iinclude -MMD -MP
LDFLAGS  := -pthread

TARGET   := blackjack_server
SRC_DIR  := src
OBJ_DIR  := build

SRCS := $(SRC_DIR)/main.c \
        $(SRC_DIR)/server.c \
        $(SRC_DIR)/protocol.c \
        $(SRC_DIR)/game.c

OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean debug release run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

debug: OPT = -Og -g
debug: clean all

release: OPT = -O3
release: clean all

run: $(TARGET)
	./$(TARGET) 10000

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

-include $(DEPS)
