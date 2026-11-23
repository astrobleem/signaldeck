# Put the ELKS ia16-elf toolchain and ELKS tools/bin (elf2elks) on PATH.
CC = ia16-elf-gcc
ELKS ?= ../../
CPPFLAGS += -I$(ELKS)/include -I$(ELKS)/elks/include -I$(ELKS)/libc/include
CFLAGS += -Os -Wall -Wextra -Werror -march=i8086 -melks-libc -mregparmcall

.PHONY: all clean
all: elksvgm sbpcm opl2play 26kplay tandyplay

elksvgm: elksvgm.c ui.inc status.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ elksvgm.c

sbpcm: sbpcm.c sbpcm-main.c sbpcm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ sbpcm.c sbpcm-main.c

opl2play: opl2play.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ opl2play.c

26kplay: 26kplay.c stream.inc overlay.inc status.inc status.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ 26kplay.c

tandyplay: tandyplay.c stream.inc status.inc status.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tandyplay.c

clean:
	rm -f elksvgm sbpcm opl2play 26kplay tandyplay
