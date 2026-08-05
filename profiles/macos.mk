# profiles/macos.mk — macOS, clang, -O2.
# ucontext is deprecated on Darwin; silence the warnings. The thread backend is
# not reliable on Apple Silicon yet (see README "Platform support").
PROFILE_CC      := clang
PROFILE_OPT     := -O2
PROFILE_CFLAGS  := -Wno-deprecated-declarations
PROFILE_LDFLAGS := -lm
PROFILE_AR      := ar
PROFILE_PREFIX  := $(HOME)/.local
PROFILE_DESC    := macOS, clang, -O2 (ucontext deprecation silenced)
