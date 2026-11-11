# hardware-stats - live hardware and process monitor for Linux
# Copyright (C) 2026  Yormask - GPL-3.0-or-later (see LICENSE)
BIN     := hardware-stats
CC      ?= cc
PREFIX  ?= /usr/local
CFLAGS  ?= -O2 -pipe
CFLAGS  += -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter

NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
NCURSES_LIBS   := $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)
LDLIBS  += $(NCURSES_LIBS) -ldl -lm

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)
HDR := $(wildcard src/*.h)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) $(NCURSES_CFLAGS) -c -o $@ $<

build:
	mkdir -p build

# Build with AddressSanitizer + UBSan for development
debug: CFLAGS = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -rf build $(BIN)

.PHONY: all debug install uninstall clean
