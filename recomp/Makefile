# Native jak_batm runtime.
#   build/batman-rt  headless runner for tests/check.sh (no dependencies)
#   build/batman     SDL2 front end
#   ../mods/*/plugin.so  native mod plugins (mod_api.h), from ../mods/*/plugin.c
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -pthread
LDLIBS ?= -lm -pthread -ldl
CORE := $(filter-out src/main.c src/sdl_main.c,$(wildcard src/*.c))
HDR := $(wildcard src/*.h)
OUT ?= build/batman-rt
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)

PLUGINS := $(patsubst %.c,%.so,$(wildcard ../mods/*/plugin.c ../tests/mods/*/plugin.c))

all: $(OUT) build/batman $(PLUGINS)

$(OUT): $(CORE) src/main.c $(HDR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(CORE) src/main.c $(LDLIBS)

build/batman: $(CORE) src/sdl_main.c $(HDR)
	mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(CORE) src/sdl_main.c $(SDL_LIBS) $(LDLIBS)

%/plugin.so: %/plugin.c src/mod_api.h
	$(CC) $(CFLAGS) -shared -fPIC -Isrc -o $@ $<

clean:
	rm -rf build $(PLUGINS)

.PHONY: all clean
