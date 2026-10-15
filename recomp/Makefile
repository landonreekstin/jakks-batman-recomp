# Native jak_batm runtime.
#   build/batman-rt  headless runner for tests/check.sh (no dependencies)
#   build/batman     SDL2 front end
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS ?= -lm
CORE := $(filter-out src/main.c src/sdl_main.c,$(wildcard src/*.c))
HDR := $(wildcard src/*.h)
OUT ?= build/batman-rt
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)

all: $(OUT) build/batman

$(OUT): $(CORE) src/main.c $(HDR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(CORE) src/main.c $(LDLIBS)

build/batman: $(CORE) src/sdl_main.c $(HDR)
	mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(CORE) src/sdl_main.c $(SDL_LIBS) $(LDLIBS)

clean:
	rm -rf build

.PHONY: all clean
