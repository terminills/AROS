SHELL := /Development/bin/sh
CC := /Development/bin/clang
SDKFLAGS := --sysroot=/Development -B/Development/bin -mcmodel=large -mno-red-zone -nix
CFLAGS := -std=c11 -O2 -Wall -Wextra -fno-color-diagnostics
.PHONY: all run clean
all: HelloAROS
HelloAROS: main.o greeting.o
	$(CC) $(SDKFLAGS) -Wl,--ld-path=/Development/bin/ld $^ -o $@
%.o: %.c greeting.h
	$(CC) $(SDKFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
run: HelloAROS
	./HelloAROS
clean:
	/Development/bin/rm -f main.o greeting.o main.d greeting.d HelloAROS
-include main.d greeting.d
