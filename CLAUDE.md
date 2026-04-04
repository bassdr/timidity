# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TiMidity++ is a software MIDI synthesizer that converts MIDI files to PCM waveform audio for real-time playback or export to audio formats (WAV, AU, AIFF, Ogg Vorbis, FLAC). It uses Gravis Ultrasound patches or SoundFont files as instrument data. Written primarily in C.

## Build Commands

```sh
# Full rebuild from scratch
autoreconf -f -i
./configure --enable-audio=pipewire,jack,alsa,vorbis,flac \
            --enable-pipewiresyn \
            --enable-ncurses \
            --with-default-path=/usr/share/timidity
make

# Install
make install
```

Key configure options:
- `--enable-audio=<list>` — select audio outputs (e.g., `pipewire,jack,alsa,oss,portaudio,flac,vorbis,ao`)
- `--enable-pipewiresyn` — PipeWire MIDI synthesizer interface (`-ip`)
- `--enable-alsaseq` — ALSA sequencer interface (`-iA`)
- `--enable-ncurses`, `--enable-gtk`, `--enable-xaw` — select UI interfaces
- `--enable-network` — enable HTTP/FTP remote MIDI playback
- `--with-default-path=DIR` — path to `timidity.cfg` and instrument files
- `--enable-debug` — debug build

There is no test suite. Verify through compilation and manual MIDI playback testing.

## Architecture

The build produces a single `timidity` binary from libraries in these directories:

- **`timidity/`** — Core synthesis engine and main entry point (`timidity.c`). Contains:
  - MIDI/format readers: SMF (`readmidi.c`), MOD (via libunimod), RCP (`rcp.c`), MFi (`mfi.c`)
  - Synthesis: `playmidi.c` (sequencing), `resample.c` (sample interpolation), `mix.c` (mixing), `instrum.c` (instrument loading), `sndfont.c` (SoundFont loading)
  - Audio output drivers (`PlayMode`): `*_a.c` files — PipeWire (`pipewire_a.c`), JACK (`jack_a.c`), ALSA (`alsa_a.c`), PortAudio (`portaudio_a.c`), etc.
  - Effects: `reverb.c`, `effect.c`
- **`interface/`** — User interface / MIDI input implementations (`ControlMode`): ncurses, GTK+, Xaw, ALSA sequencer, PipeWire MIDI (`pipewire_c.c`), Windows GUI, etc.
- **`libarc/`** — Archive and I/O abstraction (files, URLs, TAR/ZIP/GZIP/LZH)
- **`libunimod/`** — MOD/tracker format loaders (XM, IT, S3M, etc.)
- **`utils/`** — Utility library (memory management, FFT, timers, string handling)

### PlayMode vs ControlMode

These are the two plugin extension points:

- **`PlayMode`** (audio output, `-O` flag): Drivers in `timidity/*_a.c`. Struct has `open_output`, `close_output`, `output_data`, `acntl`, `detect`. Registered in `output.c:play_mode_list[]`. Each has a single-character ID (e.g., `'W'` for PipeWire, `'j'` for JACK).

- **`ControlMode`** (user interface / MIDI input, `-i` flag): Implementations in `interface/*_c.c`. Struct has `open`, `close`, `pass_playing_list`, `read`, `cmsg`, `event`. Registered in `controls.c:ctl_list[]`. Each has a single-character ID (e.g., `'p'` for PipeWire MIDI, `'A'` for ALSA seq).

### Push vs Pull audio bridging

TiMidity pushes audio via `output_data(buf, bytes)`. Callback-based backends (PipeWire, JACK) pull audio in their own thread. This is bridged with a ring buffer: `output_data` writes to the ring buffer (blocking when full), the audio callback reads from it.

### Adding a new audio output

1. Create `timidity/<name>_a.c` with a `PlayMode` struct
2. Add extern + entry in `timidity/output.c:play_mode_list[]` guarded by `#ifdef AU_<NAME>`
3. Add `<name>_a.c` to `EXTRA_timidity_SOURCES` in `timidity/Makefile.am`
4. Add detection block in `configure.ac` that sets `SYSEXTRAS`, `EXTRALIBS`, `EXTRADEFS`

### Adding a new interface

1. Create `interface/<name>_c.c` with a `ControlMode` struct and `interface_<id>_loader()` function
2. Add extern + entry in `timidity/controls.c` guarded by `#ifdef IA_<NAME>`
3. Add `<name>_c.c` to `EXTRA_libinterface_a_SOURCES` in `interface/Makefile.am`
4. Add `#undef IA_<NAME>` to `interface.h.in`
5. Add `CONFIG_INTERFACE(...)` block in `configure.ac`
6. If the interface can run without file arguments, add its ID char to `INTERACTIVE_INTERFACE_IDS` in `timidity/timidity.c`

### Config file loading

`timidity_pre_load_configuration()` in `timidity.c` loads the compiled-in `CONFIG_FILE` path before command-line options are parsed. The call uses `allow_missing_file=1` so that `-c /path/to/file.cfg` works even if the default config doesn't exist.

## Key Conventions

- Audio output drivers: `*_a.c`, define guard `AU_<NAME>`, register in `output.c`
- Interface modules: `*_c.c`, define guard `IA_<NAME>`, register in `controls.c`
- Build uses GNU Autotools with m4 macros in `autoconf/` for library detection
- Platform-specific code uses `#ifdef` guards (e.g., `__W32__`, `AU_PIPEWIRE`, `IA_PIPEWIRESYN`)
- `autogen.sh` backs up INSTALL before running autoreconf (to preserve the project's custom INSTALL file)

## PipeWire Support

Two new modules add native PipeWire support:

- **`timidity/pipewire_a.c`** — Audio output driver (`-OW`, id `'W'`, define `AU_PIPEWIRE`). Uses `pw_stream` with `SPA_AUDIO_FORMAT_S16_LE`. Supports S16, S24, U8, mono/stereo.

- **`interface/pipewire_c.c`** — MIDI synthesizer interface (`-ip`, id `'p'`, define `IA_PIPEWIRESYN`). Uses `pw_filter` with a MIDI sink port. Appears as "TiMidity MIDI Sink" in the PipeWire graph. Handles Note On/Off, CC, Program Change, Pitch Bend, Channel/Key Pressure, SysEx.

Both require `libpipewire-0.3` (pkg-config).

## Why Not Multithreaded Voice Mixing

Parallelizing voice mixing was considered but rejected. The per-voice pipeline (resample→filter→mix) looks independent but shares too much mutable state:

- **Shared static buffers**: `resample_buffer` (`resample.c:430`) and `filter_buffer` (`mix.c:161`) are single static arrays reused by every voice call. Parallelism would require per-thread copies.
- **Side effects in the mix loop**: `free_voice()` is called from within `mix_voice()` (on `VOICE_DIE`), mutating global `voice[]` state and freeing memory. `ctl_note_event()` (UI notification) is also called inline.
- **Channel buffer routing races**: In the DSP effects path, multiple voices on the same MIDI channel accumulate into the same `vpblist[ch]` buffer. In the non-DSP path, non-reverb voices all target `buffer_pointer`. Concurrent writes would race.
- **Small payoff**: `AUDIO_BUFFER_SIZE` (~4096 samples, 32KB stereo) fits in L1. SIMD already accelerates the inner loops. Thread synchronization overhead (barrier per ~5ms audio buffer) would likely dominate for typical voice counts.

## Roadmap

### Synthesis improvements (vs FluidSynth)

Completed improvements that close the major audible gaps with FluidSynth:

1. **SF2 modulator support** — PMOD/IMOD chunks are parsed in `sffile.c`, and `sndfont.c` resolves modulators through the full SF2 layer hierarchy (defaults → instrument global → instrument zone → preset additions) into existing Sample fields (`vel_to_fc`, `key_to_fc`, `vel_to_resonance`, `envelope_velf[]`, `modenv_velf[]`). For FluidR3_GM.sf2 specifically, all default modulators resolve correctly; the 738 custom instrument modulators are no-ops (amount=0 with secondary sources). Only 4 instruments have concave-curve modulators that aren't yet handled — negligible impact.
2. **Filter envelope modulation** — The modulation envelope (`env1ToFilterFc`, `modenv_to_pitch`, LFO modulations) is now enabled by default via `--enable-mod-envelope` at configure time (sets `MODULATION_ENVELOPE_ALLOW`). The runtime code in `playmidi.c` and `mix.c` was already complete; it was just disabled by default. Can still be toggled at runtime with `-Ee`/`-EE` or `--[no-]mod-envelope`. FluidR3_GM.sf2 has 51 instruments using `env1ToFilterFc`, with values up to ±12000 cents — these now get proper dynamic filter sweeps instead of static baked-in cutoff adjustments.
3. **Reverb/chorus quality** — Freeverb improved with: character-dependent damping (rooms absorb more HF than halls), modulated comb filters (slow per-comb LFO breaks up metallic ringing), DC blocking filter after the comb/allpass chain, and doubled stereo spread (rate-scaled). Chorus improved with: sine LFO (smoother than triangle), dual taps per channel at 120-degree phase offsets for richer ensemble effect. The standard (non-Freeverb) reverb is unchanged.

Remaining low-priority items (diminishing returns):

4. **SF3 (Ogg Vorbis compressed SoundFonts)** — Not supported. Would allow using compressed SoundFont files. No audio quality impact.
5. **Polyphony management** — Voice stealing could be smarter with release-phase awareness. Only matters when exceeding the voice limit (default 256).
