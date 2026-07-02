# Fizzik — Design Spec

**Fizzik** is a polyphonic *simplex* physical-modelling synth for Ableton Move (Schwung
`sound_generator`, `plugin_api_v2`). It distils the essence of
[MechanOdd](https://github.com/odoare/Mechanodd) (odoare / FX-Mechanics) into a lean,
CPU-appropriate instrument: an excitation burst driving **two coupled physical
resonators**, with a nonlinear tension-modulation layer.

"Simplex" (Berthoz's *simplexité*): rich, emergent behaviour from a deliberately simple
structure — here, complex timbres arise from the *coupling* of two simple resonators,
not from piling on modules.

- **component_type:** `sound_generator`
- **api_version:** 2  (init symbol `move_plugin_init_v2`)
- **module id:** `fizzik`   repo `filliformes/fizzik-move`
- **author / attribution:** Filliformes (algorithms clean-room reimplemented in C from
  MechanOdd's documented math + public academic references; **no source copied**).
- **polyphony:** target 6 voices (tune during build for CPU headroom).

---

## Provenance / licensing

MechanOdd declares **no license** (all-rights-reserved by default); its `CracksGenerator`
dependency is LGPL-3.0. Fizzik therefore **does not copy any MechanOdd/FxmeFX code**. Every
algorithm is reimplemented from scratch in C, based on the DSP described in MechanOdd's
`doc/` and the public references it cites:

- J. O. Smith, *Physical Audio Signal Processing* (CCRMA) — digital waveguides.
- Tolonen, Välimäki, Karjalainen, *Modeling of Tension Modulation Nonlinearity in Plucked
  Strings*, IEEE TSAP 8(3), 2000 — the hardening layer.
- RBJ Audio-EQ Cookbook — modal band-pass biquads.
- Standard modal synthesis (Kirchhoff plate, 2D-wave membrane, Euler-Bernoulli beam).

README/credits will state: *"Inspired by MechanOdd by odoare (FX-Mechanics); DSP
reimplemented independently for Schwung."*

---

## Architecture (per voice)

```
                 ┌───────────── cross-coupling (Couple knob) ─────────────┐
                 │  (1-sample delay · DC-block · tanh soft-limit per loop) │
                 ▼                                                         ▼
  MIDI ─► Exciter ──┬─────────────► Resonator A ──┐                        │
   (burst:          │                    ▲         ├─► A/B Balance ─► Amp ─► voice out
    noise+mallet    └─────────────► Resonator B ──┘         env
    +cracks, LPF,                        ▲                  (+ Space)
    perc ADSR)                           └──── coupling ────┘
```

- **Exciter**: one shared exciter per voice — a blend of white **noise** and a single-cycle
  **mallet/pluck** wavetable, plus a **crackle** generator, shaped by a resonant one-pole
  **LPF (Color)** and a percussive **ADSR** (Attack/Decay). Velocity links to level & color.
- **Two resonators (A, B)**, each independently one of four models (below), excited in
  **parallel** by the exciter.
- **Couple** knob: cross-feedback amount. At 0 the resonators are independent (parallel);
  as it rises, each resonator's (guarded) output is fed into the other's input with a
  1-sample delay — reproducing MechanOdd's emergent coupled-network behaviour. Every loop
  node passes through: NaN/Inf sanitise → one-pole **DC-block (~8 Hz)** → **tanh soft-limit**
  (ceiling ≈ +12 dBFS) so the coupled loop can never run away.
- **Balance**: A/B output mix. **Amp env** + **Level**. Optional light **Space** (reverb).

---

## Resonator models (per resonator, selectable)

| Model | Kind | Frequency law | "Structure" knob |
|---|---|---|---|
| **String** | Dispersive digital waveguide | `f0 = fs/(2N)`; 8-section allpass dispersion | Dispersion (stiffness/inharmonicity) |
| **Beam** | 1D modal morph | `f_n = f0·n·√((1−b) + b·n²)` | b: string(harmonic)→beam(n²) |
| **Plate** | Kirchhoff modal | `f_mn ∝ (m/a)²+(n/b)²` | Aspect ratio a/b |
| **Membrane** | 2D-wave modal | `f_mn ∝ √((m/a)²+(n/b)²)` | Aspect ratio a/b |

**Waveguide (String):** bidirectional rails as fractional delay lines (Lagrange
interpolation), bridge reflection = feedback-gain × one-pole LP × 8×first-order allpass
dispersion chain, allpass group-delay compensated in rail length to hold pitch. Feedback
gain/cutoff **pitch-normalised** to a reference (middle C) so T60 & timbre stay consistent
across the range. In/out position taps.

**Modal core (Beam/Plate/Membrane):** bank of up to ~24 damped RBJ band-pass biquads (one
per mode), 0-dB-peak, with per-mode geometric amplitude `phi_in·phi_out`. Damping as a
dB-scaled resonance + a frequency-slope; band-pass level compensation `√(zetaRef/zeta)`.
Modes recomputed on tuning/structure change; damping re-tuned as the gate releases.

**Nonlinear — Tension hardening (String/Beam):** per Tolonen/Välimäki. Estimate global
string elongation `epsilon` from the delay-line displacement profile, smooth it (~1–10 ms
one-pole, anti-zipper), form `k = 1 + α·epsilon`, shorten the effective loop length
`N_eff = N0/√k` via the existing fractional read. Gives the transient pitch-glide of a hard
pluck. **Tension** knob = α. (Collision/contact layer deferred to a later version.)

---

## Parameters & pages (Shadow UI `ui_hierarchy`, 8 knobs/page)

**Page 1 — Exciter** (also mirrored as `root` knobs)
1. `exc_mix`   Exc Mix        (noise ↔ mallet, 0–100%)
2. `exc_crackle` Crackle      (0–100%)
3. `exc_color` Color          (exciter LPF cutoff)
4. `exc_attack` Attack        (burst attack, ms)
5. `exc_decay` Decay          (burst length, ms)
6. `exc_reso`  Exc Reso       (LPF resonance)
7. `vel_level` Vel→Level      (0–100%)
8. `vel_color` Vel→Color      (0–100%)

**Page 2 — Reson A**
1. `a_model`   Model A        (enum: String·Beam·Plate·Membrane)
2. `a_struct`  Structure A    (dispersion / beam-morph / aspect)
3. `a_decay`   Decay A        (T60 / resonance)
4. `a_damp`    Damp A         (damping slope — HF decay)
5. `a_pos`     Position A     (in/out tap)
6. `a_tone`    Tone A         (feedback cutoff / mode brightness)
7. `a_tune`    Tune A         (coarse, −24..+24 st)
8. `a_tension` Tension A      (hardening α; String/Beam only)

**Page 3 — Reson B** — same eight params, `b_*` keys.

**Page 4 — Voice**
1. `couple`    Couple         (cross-feedback amount, 0–100%)
2. `balance`   Balance        (A ↔ B output mix)
3. `glide`     Glide          (portamento, ms)
4. `amp_attack` Amp Atk       (ms)
5. `amp_release` Amp Rel      (ms)
6. `spread`    Spread         (stereo detune/pan)
7. `space`     Space          (reverb mix)
8. `level`     Level          (master, dB)

**Page 5 — Patch** (menu-only + trigger knobs)
- `preset`     Preset         (enum, named — see below; menu-only, not on a knob)
- `rnd_patch`  Rnd Patch      (trigger; type enum ["0","1"], auto-reverts)
- `rnd_exc`    Rnd Exciter    (trigger)
- `rnd_reson`  Rnd Reson      (trigger)

**Preset names** (borrowed as flavour from MechanOdd's shipped presets):
AlienChurch, BowedGlass, CaveStrings, CouncilsPiano, DistortedBass, FeedbackHarp,
JudgementAwaits, OldResonances, PreparedPiano, RythmicBow, SensitiveSkin, Sharp,
ShockingPluck, Slappy, SurroundedByBells, XyloStyle. Each = full param set with per-voice
randomisation ranges (see `/move` preset pattern).

---

## DSP constraints / notes

- `plugin_api_v2`, symbol `move_plugin_init_v2`, block 128, SR read from host.
- Pad note → per-voice gate/pitch/velocity. Modulo pad mapping (root-note independent).
- Re-tune active voices every block so knob turns affect ringing notes (no state clear).
- Q-normalise band-pass outputs; DC-block + tanh limit every coupled-loop node.
- No `pow`/`tan` in the hot per-sample path where avoidable; precompute per block. Cap
  modal mode count and voice count to hold CPU; adapt if the build runs hot.
- Trigger knobs: `type:"enum" options:["0","1"]`, direct `set_param` handler, get_param → "0".
- Presets: menu-only enum; snap all smoothed values on apply.
- Implement `get_param("ui_hierarchy")` (DSP-side) mirroring module.json exactly.
- State persistence via `get_param("state")` / `set_param("state")` round-trip.

## Deferred (post-v1)
- Collision / unilateral-contact nonlinearity (buzz/rattle).
- Per-parameter modulation matrix (MechanOdd's 12 modulators).
- Bus/master effect chains beyond a single Space reverb.
