# profiles/zig.mk — Zig toolchain (zig cc), native target by default.
#
# Requires zig (>= 0.14) on PATH. Zig cross-compiles to any target without a
# foreign SDK, e.g.:
#   make PROFILE=zig PROFILE_CFLAGS='-target x86_64-windows-gnu' PROFILE_LDFLAGS=''
#   make PROFILE=zig PROFILE_CFLAGS='-target x86_64-macos'         PROFILE_LDFLAGS=''
# (native build: make PROFILE=zig)
PROFILE_CC      := zig cc
PROFILE_OPT     := -O2
PROFILE_CFLAGS  :=
PROFILE_LDFLAGS := -lm
PROFILE_AR      := zig ar
PROFILE_PREFIX  := $(HOME)/.local
PROFILE_DESC    := Zig (zig cc) — native by default; cross-compiles to Linux/macOS/Windows
