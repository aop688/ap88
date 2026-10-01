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
# 静态链接的 libraylib.a 直接引用了少量 X11 符号，而 raylib.pc 的
# Libs.private 是空的，pkg-config 不会带上 -lX11，这里补上
#（有的系统没装 x11.pc 但库文件在，找不到 .pc 时直接给 -lX11）
RAYLIB_LIBS   ?= $(shell pkg-config --libs raylib 2>/dev/null) $(shell pkg-config --libs x11 2>/dev/null || echo -lX11)
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
