# BukuLoops

A portable groovebox and beat station for the R36S retro handheld (RK3326 / dArkOSRE). Single-file C, SDL2, runs at 48kHz mono. Built for gamepad-only workflow.

## Features

### Step Sequencer
- 16/32/64 step patterns with up to 8 tracks per pattern
- PO-33 style workflow: one pattern = full groove, all tracks play simultaneously
- 3D raised-button step grid with per-track color coding
- Per-step note randomization (up / up+down / down) with scale-aware pitch selection
- Per-step probability (0-100%)
- Accent = roll (double trigger at half-step intervals)
- Horizontal scrolling for extended patterns with smooth camera follow

### Sample Engine
- WAV sample loading with automatic folder categorization
- Two-panel icon grid browser (folders left, tile grid right)
- Sample pitch shifting (chromatic, per-step)
- Chop/slice system: 4/8/16/32 slices with waveform visualizer
- Three chop modes: CUT (stop at boundary), THRU (play to end), LOOP (repeat slice)
- Left stick scrubs through slices with live preview
- Track self-cut (each track kills its previous voice on retrigger)

### Synthesizers
- 10 built-in instruments: 808, Kick, Hat, Snare, Clap, Lead, Pluck, Bass, Bell, Zap
- All synths respond to pitch changes
- LFO modulation (rate + depth per track)
- Attack/decay envelope shaping
- 3 auxiliary layers per track with independent source/pitch/gain

### Mixer & Effects
- 3 FX buses (A / B / C) with per-bus effect chains
- Track-to-bus routing from the step page or mixer view
- **Drive** — tanh saturation with visual knob
- **Filter** — biquad lowpass with resonance (stable at all settings)
- **Bitcrusher** — sample rate reduction + bit depth quantization
- **Delay** — tempo-synced echo with feedback and wet mix
- **Reverb** — Schroeder algorithm (4 comb + 2 allpass filters) with damping
- **Granular** — cloud generator with density, grain size, pitch scatter controls
- **Grossbeat** — time manipulation (halftime, stutter 1/8, stutter 1/16, reverse, gate)
- Per-effect toggle, dual-knob control, animated visualizations per effect panel

### Performance
- Pattern chaining: build sequences (1>2>1>3), loop or one-shot
- 3 playback modes: manual, chain, random
- Pattern queue (next bar boundary) or instant switch
- Pattern duplication from perform page
- BPM control (60-220), swing, master pitch

### Song Management
- Save/load/new/clone with gamepad text input
- Session persistence (all patterns, tracks, bus FX, settings)
- Auto-save on exit

### Themes
- 4 color themes: Gunmetal, Matrix, Midnight, Ember
- Theme-aware rendering across all pages
- Live switching in settings

## Controls

### Step Page
| Input | Action |
|-------|--------|
| DPAD Left/Right | Move cursor (enters label area at left edge) |
| DPAD Up/Down | Switch tracks |
| A | Toggle step on/off |
| SEL+A | Toggle accent (roll) |
| A (on label) | Cycle bus routing: DIR > A > B > C |
| B | Add new track |
| SEL+B | Delete current track |
| X | Open piano roll |
| Y | Open sample browser |
| SEL+Y | Open synth lab |
| L3 (left stick click) | Cycle chop mode |
| SEL+L3 | Cycle chop mode backward |
| Left Stick X | Scrub chop slices |
| R3 (right stick click) | Cycle note randomizer |
| Right Stick Y | Adjust step probability |
| Right Stick X | Smooth horizontal scroll |
| L2/R2 | Cycle track source |
| L1/R1 | Switch page |
| START+X | Duplicate/extend pattern |

### Perform Page
| Input | Action |
|-------|--------|
| DPAD Up/Down | Navigate patterns |
| A | Queue pattern / Add to chain |
| X | Switch pattern immediately |
| B | Cycle mode (manual > chain > random) |
| Y | New pattern |
| SEL+Y | Duplicate pattern |
| SEL+A | Toggle chain loop |
| SEL+X | Clear chain |

### FX Page
| Input | Action |
|-------|--------|
| DPAD Up/Down | Select track (mixer) or effect (detail) |
| A | Cycle bus (mixer) / Big nudge (detail) |
| X | Enter bus detail / Back to mixer |
| Left/Right | Adjust parameter |
| SEL+Left/Right | Adjust secondary parameter |
| SEL+A | Toggle effect on/off / Mute bus |
| Y | Swap bus |
| L2/R2 | Fine adjust |

### Global
| Input | Action |
|-------|--------|
| START | Play/pause (double-tap = rewind) |
| START+B | Open song/settings menu |
| L1/R1 (in menu) | Switch to settings |
| START+A | Quick save |
| SEL+START | Exit |

## Building

```bash
# Native build
gcc -o bukuloops src/jungledaw.c $(sdl2-config --cflags --libs) -lm -O2

# Deploy to R36S over SSH
scp bukuloops user@device:/roms/ports/jungledaw/
```

## Sample Packs

Place `.wav` files in the `samples/` directory next to the binary. Subdirectories become categories in the browser. Supports up to 512 samples. WAV files are converted to 48kHz mono float32 on load (max 6 seconds per sample).

## Session Format

Sessions are saved as `.bukuloop` text files:
- `[session]` — BPM, pitch, scale, swing, theme
- `[bus]` — per-bus FX state
- `[pattern]` + `[track]` — pattern/track data with steps, notes, probability, chop settings
- `[clip]` / `[beat]` — arrangement data

## Hardware

Built and tested on:
- **R36S** — RK3326 quad-core ARM Cortex-A35, 1GB RAM, 640x480
- **dArkOSRE** — Debian-based Linux
- **Audio** — 48kHz mono, 512-sample buffer

Runs on any Linux/macOS/Windows with SDL2 and a gamepad.

## Architecture

Single-file C (~5000 lines). No dependencies beyond SDL2 and libc/libm.

- 12 polyphonic voices with sample/synth rendering and cut groups
- 3 FX buses with 7-effect chains (drive, filter, crush, delay, reverb, granular, grossbeat)
- 16 patterns x 8 tracks x 64 steps
- Immediate-mode SDL2 rendering with custom bitmap font
- Real-time audio callback with thread-safe RNG

## License

MIT
