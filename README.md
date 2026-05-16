# BukuLoops

Portable groovebox and beat station. Runs native on Linux handhelds (R36S, RG35XX, Miyoo) and desktop. Single-file C with SDL2 — compiles anywhere SDL2 runs. Emscripten web build coming soon.

<!--
![Step Sequencer](screenshots/step-page.png)
![FX Mixer](screenshots/fx-detail.png)
![Sample Browser](screenshots/browser.png)
![Perform Mode](screenshots/perform.png)
-->

## What It Does

BukuLoops is a self-contained beat-making environment designed for small screens and gamepad input. Load samples, sequence beats across multiple tracks, route through a 3-bus mixer with 7 effects per bus, chain patterns into arrangements, and perform live. Everything saves to portable session files.

## Features

### Sequencer
- 16/32/64 step patterns, 8 tracks per pattern, 16 patterns
- Multi-track playback — one pattern contains the full groove
- 3D step buttons with per-track color coding (8-color palette)
- Per-step probability, per-step pitch, per-step note randomization
- Accent steps trigger rolls (double hit at half-step)
- Horizontal scroll for extended sequences

### Samples & Chopping
- Load WAV files from folders — auto-categorized in a tile-grid browser
- Chop any sample into 4/8/16/32 slices
- Three chop modes: **CUT** (tight), **THRU** (play to end), **LOOP** (sustain)
- Waveform visualizer shows active slice per step
- Left stick scrubs slices with instant preview
- Chromatic pitch shifting on all samples and chops

### Synthesizers
- 10 built-in engines: 808, Kick, Hat, Snare, Clap, Lead, Pluck, Bass, Bell, Zap
- All engines respond to pitch, LFO, attack/decay shaping
- 3 auxiliary layers per track (stack sounds, add sub bass, etc.)

### Mixer & Effects
Three independent FX buses. Route any track to any bus.

| Effect | What It Does |
|--------|-------------|
| **Drive** | Tanh saturation |
| **Filter** | Biquad lowpass with resonance |
| **Bitcrusher** | Sample rate + bit depth reduction |
| **Delay** | Echo with feedback and wet mix |
| **Reverb** | Schroeder reverb (4 comb + 2 allpass) |
| **Granular** | Cloud generator — density, grain size, pitch scatter |
| **Grossbeat** | Time manipulation — halftime, stutter, reverse, gate |

Each effect has animated visualizations and dual-knob control.

### Performance
- Pattern chaining (build sequences like 1→2→1→3)
- Three modes: manual, chain, random
- Queue patterns for next bar or switch instantly
- Duplicate and remix patterns on the fly

### Session Management
- Save / load / clone / rename songs
- Gamepad text input for naming
- Auto-save on exit
- Portable `.bukuloop` session files

### Themes
Four color themes — Gunmetal, Matrix, Midnight, Ember. Switch live in settings.

## Controls

Designed for gamepad. Every feature is accessible without a keyboard.

<details>
<summary><strong>Full control reference</strong></summary>

### Step Page
| Input | Action |
|-------|--------|
| DPAD L/R | Move step cursor |
| DPAD U/D | Switch tracks |
| A | Toggle step |
| SEL+A | Accent (roll) |
| A on label | Cycle bus: DIR → A → B → C |
| B / SEL+B | Add / delete track |
| X | Piano roll |
| Y | Sample browser |
| L3 | Cycle chop mode |
| Left Stick | Scrub chop slices |
| R3 | Cycle note randomizer |
| Right Stick Y | Step probability |
| Right Stick X | Scroll view |
| L2/R2 | Cycle source |

### Perform Page
| Input | Action |
|-------|--------|
| A | Queue / chain add |
| X | Instant switch |
| B | Cycle mode |
| Y / SEL+Y | New / duplicate pattern |

### FX Page
| Input | Action |
|-------|--------|
| DPAD | Navigate tracks/effects |
| L/R | Adjust parameter |
| SEL+L/R | Secondary knob |
| SEL+A | Toggle effect |
| X | Mixer ↔ detail |
| Y | Swap bus |

### Global
| Input | Action |
|-------|--------|
| START | Play/pause |
| START+B | Song menu |
| START+A | Quick save |
| SEL+START | Exit |

</details>

## Building

### Native (Linux / macOS)
```bash
make
# or directly:
gcc -O2 -o bukuloops jungledaw.c $(sdl2-config --cflags --libs) -lm
```

### Web (Emscripten) — coming soon
```bash
emcc jungledaw.c -s USE_SDL=2 -O2 -o bukuloops.html
```

### Deploy to handheld
```bash
scp bukuloops user@device:/roms/ports/bukuloops/
```

## Samples

Drop `.wav` files into a `samples/` folder next to the binary. Organize into subfolders for automatic categorization in the browser. Supports up to 512 samples (48kHz mono, 6 sec max per file).

## Technical Details

- **Language:** C99, single file (~5000 lines)
- **Dependencies:** SDL2, libc, libm
- **Audio:** 48kHz mono, 512-sample buffer, real-time callback
- **Rendering:** Immediate-mode SDL2 rects, custom 5x7 bitmap font
- **Voices:** 12 polyphonic with cut groups
- **FX:** 3 buses × 7 effects, biquad filter, Schroeder reverb, granular synthesis
- **Platform:** Linux (ARM/x86), macOS, Windows, Emscripten (planned)

## License

MIT
