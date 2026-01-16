# Makefile for shell (Part A+B)
CC = gcc
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
         -Wall -Wextra -Werror -Wno-unused-parameter -fno-asm \
         -Iinclude
SRCS = src/main.c src/prompt.c src/parser.c src/intrinsics.c src/exec.c
OBJS = $(SRCS:.c=.o)
TARGET = shell.out

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
