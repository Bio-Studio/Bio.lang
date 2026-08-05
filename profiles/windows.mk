# profiles/windows.mk — Windows / MinGW-w64, EXPERIMENTAL.
#
# Use an MSYS2/MinGW-w64 environment. No $(shell ...) here on purpose: those
# rely on POSIX tools that cmd.exe lacks. The ucontext-based thread backend
# (src/bts.c) has no Windows implementation yet, so `make PROFILE=windows`
# compiles most of the tree but NOT the Threads/Taskm subsystem — see README
# "Platform support". `bio -b` still needs a C compiler on PATH.
PROFILE_CC      := gcc
PROFILE_OPT     := -O2
PROFILE_CFLAGS  := -D_WIN32
PROFILE_LDFLAGS :=
PROFILE_AR      := ar
PROFILE_PREFIX  := $(USERPROFILE)/.local
RM              := del /Q
PROFILE_DESC    := Windows/MinGW-w64 (experimental; thread backend pending)
