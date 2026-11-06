# Pulse — POSIX terminal music player

Uses FFmpeg for playback. Linux/POSIX terminals are supported; Windows needs WSL. No C++ libraries beyond the standard library and POSIX APIs are required.

## Build and run (Ubuntu)

```bash
sudo apt install g++ ffmpeg
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic play.cpp -o play
./play "/path/to/your music"
# Include subfolders:
./play --recursive "/path/to/your music"
```

Run `./play` without a folder, then enter `mvdir /path/to/music`. Paths may contain spaces, with or without enclosing quotes. `~` and `~/...` are expanded. Initial playback starts automatically at 70% volume.

## Commands

| Command | Action |
| --- | --- |
| `theme ocean` | Switch color theme without restarting playback |
| `theme next` | Cycle through themes |
| `theme` | Show current theme |
| `themes` | List all theme names |
| `help` or `?` | Show command reference |
| `pause` | Toggle true pause/resume |
| `resume` | Resume paused playback or restart a stopped track |
| `next`, `n` | Next track in queue order |
| `previous`, `prev`, `p` | Previous track in queue order |
| `random` | Play another random track when possible |
| `track` | Show current track and its queue number |
| `tracks` | List queue starting at the current track |
| `tracks 8` | Show up to 8 tracks starting at the current track |
| `tracks -8` | Show up to 8 tracks going backward, including current |
| `tracks 0` | Same as `track` |
| `find TEXT`, `search TEXT` | Case-insensitive substring search in relative paths |
| `play 12` | Jump to queue track 12 (numbers start at 1) |
| `play TEXT` | Play a unique match; list numbered matches if ambiguous |
| `seek 90` | Jump to 1:30 |
| `seek +15`, `seek -10` | Seek forward/backward by seconds |
| `speed 1.25`, `rate 1.25` | Set decimal playback speed (0.01 to 100; 1 is normal), preserving pitch |
| `speed` | Show current playback speed |
| `volume 45`, `vol 45` | Set volume to 45% |
| `volume` | Show volume |
| `mute` | Toggle between silence and 70% |
| `shuffle` | Shuffle queue while preserving current track |
| `sort` | Sort queue by path while preserving current track |
| `forward` | Traverse queue forward |
| `reverse` | Toggle reverse traversal |
| `once` | Toggle one extra replay of the current track |
| `loop` | Toggle permanent repeat of the current track |
| `mode` | Show direction and repeat mode |
| `mvdir PATH`, `open PATH` | Load a different folder and start playback |
| `display` | Toggle a separate 640 × 240 ffplay waveform window |
| `notify` | Toggle track-change announcements |
| `stop`, `quit`, `exit`, `q` | Exit the application |
| `null` | No operation (compatibility) |

The queue wraps at either end. `next` and `previous` always mean queue order; `reverse` changes automatic advancement. `once` and `loop` are mutually exclusive and preserve direction. A manual track change clears pending repeat-once. Lists wrap once at most and never duplicate tracks. On the dashboard only the latest five messages are retained; pipe output to capture a complete long track list.

## Appearance

```bash
./play --theme ocean "/path/to/music"
# Set a preferred startup theme in your shell:
export PULSE_THEME=amber
./play "/path/to/music"
# Disable colors while keeping layout and bold text:
NO_COLOR=1 ./play "/path/to/music"
```

Themes use standard ANSI 256-color escapes, supported by common Linux terminal emulators. `mono` and `NO_COLOR` retain bold emphasis without setting text/background colors. Piped output stays plain. Theme choices last for the session; `--theme` overrides `PULSE_THEME` on startup.

The record appears at widths of at least 70 columns and heights of at least 23 rows. The queue and recent feedback contract in smaller terminals; a standard 80 × 24 terminal shows one upcoming track and the latest feedback. Larger terminals show more. Commands remain separate from now playing, with an explicit instruction to type one at the `command >` prompt and press Enter. PLAYING is green, PAUSED is amber, and errors are red (palette shades vary by theme). Feedback uses explicit INFO/ERROR labels, including in monochrome mode.

## Playback details and limits

Elapsed time is estimated from a monotonic clock, not read from the audio device, so startup latency can cause a small discrepancy. Duration uses optional `ffprobe`; without it, playback works and total duration is shown as unknown. A probe times out after two seconds.

Seeking, speed changes, volume changes, and waveform toggling restart ffplay at the estimated current position, preserving the paused state. Expect a brief interruption and possible small seek error for some formats. Changing volume or speed while paused is supported. Progress and seeking use the original track timeline, with elapsed position advancing at the selected speed. Speed values use double precision rather than fixed presets; invalid, nonfinite, zero, negative, and out-of-range values are rejected. Chained FFmpeg `atempo` filters preserve pitch; extreme speed changes can degrade sound quality. Speed affects audio; MP4 video timing is not adjusted. The display is a separate window, not embedded in the terminal. Use commands in the terminal to control the player; controls in the ffplay window can make the dashboard's estimated state diverge.

Command history exists only during the current session. Queue numbers change after shuffle/sort. Basic line editing operates on bytes, so cursor editing is best with ASCII commands; non-ASCII paths can be pasted and used. No metadata tags, album art, persistent playlists, or gapless playback are included. The original `!cmd` arbitrary-shell shortcut was removed; use your normal shell for shell commands.

## Checks

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic play.cpp -o play
python3 test_player.py
python3 test_terminal.py
```

The integration checks use a temporary fake backend to verify pause/resume, seek, volume, shuffle identity, literal filenames, search, invalid input, repeat-once direction, failure limits, cleanup, and process isolation. Terminal checks cover all six themes, ASCII art, command separation, four screen sizes, error highlighting, plain output, NO_COLOR, and terminal restoration. `play.original.cpp` preserves your uploaded version for reference.
