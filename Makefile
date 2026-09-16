CFLAGS  ?= -O2 -Wall -Wextra -std=c99
PREFIX  ?= /usr/local
SRC     := src/main.c

ifeq ($(OS),Windows_NT)
  CC     ?= gcc
  BIN    := dirwhale.exe
  PYTHON ?= python
else
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S),Darwin)
    CC ?= cc
  else
    CC ?= gcc
  endif
  BIN    := dirwhale
  PYTHON ?= python3
endif

ifeq ($(UNIVERSAL),1)
  CFLAGS += -arch arm64 -arch x86_64 -mmacosx-version-min=11.0
endif

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

install: $(BIN)
	install -m 755 $(BIN) $(PREFIX)/bin/dirwhale

# Universal Intel + Apple Silicon binary. Only works on macOS.
universal:
	@test "$(UNAME_S)" = "Darwin" || (echo "make universal requires macOS" >&2; exit 1)
	$(MAKE) clean
	$(MAKE) UNIVERSAL=1
	codesign -s - --force $(BIN)

test: $(BIN)
	$(PYTHON) tests/run.py

clean:
	rm -f dirwhale dirwhale.exe dirwhale-darwin-universal

.PHONY: all install clean test universal
