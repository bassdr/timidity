# Distribution packaging reference files

This directory contains service files, configuration, and integration
scripts that distribution packagers can adapt. They are not installed
by `make install` — copy and adjust them for your distribution.

## Contents

| Path | Description |
|------|-------------|
| `config/timidity.cfg` | Reference config that sources patch sets from `/usr/share/timidity` and `~/.timidity` |
| `desktop/timidity.desktop` | XDG desktop entry |
| `desktop/timidity.xpm` | Application icon (32x32) |
| `openrc/timidity.initd` | OpenRC init script (supervise-daemon) |
| `openrc/timidity.confd` | OpenRC configuration defaults |
| `systemd/timidity.service` | systemd unit file |
| `udev/99-timidity-midi.rules` | udev rule: auto-connect USB MIDI devices to TiMidity in ALSA sequencer mode (`-iA`) |
| `udev/timidity-midi-connect` | Helper script called by the udev rule, uses `aconnect(1)` to link hardware MIDI ports |

## Notes

The udev rules are only useful in ALSA sequencer mode (`-iA`). In PipeWire
MIDI mode (`-ip`), auto-linking of MIDI sources is handled natively by
TiMidity with the `-ipA` option (e.g. `timidity -ipA -OW`).

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
