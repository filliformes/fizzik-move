# Fizzik

**Polyphonic simplex physical-modeling synth** for [Ableton Move](https://www.ableton.com/move/),
built on the [Schwung](https://github.com/charlesvestal/schwung) framework.

Fizzik makes sound the way physical objects do: a short **excitation** (a strike, a pluck, a
breath of noise) is injected into **two simulated resonators** — strings, beams, plates,
membranes — and what you hear is the object ringing. A single **Couple** knob cross-feeds the
two resonators into each other, so complex, emergent timbres arise from the *interaction* of
two simple structures. That's the *simplex* idea: richness from simplicity.

Distilled from [MechanOdd](https://github.com/odoare/Mechanodd) by odoare (FX-Mechanics);
all DSP reimplemented independently in C for the Move.

---

## Quick start

1. Install via the Schwung Module Store (or `MOVE_HOST=move.local ./scripts/install.sh`),
   power-cycle the Move, and add **Fizzik** as a sound generator.
2. You land on the **Patch** page: browse **Preset** (knob 1), or roll the dice —
   **Rnd Patch** (k2) for a new instrument, **Rnd All** (k5) to also randomize FX and mods.
3. Play the pads. **Press into a held pad** — polyphonic aftertouch makes notes bloom,
   swell, and sing (pick a feel with **AT Preset** on the Aftertouch page).
4. Sculpt live with the global **filter** (Patch knobs 6–8) — 12 analog voicings.

---

## Architecture

```
             ┌──────────── Couple (cross-feedback) ────────────┐
             ▼                                                  ▼
MIDI ─► EXCITER ──┬──► RESONATOR A ──┐
        noise      │                  ├─► Balance ─► Amp Env ─► Pan ─┐
        mallet     └──► RESONATOR B ──┘                              │  × 6 voices
        crackle                                                      ▼
                                             ┌───────────────────────┘
                                             ▼
        FILTER (12 voicings) ─► Drive ─► EQ ─► Chorus ─► Delay ─► Reverb
              ─► Width ─► Glue Comp ─► Soft-Clip ─► Lookahead Limiter ─► OUT
```

- **6-voice polyphony.** Each voice: one exciter, two independent resonators, coupling,
  amp envelope, stereo spread. Voice stealing picks the quietest voice.
- Every coupled feedback node is guarded (DC-blocker + soft-limit) so coupling is
  self-limiting — it saturates like a real object instead of blowing up.
- All continuous knobs are smoothed (~20 ms, analog-style): no zipper, no clicks.

### The resonator models

| Model | Physics | Character | Structure knob |
|---|---|---|---|
| **String** | Dispersive digital waveguide with tension modulation | Plucks, harps, basses, sitars | Dispersion (stiffness → inharmonic shimmer) |
| **Beam** | 1D modal morph `f₀·n·√((1−b)+b·n²)` | Kalimbas, xylophones, music boxes | String→beam morph (harmonic → n² partials) |
| **Plate** | Kirchhoff plate modes `∝ (m/a)²+(n/b)²` | Bells, gongs, metallic shimmer | Aspect ratio (square → long) |
| **Membrane** | 2D wave modes `∝ √((m/a)²+(n/b)²)` | Drums, toms, skins | Aspect ratio |

**Tension** (String & Beam): tension-modulation nonlinearity (Tolonen/Välimäki) —
hard hits transiently *raise* the pitch, which settles as the note decays: the
pitch-glide of a hard-plucked string.

---

## Pages & parameters

Jog wheel navigates pages; knobs 1–8 edit the current page.

### 1 · Patch (home page)

| # | Param | What it does |
|---|---|---|
| 1 | **Preset** | 30 factory presets (click-free switching, even mid-note) |
| 2 | **Rnd Patch** | New random instrument: exciter + both resonators + couple/balance |
| 3 | **Rnd Exciter** | Randomize only the strike — audible on the *next* note |
| 4 | **Rnd Reson** | Randomize only the two resonators (keeps your exciter) |
| 5 | **Rnd All** | Randomize *everything*: patch + FX + filter + LFOs + aftertouch |
| 6 | **Cutoff** | Global filter cutoff (30 Hz – 18 kHz) |
| 7 | **Resonance** | Global filter resonance (ladder voicings self-oscillate at max) |
| 8 | **Filter Type** | LP / HP / BP / Notch |
| menu | **Voicing** | 12 filter voicings (jog-click to open the menu) |

Randomizers are tuned to stay musical: darker-leaning, never above the played pitch,
level-consistent, and applied through a fast fade so they never click. **Rnd All never
touches the output limiter** — your hearing-safety ceiling always survives the dice.

### 2 · Exciter — the strike

| # | Param | What it does |
|---|---|---|
| 1 | **Exc Mix** | Noise burst ↔ mallet (a single pitched sine cycle) |
| 2 | **Crackle** | Random impulse clicks mixed into the strike |
| 3 | **Color** | Resonant low-pass on the excitation — dark thud ↔ bright snap |
| 4 | **Attack** | Burst attack, 0.2–40 ms (slow = bowed/breathy onsets) |
| 5 | **Decay** | Burst length, 2–400 ms (long = scraped/bowed textures) |
| 6 | **Exc Reso** | Resonance of the Color filter (adds a formant-like peak) |
| 7 | **Vel Level** | How much velocity drives loudness |
| 8 | **Vel Color** | How much velocity drives brightness |

The exciter shapes the *attack of the next note* — it doesn't change ringing notes.

### 3 & 4 · Reson A / Reson B — the two objects

| # | Param | What it does |
|---|---|---|
| 1 | **Model** | String / Beam / Plate / Membrane |
| 2 | **Structure** | Model-specific (see table above) |
| 3 | **Decay** | Ring time, from dead thunk to near-endless |
| 4 | **Damp** | High-frequency damping — for modal models this *is* the brightness control |
| 5 | **Position** | Strike/pickup position — comb-like timbre changes (high = hollow) |
| 6 | **Tone** | String only: loop brightness filter |
| 7 | **Tune** | −24…+24 semitones relative to the played note |
| 8 | **Tension** | String/Beam: nonlinear pitch-glide on hard hits |

Detune A vs B a fifth or octave apart, then use **Couple** — that's where Fizzik sings.

### 5 · Voice

| # | Param | What it does |
|---|---|---|
| 1 | **Couple** | Cross-feedback A↔B. 0 = independent, up = interacting, emergent, alive |
| 2 | **Balance** | A ↔ B output mix |
| 3 | **Glide** | Portamento, up to 500 ms |
| 4 | **Amp Atk** | Amplitude attack, 0.5–200 ms |
| 5 | **Amp Rel** | Release, 20 ms–3 s (the resonators keep ringing inside it) |
| 6 | **Spread** | Per-note stereo panning width |
| 7 | **Drive** | Warm saturation (dry/wet blended — silent at 0) |
| 8 | **Level** | Patch level |

### 6 · FX

| # | Param | What it does |
|---|---|---|
| 1 | **Reverb** | Wet mix of the stereo Schroeder reverb |
| 2 | **Rev Size** | Tail length |
| 3 | **Rev Damp** | Tail darkness |
| 4 | **Delay** | Wet mix of the stereo ping-pong delay |
| 5 | **Dly Time** | 30–700 ms (tape-style pitch warp when turned) |
| 6 | **Dly Fbk** | Feedback (echoes cross L↔R) |
| 7 | **Dly Tone** | Echo brightness |
| 8 | **Width** | Stereo width (M/S), 50% = as-is |

### 7 · FX 2 (mastering)

| # | Param | What it does |
|---|---|---|
| 1 | **Tone** | Tilt EQ — dark ↔ bright around 500 Hz |
| 2 | **Body** | Low-end weight (~150 Hz shelf) |
| 3 | **Chorus** | Stereo chorus mix |
| 4 | **Cho Rate** | 0.05–6 Hz |
| 5 | **Cho Depth** | Modulation depth |
| 6 | **Glue** | Bus compressor — cohesion and sustain for chords |
| 7 | **Lim Drive** | Pushes into the limiter (maximizer loudness) |
| 8 | **Lim Ceil** | **Brickwall ceiling.** Defaults low (≈ −3.5 dB) for headphone safety |

The output chain ends in a warm soft-clip followed by a **2 ms lookahead brickwall
limiter** (design borrowed from MechanOdd's Limiter). Whatever you do with resonance,
coupling, or the randomizers, the output *cannot* exceed the ceiling. Raise **Lim Ceil**
if you want more level; it is intentionally never randomized.

### 8 · Mod — two LFOs

Each LFO: **Rate** (0.05–20 Hz) · **Depth** · **Shape** (Sine/Tri/Saw/Square/S&H) ·
**Target** (Off, Cutoff, Pitch, Couple, Balance, Tension, Tone, Reso).

Try: LFO1→Couple (slow sine) for breathing interaction; LFO2→Pitch (S&H, low depth)
for broken-machine detune; LFO→Cutoff (square) for rhythmic filter chops.

### 9 · Aftertouch — press into the sound

Move's pads send **polyphonic aftertouch**: each held note responds to its own finger.

| # | Param | What it does |
|---|---|---|
| 1 | **AT Preset** | 10 curated feels (sets all seven depths at once) |
| 2 | **AT Bright** | Pressure opens the pressed note's resonator brightness |
| 3 | **AT Bow** | Pressure *re-excites* the note — plucks bloom into bowed sustains |
| 4 | **AT Cutoff** | Peak pressure opens the global filter |
| 5 | **AT Vib** | Pressure vibrato (per note) |
| 6 | **AT Bend** | Pressure bends pitch up (string-tension feel) |
| 7 | **AT Vib Rate** | Vibrato speed, 3–9 Hz |
| 8 | **AT Curve** | Pressure response — soft (sensitive) ↔ hard (needs a push) |

**AT Presets:** Off · Gentle · Brighten · Bow · Swell · Vibrato · Expressive (default) ·
Cello · Wild · Sforzato.

Channel aftertouch (from external MIDI) is also supported.

---

## Presets (30)

AlienChurch · BowedGlass · CaveStrings · CouncilsPiano · DistortedBass · FeedbackHarp ·
JudgementAwaits · OldResonances · PreparedPiano · RythmicBow · SensitiveSkin · Sharp ·
ShockingPluck · Slappy · SurroundedByBells · XyloStyle · GlassKalimba · IronLullaby ·
TidalGong · HollowReed · StarlightPad · BrokenMusicBox · DeepDiveBass · CopperTongue ·
GhostSitar · MarbleDrum · WhisperHarp · TitaniumBell · FrozenLake · PulseEngine

All presets are level-calibrated (peak *and* perceived loudness) so browsing never jumps
out at you. Presets carry only the *instrument* — the global performance layer (filter,
aftertouch, LFOs, FX 2 mastering) **persists across preset changes and randomizes**, so
your live setup stays put.

## The 12 filter voicings

Clean SVF · SEM · MS-20 · Steiner · Ladder 4P · Ladder 2P · Ladder 1P · Prophet ·
Oberheim · Diode · Sallen-Key · Vintage

Two engines under the hood (trapezoidal SVF + zero-delay-feedback transistor ladder,
from the published VA-filter math), level-matched, all working in all four filter types.
The ladder voicings self-oscillate at full resonance — bounded, never runaway. The filter
is the main tool for taming a patch that's too bright.

## Tips

- **Bell into drum:** Reson A = Plate (Tune +12), Reson B = Membrane, Balance center,
  Couple ~30% — struck metal over a resonant skin.
- **Bowing without a bow:** AT Preset = *Bow* or *Cello*, hold a pluck and press.
- **Dub station:** Delay mix + feedback up, then ride Dly Time — tape-style warble.
- **One-knob mayhem, safely:** park on Patch, hit **Rnd All** between phrases. The
  limiter ceiling guarantees it never gets dangerous.

## Building from source

```bash
./scripts/build.sh                      # Docker ARM64 cross-compile
MOVE_HOST=move.local ./scripts/install.sh
```

Requires Docker (`aarch64-linux-gnu-gcc`). Single C file, no dependencies. Power-cycle
the Move after installing so it reloads the module metadata.

## Credits

Inspired by **MechanOdd** by [odoare](https://github.com/odoare) (FX-Mechanics). All
algorithms reimplemented from published references: J.O. Smith, *Physical Audio Signal
Processing* (CCRMA); Tolonen/Välimäki/Karjalainen, tension-modulation nonlinearity;
RBJ Audio-EQ Cookbook; Cytomic/Zavalishin VA-filter design. Built with
[Schwung](https://github.com/charlesvestal/schwung) by Charles Vestal.

By **Filliformes**.

## License

MIT — see [LICENSE](LICENSE).
