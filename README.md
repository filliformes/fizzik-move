# Fizzik

**Polyphonic simplex physical-modeling synth** for [Ableton Move](https://www.ableton.com/move/),
built for the [Schwung](https://github.com/charlesvestal/schwung) framework.

Fizzik synthesizes sound the way physical objects do — by *exciting* simulated resonators and
letting them ring. Each voice runs an excitation burst into **two coupled resonators** (string,
beam, plate, or membrane). A single **Couple** knob cross-feeds them, so complex, emergent
timbres arise from the interaction of two simple structures rather than from stacking modules —
the *simplex* idea.

Distilled from [MechanOdd](https://github.com/odoare/Mechanodd) by odoare (FX-Mechanics); the
DSP is reimplemented independently in C for the Move.

## Features

- 6-voice polyphony
- Two independent, selectable resonators per voice:
  - **String** — dispersive digital waveguide with **tension-modulation** (hard plucks glide up in pitch)
  - **Beam** — modal morph from harmonic string to inharmonic flexural beam
  - **Plate** — metallic Kirchhoff-plate modes
  - **Membrane** — drum-like 2D-wave modes
- **Couple** knob — cross-feedback between the two resonators (bounded, self-limiting)
- Exciter blends noise, a mallet cycle, and crackle through a resonant filter
- 16 named presets + Rnd Patch / Rnd Exciter / Rnd Reson
- Stereo spread + built-in Space reverb

## Controls

| Page | Knobs |
|------|-------|
| **Exciter** | Exc Mix · Crackle · Color · Attack · Decay · Exc Reso · Vel→Level · Vel→Color |
| **Reson A** | Model · Structure · Decay · Damp · Position · Tone · Tune · Tension |
| **Reson B** | (same eight, for the second resonator) |
| **Voice** | Couple · Balance · Glide · Amp Atk · Amp Rel · Spread · Space · Level |
| **Patch** | Preset · Rnd Patch · Rnd Exciter · Rnd Reson |

Use the jog wheel to move between pages; knobs 1–8 edit the current page.

## Building

```
./scripts/build.sh
```

Requires Docker (ARM64 cross-compile via `aarch64-linux-gnu-gcc`).

## Installation

```
MOVE_HOST=move.local ./scripts/install.sh
```

Or install via the Module Store in Schwung. Power-cycle the Move after installing so it picks
up the module metadata.

## Credits

Inspired by **MechanOdd** by [odoare](https://github.com/odoare) (FX-Mechanics). Physical-modeling
algorithms reimplemented from published references: J. O. Smith, *Physical Audio Signal
Processing* (CCRMA); Tolonen/Välimäki/Karjalainen, *Tension Modulation Nonlinearity in Plucked
Strings*; the RBJ Audio-EQ Cookbook; and standard modal synthesis.

## License

MIT — see [LICENSE](LICENSE).
