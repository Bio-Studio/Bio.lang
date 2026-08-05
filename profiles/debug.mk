# profiles/debug.mk — no optimization, debug info
PROFILE_CC      := gcc
PROFILE_OPT     := -O0 -g
PROFILE_CFLAGS  :=
PROFILE_LDFLAGS := -lm
PROFILE_AR      := ar
PROFILE_PREFIX  := $(HOME)/.local
PROFILE_DESC    := Debug: -O0 -g
