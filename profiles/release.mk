# profiles/release.mk — aggressive optimization
PROFILE_CC      := gcc
PROFILE_OPT     := -O3
PROFILE_CFLAGS  :=
PROFILE_LDFLAGS := -lm
PROFILE_AR      := ar
PROFILE_PREFIX  := $(HOME)/.local
PROFILE_DESC    := Release: -O3
