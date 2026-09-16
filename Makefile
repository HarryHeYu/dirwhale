CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
PREFIX  ?= /usr/local

SRC := src/main.c
ifeq ($(OS),Windows_NT)
  BIN    := dirwhale.exe
  PYTHON ?= python
else
  BIN    := dirwhale
  PYTHON ?= python3
endif

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

install: $(BIN)
	install -m 755 $(BIN) $(PREFIX)/bin/dirwhale

test: $(BIN)
	$(PYTHON) tests/run.py

clean:
	rm -f dirwhale dirwhale.exe

.PHONY: all install clean test
