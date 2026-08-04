# BioLang — C 解释器
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Isrc
SRC     = src/main.c src/arena.c src/lexer.c src/value.c src/builtin.c src/parser.c src/interp.c src/bts.c
BIN     = bio
PREFIX  = $(HOME)/.local

$(BIN): $(SRC) src/bio.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

install: $(BIN)
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: install clean
