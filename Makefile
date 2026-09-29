CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  = -lpthread -lm

ifeq ($(shell uname -s),Darwin)
LDLIBS += -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
RAYLIB_PREFIX ?= $(shell brew --prefix raylib 2>/dev/null)
RAYLIB_CFLAGS ?= -I$(RAYLIB_PREFIX)/include
RAYLIB_LIBS   ?= -L$(RAYLIB_PREFIX)/lib -lraylib -lobjc
HAVE_RAYLIB   := $(wildcard $(RAYLIB_PREFIX)/include/raylib.h)
else
LDLIBS += -ldl
RAYLIB_CFLAGS ?= $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS   ?= $(shell pkg-config --libs raylib 2>/dev/null)
HAVE_RAYLIB   := $(RAYLIB_LIBS)
endif

# 找到 raylib 时启用图形窗口 (-g)；make NO_GUI=1 强制只编译终端版
ifneq ($(HAVE_RAYLIB),)
ifeq ($(NO_GUI),)
CFLAGS += -DAP88_GUI $(RAYLIB_CFLAGS)
LDLIBS += $(RAYLIB_LIBS)
endif
endif

ap88: main.c miniaudio.h
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

clean:
	rm -f ap88

.PHONY: clean
