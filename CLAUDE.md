# Fizzik — Claude Code context

## What this is
Polyphonic **simplex physical-modeling synth** for Ableton Move (Schwung `sound_generator`,
`plugin_api_v2`, C). Distilled from [MechanOdd](https://github.com/odoare/Mechanodd) — DSP
reimplemented independently (no source copied). Two coupled physical resonators per voice.

## Architecture
Per voice: **Exciter** (noise+mallet+crackle → resonant SVF LPF → percussive AD burst)
excites **two resonators A/B in parallel**. A **Couple** knob cross-feeds each resonator's
guarded output (1-sample delay → sanitize → DC-block ~8 Hz → tanh soft-limit) into the
other's input — MechanOdd's emergent coupled-network behaviour in one knob. **Balance** mixes
A/B, an **amp env** gates, **Spread** pans per voice, **Space** is a Schroeder reverb.

Resonator models (per resonator, selectable):
- **String** — single-loop dispersive digital waveguide (Lagrange-free linear-interp fractional
  delay, one-pole loop LP, 4-section allpass dispersion, pitch-normalized feedback, pickup-comb
  position) + **tension-modulation hardening** (Tolonen/Välimäki: loop energy shortens Leff).
- **Beam** — 1D modal morph `f_n = f0·n·√((1−b)+b·n²)`.
- **Plate** — Kirchhoff `f_mn ∝ (m/a)²+(n/b)²`.
- **Membrane** — 2D-wave `f_mn ∝ √((m/a)²+(n/b)²)`.
Modal core = bank of ≤24 RBJ band-pass biquads, recomputed only when pitch/structure changes.

## Files
- `src/dsp/fizzik.c` — all DSP (single translation unit)
- `src/module.json` — metadata + ui_hierarchy (root + 8 sub-pages) + chain_params (~72 params, preset first; mirrored in the DSP-served copies)
- `scripts/build.sh` — Docker ARM64 cross-compile (docker create + cp pattern, exit-code checked)
- `scripts/install.sh` — scp to `modules/sound_generators/fizzik/`, chmod +x, chown
- `.github/workflows/release.yml` — CI: version check → build → release → release.json

## Pages (root is the Patch page; 8 knobs each — no separate "Patch" sub-level)
1. **root** knobs: preset (31 named, last = "Init" blank neutral patch), rnd_patch, rnd_exc, rnd_reson (fire-buttons), **cutoff, resonance, ftype, voicing** (global filter). Root `params` = these 8 keys + the sub-level links. rnd_all was removed (user feedback).
2. **Exciter**: exc_mix, exc_crackle, exc_color, exc_attack, exc_decay, exc_reso, vel_level, vel_color
3. **Reson A**: a_model, a_struct, a_decay, a_damp, a_pos, a_tone, a_tune, a_tension
4. **Reson B**: b_* (same eight)
5. **Voice**: couple, balance, glide, amp_attack, amp_release, spread, drive, level
6. **FX**: rev_mix, rev_size, rev_damp, dly_mix, dly_time, dly_fb, dly_tone, width
7. **FX2**: eq_tone, eq_body, cho_mix, cho_rate, cho_depth, comp_amt, lim_drive, lim_ceil
8. **Mod**: lfo1_rate/depth/shape/target, lfo2_rate/depth/shape/target
9. **Aftertouch (Touch)**: at_preset (10), at_bright, at_bow, at_cutoff, at_vib, at_bend, at_vrate, at_curve

## Global params & the GLOBAL region
`params_t` splits at `GLOBAL_PARAMS_OFF` (= offsetof flt_cutoff). Everything from there
(filter, aftertouch, LFOs, FX2) is a **global performance layer**: `apply_preset` memcpy-saves
that whole region, loads the preset, then restores it — so presets only carry the voice sound;
filter/aftertouch/mod/master-FX persist across preset changes. State round-trips all of it.

## Poly aftertouch (Move 0xA0)
Per-voice `pressure` (smoothed ~28ms, shaped by `at_curve`) drives, scaled by the at_* depths:
brightness (adds to resonator tone at retune), **bow** (noise re-excitation into the voice —
plucks bloom into sustains), cutoff (peak pressure → global filter), vibrato + bend (per-voice
pitch at block-rate retune). 10 AT presets (`AT_PRESETS`) set all seven depths at once via
`apply_at_preset`. Also handles channel AT (0xD0).

## Modulation (2 LFOs) + master FX2
Two block-rate LFOs (`lfo_block`, sine/tri/saw/square/S&H) → targets {Off,Cutoff,Pitch,Couple,
Balance,Tension,Tone,Reso}, summed into per-block mod accumulators. FX2 = tilt/body EQ
(`eq_process`), stereo chorus, glue compressor, and a **lookahead brickwall limiter** (2ms/
`LIM_LA`, drive+ceiling, after the warm `out_limit` tanh) — reimplemented clean-room from
MechanOdd's Limiter design (odoare/FxmeFX, LGPL — studied, not copied). Master chain order:
filter → drive → EQ → chorus → delay → reverb → width → glue → tanh → lookahead limiter.

## Level calibration & FX
- Per-voice output halved (`level_gain = level²·0.7·makeup`); `out_limit` is a SAFETY
  soft-clip with real headroom (2·tanh(x/2) — linear through normal poly program), and
  the **lookahead brickwall limiter is the actual ceiling** (the old 0.9·tanh sheared
  from ~0.4 → poly+resonance distortion no limiter setting could remove).
- **Polyphony compensation:** the voice bus is scaled by `poly_gain = n_eff^-0.35`
  (n_eff = Σ amp envelopes, smoothed ~45 ms), applied PRE-filter — single notes stay at
  the calibrated level, a 4-voice chord sits ~4 dB down, so chords no longer park the
  limiter in constant gain reduction (that + the 60→150 ms limiter release fixed "all
  voicings distort on a chord"). Chord cleanliness asserted by the clip-incidence check
  in `scripts/test_filter_stress.c` (0% pinned samples at the default ceiling).
- Each preset carries a **baked `makeup`** gain so every one peaks ~0.4 for a single
  note (target set in `scripts/test_levels.c`). Modal resonators self-level via a
  reference-burst RMS measurement in `modal_recompute`.
- **Recalibrating presets:** edit params, then run the offline meter — it links `fizzik.c`
  for x86 and reports per-preset single-note peak, 4-voice chord peak, post-release tail
  (stability), and the makeup needed for peak 0.4:
  ```bash
  docker run --rm -v "$(pwd -W):/repo" -w /repo fizzik-native \
    bash -c "gcc -O2 -ffast-math -o /tmp/tl scripts/test_levels.c src/dsp/fizzik.c -lm && /tmp/tl"
  ```
  (`fizzik-native` = debian+native-gcc image.) Hidden DSP hooks `get_param("__meter")`
  and `set_param("__makeup",..)` support it. Bake results into the `PRESETS[]` makeup column.
- FX: reverb (Schroeder, size/damp/mix), stereo ping-pong delay (time/fb/tone/mix),
  drive (tanh **dry/wet blend** — no click at engage), width (M/S). FX defaults set in
  `apply_preset` (not in the positional table).

## Global filter (post-synth, pre-FX)
Multimode VA filter on the Patch/root page (knobs 5–8: Cutoff, Resonance, Filter Type
LP/HP/BP/Notch, Voicing). Clean-room from public VA-filter math (Cytomic trapezoidal SVF +
Zavalishin ZDF transistor ladder). **12 voicings**: Clean SVF, SEM, MS-20, Steiner,
Ladder 4P/2P/1P, Prophet, Oberheim, Diode, Sallen-Key, Vintage. Self-oscillating resonance
bounded by tanh feedback (verified finite at res=1; ladder family rings at res=1 BY DESIGN
— `scripts/test_filter_stress.c` asserts bounded there, decay at res≤0.7). SVF-family
LP/HP get **resonant-peak compensation** (`rtrim`, kicks in only when Q>1) so driving
reso+cutoff polyphonically doesn't slam the output stage. Every voicing carries a
**measured `trim`** level-matching it to Clean SVF on sustained 4-voice program
(`scripts/test_voicing_levels.c` — the driven voicings' input tanh loses 2-4 dB, which
the old output squash masked); re-run + re-bake after touching drive/kmin/kmax **or any
gain upstream of the filter** (trims are level-dependent: they were re-baked once already
when polyphony compensation lowered the bus level into the filter). Stereo (`fltL/fltR`). It's a **global
master control** — `apply_preset` preserves it across preset loads (not per-preset). This is
the primary tool for taming bright/high-pitched resonances.

## Analog-style parameter smoothing
Every continuous param is 20 ms one-pole smoothed per block into `inst->sm` (a `params_t`
shadow), iterated over the `PDESC` table (float = smoothed, int = copied). render_block reads
continuous params from `sm`, discrete (models, tunes, enums, makeup) from `p`. Prime `sm = p`
in create_instance. This kills zipper/clicks (incl. the old Drive engage click).

## Level calibration = perceptual (peak + RMS)
`makeup = min(0.42/peak, 0.13/rms)` — caps by transient peak AND sustained RMS, so long/
bright resonances (pads, bells) don't read as loud as their attack. Output is halved
(`level²·0.7`); `out_limit` (2·tanh(x/2), headroom soft-clip) + the lookahead brickwall
keep polyphony from ever hard-clipping.
**Baking convention:** the meter now measures truly raw (its `__makeup=1.0` override used to
be stomped by the old deferred preset apply); all shipped presets sit at **baked ≈ 0.5 × the
raw-meter suggestion** (effective single-note peak ≈ 0.21). Calibrate new presets the same
way — do NOT re-bake existing ones (users' tracks are balanced against them).

## Constants worth knowing
`MAX_VOICES 6`, `DELAY_MAX 2048` (~21.5 Hz min), `N_ALLPASS 4`, `MAX_MODES 24`. SR 44100.
Params stored normalized 0..1 in `params_t` (also the preset layout); `PDESC[]` table maps
keys → fields via offsetof. Page-aware knob overlay via `PAGE_KEYS` + `current_page`.

## Critical constraints
- NEVER allocate / printf / lock in `render_block`. All state in the instance struct.
- `plugin_api_v2_t` has 8 fields incl. `get_error` (=NULL) — omitting it SIGSEGVs render.
- DSP must be `dsp.so`; needs `chmod +x` on device (install.sh handles it).
- `get_param("ui_hierarchy")` MUST be implemented (Shadow UI routes it to the DSP) — mirror
  module.json exactly.
- Enum `get_param` returns the option **string** (model/preset names); `set_param` accepts
  name or index. Regular `get_param` returns **raw** values (round-trips for state persistence).
- **State blob** (`get_param("state")`, newline key=value): starts with `preset=<name>` then
  `__makeup=`, then all PDESC fields. By-key `set_param("preset")` applies **immediately** and
  is a **no-op if the preset is already current** — NEVER defer it (the deferred apply fired
  after the host's state restore and stomped saved patches back to the factory preset; knob
  path keeps the click-guarded deferred duck). `preset` sits FIRST in chain_params for the
  same reason. Round-trip test: `scripts/test_state.c` (same docker fizzik-native invocation
  as test_levels.c).
- `get_param` returns **-1** for unknown keys (0 breaks Master FX menu editing).
- Trigger knobs (rnd_*) = `type:enum ["idle","trigger"]` with **`access:"write"`** (v1.0+
  fire-on button — the host renders a button, click fires, never scrubs). DSP fires on
  "trigger" or nonzero; get_param returns "0"; excluded from state.
- **LFO viz**: both LFO groups declare `viz {group, role}` (rate/depth/shape) in chain_params
  (module.json AND the DSP-served copy) so BOTH draw the animated LFO graphic — the host's
  detector alone only resolves ONE LFO group per page (that was the "LFO2 is plain knobs" bug).
- module.json is cached at host startup — **power-cycle the Move** to reload it.

## Build & deploy
```bash
./scripts/build.sh          # Docker ARM64 cross-compile → dist/fizzik-module.tar.gz
MOVE_HOST=move.local ./scripts/install.sh
```
Power-cycle the Move after a module.json change; otherwise remove/re-add the module reloads dsp.so.

## Release
`/move-schwung-release 0.1.0`. Tag `vX.Y.Z` must match `version` in `src/module.json`.

## Provenance
Inspired by MechanOdd (odoare / FX-Mechanics, no license declared). Algorithms reimplemented
from documented math + public academic references (JOS PASP, Tolonen/Välimäki, RBJ). Attribution:
**Filliformes**. MIT-licensed original code.
