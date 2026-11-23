# Signal Deck for ELKS

An 80×25 keyboard player for real-mode ELKS. The PC-98 and Tandy
screens show actual key/volume state sent to the selected chip, not a
spectrum or an estimate of sound pressure. The two Sound Blaster paths
are separate: digital PCM through ELKS or melodic OPL2 at port 388h.

## Start the deck

Copy the executables to ELKS (`/bin/elksvgm`, plus `/bin/sbpcm` for PCM and
`/bin/opl2play` for OPL2). Make them executable and copy the appropriate
tracks to a directory on the guest. The `elksvgm` executable also runs
from a copied folder as `./elksvgm`; PCM and OPL2 still need their
respective helpers in `/bin`. Run from an ANSI-enabled 80×25 console:

```sh
elksvgm --deck --pc98-26ks track1.vgm track2.vgm
elksvgm --deck --pc98-26k track.vgm
elksvgm --deck --tandy-ex track.vgm
elksvgm --deck --sb-pcm track1.wav track2.wav
elksvgm --deck --adlib-388 track.vgm
```

Use the hardware mode that matches the installed, idle device. File
case should match the names on the guest filesystem. Press **Enter**
to play the selected track from the start, **S** to stop, **P/N** to
switch tracks, and **Q** to quit. Switching during playback stops the
current track and starts the newly selected track. Tracks do not
automatically advance at the end.

- **PC-9801-26/K**: raw, single YM2203 stream at 3.9936 MHz. Use
  `--pc98-26ks` for FM and fixed-volume SSG tone/noise, or
  `--pc98-26k` for FM only. FM-only tracks work in either queue. The
  SSG mode preserves GPIO direction, does not touch GPIO data ports,
  and refuses timer or hardware-envelope commands. It requires successful
  SSG readback before playback.
- **Tandy 1000 EX**: raw single-chip SN76489-style PSG stream at 3.579545 MHz.
- **Sound Blaster PCM**: uncompressed 8-bit unsigned mono WAV, 4–20 kHz,
  through a working ELKS `/dev/dsp`. Use a kernel with its Sound Blaster PCM driver.
- **AdLib / Sound Blaster OPL2**: raw single YM3812 melodic stream at
  3.579545 MHz and a confirmed idle OPL2-compatible interface at 388h;
  no rhythm mode or OPL3 features. This is not a PCM mode.

VGM is limited to 8 KiB and two minutes, without loop replay, VGZ,
data blocks or multi-chip playback. PCM is limited to four minutes.
The child-based PCM and OPL2 views do not report live meters or a clock.
The deck does not pause, seek, mute individual channels or fade.

These backends were exercised in DOSBox-X booting real-mode ELKS.
The combined player has not yet been verified on physical hardware.

Without `--deck`, the PC-98 and Tandy arguments retain one-shot CLI
playback: `elksvgm --pc98-26k track.vgm` and
`elksvgm --tandy-ex track.vgm`.

Build with the ELKS ia16-elf toolchain and `elf2elks` on PATH:
`make ELKS=/path/to/elks`.

This repository contains source only. Music files are not included.
