# BioLang — C 解释器 + 源码嵌入编译器
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Isrc
# 运行时库（不含 main）：bio 解释器 + 编译驱动都链接它
LIB     = src/arena.c src/lexer.c src/value.c src/builtin.c src/parser.c src/interp.c src/bts.c src/compile.c
BIN     = bio
PREFIX  = $(HOME)/.local

$(BIN): src/main.c $(LIB) src/bio.h
	$(CC) $(CFLAGS) -DBIO_HOME=\"$(CURDIR)\" -o $@ src/main.c $(LIB) -lm

# bio -b 编译时链接的静态库（含 run_source 等全部运行时）
OBJS = $(notdir $(LIB:.c=.o))
libbio.a: $(LIB) src/bio.h
	$(CC) $(CFLAGS) -c $(LIB)
	ar rcs $@ $(OBJS)
	rm -f $(OBJS)

install: $(BIN) libbio.a
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) libbio.a $(LIB:.c=.o)

.PHONY: install clean
