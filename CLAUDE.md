# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TiMidity++ is a software MIDI synthesizer that converts MIDI files to PCM waveform audio for real-time playback or export to audio formats (WAV, AU, AIFF, Ogg Vorbis, FLAC). It uses Gravis Ultrasound patches or SoundFont files as instrument data. Written primarily in C.

## Build Commands

```sh
# Bootstrap (regenerates configure from configure.ac, then runs configure)
./autogen.sh

# Or manually:
autoreconf -f -i
./configure
make

# Install
make install
```

Key configure options:
- `--enable-audio=<list>` — select audio outputs (e.g., jack,alsa,oss,portaudio,flac,vorbis)
- `--enable-ncurses`, `--enable-gtk`, `--enable-xaw` — select UI interfaces
- `--enable-network` — enable HTTP/FTP remote MIDI playback
- `--enable-debug` — debug build
- `--enable-dynamic-<interface>` — build interfaces as dynamically loadable modules

There is no test suite. The project is verified through compilation and manual MIDI playback testing.

## Architecture

The build is organized as a set of libraries linked into the `timidity` binary:

- **`timidity/`** — Core synthesis engine and main entry point (`timidity.c`). Contains:
  - MIDI/format readers: SMF (`readmidi.c`), MOD (via libunimod), RCP (`rcp.c`), MFi (`mfi.c`)
  - Synthesis: `playmidi.c` (sequencing), `resample.c` (sample interpolation), `mix.c` (mixing), `instrum.c` (instrument loading), `sndfont.c` (SoundFont loading)
  - Audio output drivers: `*_a.c` files — JACK (`jack_a.c`), ALSA (`alsa_a.c`), PortAudio (`portaudio_a.c`), FLAC (`flac_a.c`), Vorbis (`vorbis_a.c`), WAV (`wave_a.c`), etc.
  - Effects: `reverb.c`, `effect.c`
  - SoundFont parser: `sffile.c` (file parsing), `sndfont.c` (conversion to internal format)
- **`interface/`** — User interface implementations (ncurses, GTK+, Xaw, Tcl/Tk, Emacs, Windows GUI, ALSA sequencer, real-time synth interfaces)
- **`libarc/`** — Archive and I/O abstraction layer. Handles reading from files, URLs (HTTP/FTP), and archives (TAR, ZIP, GZIP, LZH)
- **`libunimod/`** — MOD/tracker format loaders (XM, IT, S3M, STM, FAR, etc.)
- **`utils/`** — Utility library (memory management via `mblock.c`, FFT, timers, string handling)
- **`windrv/`** — Windows MIDI driver interface

## Key Conventions

- Audio output drivers follow a naming pattern `*_a.c` and register via `PlayMode` structs in `output.c`
- Interface modules register via `ControlMode` structs
- The build uses GNU Autotools with m4 macros in `autoconf/` for library detection (ALSA, JACK, FLAC, Vorbis, etc.)
- Platform-specific code uses `#ifdef` guards (e.g., `__W32__`, `HAVE_JACK`, `AU_ALSA`)
- `autogen.sh` backs up INSTALL before running autoreconf (to preserve the project's custom INSTALL file)
