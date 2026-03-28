# Distribution packaging reference files

This directory contains service files, configuration, and integration
scripts that distribution packagers can adapt. They are not installed
by `make install` — copy and adjust them for your distribution.

## Contents

| Path | Description |
|------|-------------|
| `config/timidity.cfg` | Reference config that sources patch sets from `/usr/share/timidity` and `~/.timidity` |
| `config/50timidity++-gentoo.el` | Emacs integration (sets `timidity-prog-path`) |
| `desktop/timidity.desktop` | XDG desktop entry |
| `desktop/timidity-autostart.desktop.example` | XDG autostart entry example — start TiMidity as a PipeWire MIDI daemon at login |
| `desktop/timidity.xpm` | Application icon (32x32) |
| `openrc/timidity.initd` | OpenRC init script (supervise-daemon) |
| `openrc/timidity.confd` | OpenRC configuration defaults |
| `systemd/timidity.service` | systemd system service for ALSA sequencer mode (`-iA`) |
| `systemd/timidity-user.service` | systemd user service for PipeWire MIDI synthesis (`-ip -OW`) |
| `udev/99-timidity-midi.rules` | udev rule: auto-connect USB MIDI devices to TiMidity in ALSA sequencer mode (`-iA`) |
| `udev/timidity-midi-connect` | Helper script called by the udev rule, uses `aconnect(1)` to link hardware MIDI ports |

## Notes

The udev rules are only useful in ALSA sequencer mode (`-iA`). In PipeWire
MIDI mode (`-ip`), auto-linking of MIDI sources is handled natively by
TiMidity with the `-ipA` option (e.g. `timidity -ipA -OW`).

## Headless / system-wide PipeWire

The systemd user service assumes the typical per-user PipeWire session. For
headless setups (e.g. Raspberry Pi) running PipeWire system-wide without a
user session, use the OpenRC init script instead — it handles
`XDG_RUNTIME_DIR` and `PULSE_SERVER` environment variables for the system-wide
PipeWire socket (see `timidity.confd`).

## C library requirement (PipeWire driver)

The PipeWire audio driver (`pipewire_a.c`) uses `pthread_cond_broadcast`
from within a signal handler to wake the audio writer thread on
SIGINT/SIGTERM. This is not async-signal-safe per POSIX, but works
reliably on glibc (the implementation is a simple futex wake). Other C
libraries — notably musl — may not behave the same way, and could hang
or crash on signal delivery. If porting to a musl-based distribution,
this code path will need to be reworked (e.g. using a pipe or eventfd
for cross-thread signal notification).

## Tested configuration

The following has been tested on Gentoo Linux with PipeWire 1.6.2 and
a Roland A-88MK2 USB MIDI controller:

```sh
timidity -ipA -B3,10 -OW -c /usr/share/timidity/timidity.cfg
```

This runs TiMidity as a PipeWire MIDI synthesizer with auto-linking of
MIDI sources, PipeWire audio output, and low-latency buffer settings.
MIDI device hot-plug (disconnect/reconnect) requires two upstream fixes
that may not yet be in your distribution's packages:

- **PipeWire alsa-seq-bridge crash fix** — `free_port()` does not remove
  the port from `mix_list` before zeroing the struct, causing a SIGSEGV
  in `process_read()` on the data loop thread when a MIDI device is
  disconnected. See `pipewire-1.6.2-fix-midi-relink-crash.patch` in the
  [Gentoo overlay](https://github.com/bassdr/gentoo-local-overlay/tree/main/media-video/pipewire/files).

- **WirePlumber MIDI session items** — WirePlumber 0.5.x's `create-item.lua`
  only creates session items for `Stream/*`, `Audio/*`, and `Video/*` media
  classes, ignoring `Midi/*` nodes. This prevents MIDI filter nodes (like
  TiMidity's) from being visible to the session manager. See
  `wireplumber-0.5.13-midi-session-items.patch` in the
  [Gentoo overlay](https://github.com/bassdr/gentoo-local-overlay/tree/main/media-video/wireplumber/files).
  This is not Gentoo-specific — any distribution using WirePlumber 0.5.x
  needs this patch for PipeWire MIDI nodes to work properly.
