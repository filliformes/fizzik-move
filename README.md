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
   **Rnd Patch** (k2) for a new instrument.
3. Play the pads. **Press into a held pad** — polyphonic aftertouch makes notes bloom,
   swell, and sing (pick a feel with **AT Preset** on the Aftertouch page).
4. Sculpt live with the global **filter** (Patch knobs 5–8) — 12 analog voicings.
5. Got an **MPE controller**? Set the slot to Receive **All**, turn **MPE On** on the last
   page, and every finger gets its own pitch bend, pressure and timbre.

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
| 1 | **Preset** | 31 factory presets (click-free switching, even mid-note; last = Init blank patch) |
| 2 | **Rnd Patch** | Fire-button: new random instrument — exciter + both resonators + couple/balance |
| 3 | **Rnd Exciter** | Fire-button: randomize only the strike — audible on the *next* note |
| 4 | **Rnd Reson** | Fire-button: randomize only the two resonators (keeps your exciter) |
| 5 | **Cutoff** | Global filter cutoff (30 Hz – 18 kHz) |
| 6 | **Resonance** | Global filter resonance (ladder voicings self-oscillate at max) |
| 7 | **Filter Type** | LP / HP / BP / Notch |
| 8 | **Voicing** | 12 filter voicings |

The three randomizers are momentary **buttons** — click to fire. They're tuned to stay
musical: darker-leaning, never above the played pitch, level-consistent, and applied
through a fast fade so they never click.

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

### 10 · MPE — per-note expression from external controllers

Plug in an MPE controller (LinnStrument, Seaboard, Osmose, Erae, Push 3…) and every
finger gets its own pitch bend, pressure and timbre. Fizzik's bowed/struck physical
models are exactly what MPE was made for: slide between notes, bow one string of a
chord, brighten a single voice.

**Setup:** set the Schwung slot's **Receive channel to All** and leave **Forward on
Auto or Thru** (so the per-note channels reach the synth untouched — an explicit forward
channel would flatten them), then turn **MPE On** here. Leave your controller on its
default MPE layout (lower zone, 48-semitone bend) and it just works.

| # | Param | What it does |
|---|---|---|
| 1 | **MPE** | Off / On. Off = classic behaviour (channels ignored) |
| 2 | **Zone** | Lower (master ch 1, notes on 2–16) or Upper (master ch 16, notes on 1–15) |
| 3 | **Bend Range** | Per-note pitch-bend range in semitones (1–96, default 48 — match your controller) |
| 4 | **Mstr Bend** | Master-channel / global bend range (0–24, default 2) |
| 5 | **Pressure** | How much per-note pressure drives the Aftertouch engine |
| 6 | **Timbre** | Depth of the controller's Y axis (CC74) |
| 7 | **Timbre Tgt** | What Y does: **Bright** (resonator brightness, centred), **Bow** (continuous bowing), **Vib** (vibrato depth) or **Cutoff** (global filter) |
| 8 | **Bend Glide** | Pitch-bend smoothing, ~1–60 ms (low = precise slides, high = liquid) |

- **Pressure goes through the Aftertouch page** — so every AT Preset (Bow, Cello, Swell…)
  is instantly an MPE pressure preset, per finger.
- **Pitch bend works without MPE too:** with MPE Off, a normal bend wheel bends all
  notes by the *Mstr Bend* range.
- All MPE settings are part of the global performance layer: they survive preset changes
  and are saved with your track.
- **The Move's own pads are not MPE** (they send polyphonic aftertouch, handled by the
  Aftertouch page) — this page is for external controllers.
- Zone and bend range are set with the knobs; the controller's own MPE configuration
  messages are not read. If slides land flat or sharp, match **Bend Range** to your
  controller.

> **New in 0.2.0.** MPE has been verified with generated MIDI streams (per-note bend,
> channel isolation, pressure, timbre), but not yet with every hardware controller.
> If yours behaves oddly, please open an issue with the controller model and settings.

---

## Presets (31)

AlienChurch · BowedGlass · CaveStrings · CouncilsPiano · DistortedBass · FeedbackHarp ·
JudgementAwaits · OldResonances · PreparedPiano · RythmicBow · SensitiveSkin · Sharp ·
ShockingPluck · Slappy · SurroundedByBells · XyloStyle · GlassKalimba · IronLullaby ·
TidalGong · HollowReed · StarlightPad · BrokenMusicBox · DeepDiveBass · CopperTongue ·
GhostSitar · MarbleDrum · WhisperHarp · TitaniumBell · FrozenLake · PulseEngine · Init

**Init** (last in the list) is a blank neutral patch — twin plain strings, no coupling,
no FX — made to be a clean starting point for your own sounds: dial it in, then save it
as a user preset.

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
- **MPE cello:** MPE On, Timbre Tgt = *Bow*, AT Preset = *Vibrato* — slide Y to bow each
  string, press for vibrato, glide between notes with per-finger pitch bend.
- **Dub station:** Delay mix + feedback up, then ride Dly Time — tape-style warble.
- **One-button mayhem, safely:** park on Patch, hit **Rnd Patch** between phrases. The
  limiter ceiling guarantees it never gets dangerous.

## Building from source

```bash
./scripts/build.sh                      # Docker ARM64 cross-compile
MOVE_HOST=move.local ./scripts/install.sh
```

Requires Docker (`aarch64-linux-gnu-gcc`). Single C file, no dependencies. Power-cycle
the Move after installing so it reloads the module metadata.

### Offline tests

The DSP links natively, so behaviour is checked without a Move (any gcc; the repo uses a
`fizzik-native` Debian image):

```bash
gcc -O2 -ffast-math -o /tmp/t scripts/test_state.c src/dsp/fizzik.c -lm && /tmp/t
```

| Test | Checks |
|---|---|
| `test_state.c` | Saved tracks/presets restore exactly (preset, level, every page incl. MPE) |
| `test_mpe.c` | Per-note bend, channel isolation, bend seeding, channel-aware note-off, CC74 bow |
| `test_filter_stress.c` | 12 voicings × 4 types at full resonance stay bounded; chords don't clip |
| `test_voicing_levels.c` | All filter voicings level-matched to Clean SVF |
| `test_levels.c` | Per-preset loudness calibration meter |

`scripts/mpe_demo.py` (Windows, experimental) plays an MPE test stream into the Move over
USB for trying MPE without a controller — run `probe` first to find the port that reaches
Fizzik. It depends on the PC's MIDI service opening the Move's ports, which is not
reliable on every machine.

## Changelog

**0.2.0**
- **MPE** for external controllers: per-note pitch bend, pressure and timbre (CC74), on a
  new MPE page (zone, bend ranges, pressure/timbre depth, timbre target, bend glide).
- Pitch bend now works without MPE too (bend wheel → all notes, *Mstr Bend* range).
- On-device help rewritten to match the current pages.

**0.1.1**
- Fixed saved tracks reopening on a factory preset; presets/tracks now restore exactly.
- New **Init** preset (blank patch).
- Chords no longer distort: real polyphonic headroom; all 12 filter voicings level-matched.
- Main page reworked (full filter on knobs, randomizers are buttons, Rnd All removed);
  both LFOs draw the animated graphic.

**0.1.0** — first release.

## Credits

Inspired by **MechanOdd** by [odoare](https://github.com/odoare) (FX-Mechanics). All
algorithms reimplemented from published references: J.O. Smith, *Physical Audio Signal
Processing* (CCRMA); Tolonen/Välimäki/Karjalainen, tension-modulation nonlinearity;
RBJ Audio-EQ Cookbook; Cytomic/Zavalishin VA-filter design. Built with
[Schwung](https://github.com/charlesvestal/schwung) by Charles Vestal.

By **Filliformes**.

## License

MIT — see [LICENSE](LICENSE).
