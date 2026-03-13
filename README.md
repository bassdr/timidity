# TiMidity++ — MIDI-to-WAVE converter and player

Originally by Masanao Izumo <iz@onicos.co.jp>, based on TiMidity 0.2i by Tuukka Toivonen (1995).

TiMidity++ is a software synthesizer. It plays MIDI files by converting them into PCM waveform data using digital instrument data (GUS patches or SoundFonts), synthesizing in real-time. It can also export to various audio file formats.

Distributed under the terms of the GNU General Public License.

## Features

- Plays MIDI files without external MIDI hardware
- Supported input formats:
  - SMF (Format 0, 1, 2)
  - MOD
  - RCP, R36, G18, G36 (Recomposer)
  - MFi (Version 3; Melody Format for i-Mode)
- Exports to: WAV, AU, AIFF, Ogg Vorbis, FLAC, Speex, MP3 (Windows only)
- Instrument data: GUS patches, SoundFonts, AIFF/WAV samples
- Audio output backends:
  - **PipeWire** (`-OW`) — native, low-overhead
  - JACK (`-Oj`)
  - ALSA (`-Os`)
  - PortAudio (`-Op`, `-OP`)
  - libao (`-OO`)
  - OSS (`-Od`)
- User interfaces:
  - dumb terminal, ncurses, S-Lang, vt100
  - X Athena Widget, Tcl/Tk, Motif, GTK+
  - Emacs front-end (`M-x timidity`)
  - ALSA sequencer interface (`-iA`)
  - **PipeWire MIDI synthesizer interface** (`-ip`)
  - Windows GUI, Windows synthesizer, PortMIDI synthesizer
- Network playback: HTTP, FTP, NetNews
- Archive playback: TAR, tar.gz, ZIP, LZH
- Sound spectrogram display
- Trace playing

## Building

```sh
autoreconf -f -i
./configure [options]
make
make install
```

### Configure options

Audio outputs and interfaces are selected at configure time:

```sh
# PipeWire audio output and MIDI input
./configure --enable-audio=pipewire --enable-pipewiresyn

# Multiple audio backends
./configure --enable-audio=pipewire,jack,alsa,vorbis,flac

# With ncurses UI
./configure --enable-ncurses

# Custom config/instrument path (default: /usr/local/share/timidity)
./configure --with-default-path=/usr/share/timidity
```

Key options:
| Option | Description |
|--------|-------------|
| `--enable-audio=LIST` | Audio outputs: `pipewire`, `jack`, `alsa`, `oss`, `portaudio`, `vorbis`, `flac`, `ao` |
| `--enable-pipewiresyn` | PipeWire MIDI synthesizer interface (`-ip`) |
| `--enable-alsaseq` | ALSA sequencer interface (`-iA`) |
| `--enable-ncurses` | ncurses terminal UI |
| `--enable-network` | HTTP/FTP remote MIDI playback |
| `--with-default-path=DIR` | Path to `timidity.cfg` and instrument files |

### Dependencies

PipeWire support requires `libpipewire-0.3` (detected via `pkg-config`).

## Usage

```sh
# Play a MIDI file (PipeWire output)
timidity -OW file.mid

# Play with ncurses interface
timidity -in file.mid

# Run as PipeWire MIDI synthesizer (accepts MIDI input from PipeWire graph)
timidity -ip -OW

# Run as ALSA sequencer synthesizer
timidity -iA -Os

# Specify config file explicitly
timidity -c /path/to/timidity.cfg file.mid

# Export to WAV
timidity -Ow -o output.wav file.mid
```

### PipeWire MIDI synthesizer mode

When started with `-ip`, TiMidity++ creates a MIDI sink in the PipeWire graph. Connect any MIDI source (hardware controller, DAW, virtual keyboard) to it using your PipeWire patchbay (e.g., `qpwgraph`, Helvum).

Combined with `-OW`, the entire audio path stays within PipeWire with no ALSA or libao intermediaries.

## Configuration

TiMidity++ requires a configuration file (`timidity.cfg`) pointing to instrument data (SoundFonts or GUS patches). On most distributions, this is provided by packages like `timidity-freepats` or `fluid-soundfont`.

The compiled-in default path is set by `--with-default-path` at configure time. You can override it at runtime with `-c`:

```sh
timidity -c /etc/timidity/timidity.cfg file.mid
```

## Links

- Source: https://sourceforge.net/projects/timidity/
