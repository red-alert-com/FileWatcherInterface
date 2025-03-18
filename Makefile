# Makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pedantic
LDFLAGS = 

SOURCES = fswatcher.c daemon_utils.c
HEADERS = daemon_utils.h
OBJECTS = $(SOURCES:.c=.o)
TARGET = fswatcher

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $

clean:
	rm -f $(OBJECTS) $(TARGET)
