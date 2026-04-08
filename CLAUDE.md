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

4. **SF3 (Ogg Vorbis compressed SoundFonts)** — Not supported. Would allow using compressed SoundFont files. No audio quality impact. Low priority: the recommended MuseScore_General SoundFont is available as SF2 (236 MB) and packaged in the `musescore-soundfont` ebuild.
5. **Polyphony management** — Voice stealing could be smarter with release-phase awareness. Only matters when exceeding the voice limit (default 256).

### Real-time latency reduction

Completed improvements for low-latency MIDI-to-speaker response:

1. **mlockall + RT scheduling** — `mlockall(MCL_CURRENT | MCL_FUTURE)` at startup prevents page faults. `SCHED_FIFO` via `--realtime-priority=N` (now works for both `-iA` and `-ip`). PipeWire's `@pipewire` group ulimits provide the needed `memlock` and `rtprio` capabilities. Priority 50 is a good default — well below PipeWire's audio thread (88), ensuring the audio callback always preempts synthesis.

2. **Graceful underrun handling** — On buffer underrun, the PipeWire audio callback replays the last chunk with a linear fade-out instead of outputting silence. A faded repeat of ~5ms audio is perceptually invisible; a silence gap produces an audible click.

3. **Demand-driven synthesis** — The PipeWire synthesis loop (`doit()` in `pipewire_c.c`) uses `PM_REQ_OUTPUT_READY` to wait on a condvar signaled by the audio callback, replacing the old fixed `usleep`. This paces synthesis to exactly when the ring buffer needs data. The `-B` flag is ignored in interactive mode; the ring buffer is auto-sized to 3× the 256-frame fragment size. Latency is controlled entirely by PipeWire's graph quantum (`default.clock.quantum` in `pipewire.conf`). A `sched_yield()` at the end of each loop iteration prevents CPU starvation if the condvar returns immediately.

4. **PipeWire quantum config** — Ships `dist/pipewire/10-timidity-low-latency.conf` setting quantum to 128 frames (~2.7ms at 48kHz). Packagers install to `/usr/share/pipewire/pipewire.conf.avail/`; users activate via symlink to `/etc/pipewire/pipewire.conf.d/`.

5. **Kernel PREEMPT_RT** — Documented in `dist/README.md`. `CONFIG_PREEMPT_RT=y` (mainline since Linux 6.12) reduces worst-case scheduling latency from ~10ms to ~0.1ms. Ebuild checks kernel config with `linux-info` eclass warnings.

6. **Instrument preloading** — All GM bank 0 instruments (128 melodic + 128 drums) and any additional config-referenced banks are loaded at startup before entering the RT loop. Instruments are never freed during RT operation (`stop_sequencer` and `server_reset` skip `free_instruments`). This eliminates all file I/O (`fopen`/`read` in `load_instrument`) from the synthesis path, which was the root cause of audible pops on instrument changes and nondeterministic latency spikes under SCHED_FIFO. Memory cost is ~140 MB for FluidR3_GM.sf2 — the same as FluidSynth's approach of loading the entire SoundFont at startup.

Remaining low-priority items:

7. **RT-safe allocation mitigations** — Several malloc/free calls that were in the `doit()` → `play_event()` synthesis path have been eliminated:
   - **Pan delay buffers**: Changed from per-note `safe_malloc`/`free` to a fixed `int32[PAN_DELAY_BUF_MAX]` array embedded in `struct Voice` (`playmidi.h:465`). Zero heap operations per note-on/off.
   - **Reverb buffer**: Pre-allocated at startup via `init_reverb_buffer()` called from `preload_instruments()`, eliminating the lazy `safe_malloc` in `do_compute_data_midi()`.
   - **Drum parts pool**: `playmidi_pool` mblock pre-grown at startup via `pregrow_playmidi_pool()` with room for 128 DrumParts entries, so `new_segment()` in `play_midi_setup_drums()` doesn't need to call `malloc`.
   - **Effect chain nodes**: `push_effect()` and `free_effect_list()` now use a static pool of 32 `EffectList` nodes (`reverb.c`), falling back to malloc only if exhausted. The `info` blocks within each effect node are still heap-allocated.

   Remaining non-RT-safe operations (diminishing returns / high refactoring cost):
   - **Instrument loading on uncovered banks** (`playmidi.c:1338-1391`): `play_midi_load_instrument()` can still call `load_instrument()` (full disk I/O + many mallocs) if a MIDI bank select targets a bank not covered by preloading (non-GM banks not referenced in `timidity.cfg`).
   - **Effect info blocks** (`reverb.c`): `alloc_effect()` still calls `safe_malloc` for effect info structs (up to 8KB each) which contain nested delay line buffers. Eliminating these would require refactoring each effect engine.
   - **Drum effect buffers** (`playmidi.c:3332-3341`): `safe_malloc` for per-channel drum effect arrays when drum reverb/chorus/delay levels are configured.
   - **`vfprintf` + `fflush` in `cmsg()`** (`pipewire_c.c:746-748`): stdio can malloc internally; `fflush` is a blocking syscall. Filtered by verbosity at default levels but risky with `-v`.

8. **True pull-model synthesis** — Currently synthesis still runs in the main thread and pushes through a small ring buffer to the PipeWire RT callback. The ideal architecture (like FluidSynth) synthesizes directly inside `on_process()`, eliminating the ring buffer entirely. Now that instruments are preloaded, the main remaining blocker is that `play_event()` still has other non-RT-safe operations (global mutable state, UI callbacks, remaining malloc/free as listed above). Would require significant refactoring of the synthesis core.
