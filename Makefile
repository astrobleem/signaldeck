# Put the ELKS ia16-elf toolchain and ELKS tools/bin (elf2elks) on PATH.
CC = ia16-elf-gcc
ELKS ?= ../../
CPPFLAGS += -I$(ELKS)/include -I$(ELKS)/elks/include -I$(ELKS)/libc/include
CFLAGS += -Os -Wall -Wextra -Werror -march=i8086 -melks-libc -mregparmcall

.PHONY: all clean
all: elksvgm sbpcm opl2play

elksvgm: elksvgm.c ui.inc
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ elksvgm.c

sbpcm: sbpcm.c sbpcm-main.c sbpcm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ sbpcm.c sbpcm-main.c

opl2play: opl2play.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ opl2play.c

clean:
	rm -f elksvgm sbpcm opl2play
