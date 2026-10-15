# Reconstruction build. Pick the compiler that matches the original:
#   CC=i686-w64-mingw32-gcc   (32-bit Windows PE)
#   CC=x86_64-w64-mingw32-gcc (64-bit Windows PE)
#   CC=gcc                    (native Linux ELF)
CC ?= gcc
CFLAGS ?= -O2 -g
SRC := $(wildcard src/*.c)
OUT ?= build/recomp

$(OUT): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -rf build

.PHONY: clean
