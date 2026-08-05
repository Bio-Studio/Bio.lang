# BioLang — C interpreter + source-embedding compiler
# Build profiles: `make PROFILE=<name>` (default: auto-detect).
#   make profiles        list available profiles
#   make PROFILE=x info  show the resolved toolchain for profile x
# Switching profiles requires `make clean` (make tracks timestamps, not values).

# ---- Profile selection -----------------------------------------------------
PROFILE ?= default
# Missing profile file = hard error (`include`, not `-include`).
include profiles/$(PROFILE).mk
ifeq ($(PROFILE_CC),)
$(error profile '$(PROFILE)' does not define PROFILE_CC)
endif

# ---- Toolchain (from the profile) -----------------------------------------
CC      = $(PROFILE_CC)
AR      = $(PROFILE_AR)
LDFLAGS = $(PROFILE_LDFLAGS)
PREFIX ?= $(or $(PROFILE_PREFIX),$(HOME)/.local)

# The '"..."' single-quote wrap keeps space-containing values (e.g.
# "ccache gcc") as ONE compiler argv and lets the C string literal reach the
# preprocessor, so `bio -b` reuses this profile's compiler/link flags.
CFLAGS = -Wall -Wextra $(PROFILE_OPT) -Isrc $(PROFILE_CFLAGS) \
         -DBIO_CC='"$(PROFILE_CC)"' -DBIO_LDFLAGS='"$(PROFILE_LDFLAGS)"'

# ---- Sources ---------------------------------------------------------------
LIB  = src/arena.c src/lexer.c src/value.c src/builtin.c src/parser.c \
       src/interp.c src/bts.c src/compile.c src/toml.c src/project.c \
       src/platform.c
BIN  = bio
OBJS = $(notdir $(LIB:.c=.o))

# ---- Build -----------------------------------------------------------------
all: $(BIN)

$(BIN): src/main.c $(LIB) src/bio.h src/platform.h
	$(CC) $(CFLAGS) -DBIO_HOME='"$(CURDIR)"' -o $@ src/main.c $(LIB) $(LDFLAGS)

# Standalone static runtime library (optional; not used by $(BIN) or install).
libbio.a: $(LIB) src/bio.h src/platform.h
	$(CC) $(CFLAGS) -c $(LIB)
	$(AR) rcs $@ $(OBJS)
	rm -f $(OBJS)

# ---- Install ---------------------------------------------------------------
install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

install-lib: libbio.a
	install -d $(PREFIX)/lib $(PREFIX)/include
	install -m 644 libbio.a $(PREFIX)/lib/libbio.a
	install -m 644 src/bio.h $(PREFIX)/include/bio.h

# ---- Meta ------------------------------------------------------------------
# Print resolved settings ONLY when `info` is a goal. Shell echo can't handle
# the embedded quotes in CFLAGS, so use $(info) gated on MAKECMDGOALS (must be
# defined after CFLAGS, since $(info) evaluates at parse time).
ifeq ($(filter info,$(MAKECMDGOALS)),info)
$(info PROFILE  = $(PROFILE))
$(info CC       = $(CC))
$(info CFLAGS   = $(CFLAGS))
$(info LDFLAGS  = $(LDFLAGS))
$(info AR       = $(AR))
$(info PREFIX   = $(PREFIX))
endif
info:
	@:

profiles:
	@for f in profiles/*.mk; do \
	  name="$${f#profiles/}"; name="$${name%.mk}"; \
	  desc="$$(sed -n 's/^PROFILE_DESC[[:space:]]*[:?]\?=[[:space:]]*//p' "$$f" | head -n1)"; \
	  printf '%-16s %s\n' "$$name" "$$desc"; \
	done

clean:
	-$(RM) $(BIN) libbio.a $(OBJS)

.PHONY: all install install-lib clean profiles info
