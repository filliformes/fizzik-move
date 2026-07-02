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
- `src/module.json` — metadata + ui_hierarchy (5 pages) + chain_params (36 params)
- `scripts/build.sh` — Docker ARM64 cross-compile (docker create + cp pattern, exit-code checked)
- `scripts/install.sh` — scp to `modules/sound_generators/fizzik/`, chmod +x, chown
- `.github/workflows/release.yml` — CI: version check → build → release → release.json

## Pages (8 knobs each; Patch has 4)
1. **Exciter**: exc_mix, exc_crackle, exc_color, exc_attack, exc_decay, exc_reso, vel_level, vel_color
2. **Reson A**: a_model, a_struct, a_decay, a_damp, a_pos, a_tone, a_tune, a_tension
3. **Reson B**: b_* (same eight)
4. **Voice**: couple, balance, glide, amp_attack, amp_release, spread, space, level
5. **Patch**: preset (16 named), rnd_patch, rnd_exc, rnd_reson

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
- `get_param` returns **-1** for unknown keys (0 breaks Master FX menu editing).
- Trigger knobs (rnd_*) = `type:int` 0..127; fire on nonzero; get_param returns "0".
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
