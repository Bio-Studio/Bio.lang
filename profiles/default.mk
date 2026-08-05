# profiles/default.mk — auto-detect platform + compiler.
#
# $(shell ...) is evaluated once at parse time (:=) and expands to EMPTY when
# the tool or a POSIX shell is unavailable (e.g. cmd.exe), so every branch
# guards on emptiness. For a real Windows build use `make PROFILE=windows`.

UNAME_S    := $(shell uname -s 2>/dev/null)
HAVE_GCC   := $(shell command -v gcc 2>/dev/null)
HAVE_CLANG := $(shell command -v clang 2>/dev/null)

ifneq ($(UNAME_S),)
  ifeq ($(UNAME_S),Darwin)
    PROFILE_CC := clang
  else
    PROFILE_CC := $(if $(HAVE_GCC),gcc,$(if $(HAVE_CLANG),clang,gcc))
  endif
  PROFILE_LDFLAGS := -lm
else
  # No uname (minimal shell / cmd.exe): assume MinGW-style gcc, no libm.
  PROFILE_CC      := gcc
  PROFILE_LDFLAGS :=
endif

PROFILE_OPT     := -O2
PROFILE_CFLAGS  :=
PROFILE_AR      := ar
PROFILE_PREFIX  := $(HOME)/.local
PROFILE_DESC    := Auto-detect platform/compiler (default)
