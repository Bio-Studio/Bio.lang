# profiles/sanitize.mk — AddressSanitizer + UndefinedBehaviorSanitizer (dev)
PROFILE_CC      := gcc
PROFILE_OPT     := -O1 -g
PROFILE_CFLAGS  := -fsanitize=address,undefined -fno-omit-frame-pointer
PROFILE_LDFLAGS := -lm -fsanitize=address,undefined
PROFILE_AR      := ar
PROFILE_PREFIX  := $(HOME)/.local
PROFILE_DESC    := ASan + UBSan (development)
