/**
 * Fizzik — Polyphonic simplex physical-modeling synth for Ableton Move (Schwung).
 * Author: Filliformes
 * License: MIT
 *
 * Inspired by MechanOdd (odoare / FX-Mechanics). DSP reimplemented independently
 * in C from the documented math and public references (J.O. Smith PASP digital
 * waveguides; Tolonen/Valimaki tension modulation; RBJ modal band-pass; modal
 * synthesis of plate/membrane/beam). No source copied.
 *
 * Architecture (per voice):
 *   Exciter (noise+mallet+crackle -> resonant LPF -> percussive AD)
 *     -> two resonators A,B (String/Beam/Plate/Membrane), excited in parallel
 *     -> cross-coupling (Couple knob, 1-sample delay, DC-block + tanh soft-limit)
 *     -> A/B balance -> amp env -> pan -> voice out
 *   Global: sum voices -> Space reverb -> master level -> stereo int16.
 *
 * API: plugin_api_v2_t. 44100 Hz, 128 frames/block, stereo interleaved int16 out.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>

/* ── Constants ───────────────────────────────────────────────────────────────── */

#define SR            44100.0f
#define SR_INV        (1.0f / SR)
#define TWO_PI        6.283185307179586f
#define PI            3.141592653589793f

#define MAX_VOICES    6
#define DELAY_MAX     2048      /* single-loop waveguide, min ~21.5 Hz */
#define DLY_MAX       33075     /* FX delay line, 0.75 s @ 44.1k */
#define N_ALLPASS     4         /* dispersion sections */
#define MAX_MODES     24        /* modal bank size */
#define MAX_CAND      256       /* modal candidate scratch */

#define FIRST_NOTE    36        /* Move pad base (used only for modulo mapping) */
#define REF_FREQ      261.626f  /* middle C — pitch normalization reference */

#define MODEL_STRING   0
#define MODEL_BEAM     1
#define MODEL_PLATE    2
#define MODEL_MEMBRANE 3

#define N_PRESETS     16

static const char *MODEL_NAMES[4]  = { "String", "Beam", "Plate", "Membrane" };
static const char *PRESET_NAMES[N_PRESETS] = {
    "AlienChurch", "BowedGlass", "CaveStrings", "CouncilsPiano", "DistortedBass",
    "FeedbackHarp", "JudgementAwaits", "OldResonances", "PreparedPiano", "RythmicBow",
    "SensitiveSkin", "Sharp", "ShockingPluck", "Slappy", "SurroundedByBells", "XyloStyle"
};

/* Page-aware knob overlay: keys per page (index = current_page). */
static const char *PAGE_KEYS[6][8] = {
    { "exc_mix","exc_crackle","exc_color","exc_attack","exc_decay","exc_reso","vel_level","vel_color" },
    { "a_model","a_struct","a_decay","a_damp","a_pos","a_tone","a_tune","a_tension" },
    { "b_model","b_struct","b_decay","b_damp","b_pos","b_tone","b_tune","b_tension" },
    { "couple","balance","glide","amp_attack","amp_release","spread","drive","level" },
    { "rev_mix","rev_size","rev_damp","dly_mix","dly_time","dly_fb","dly_tone","width" },
    { "preset","rnd_patch","rnd_exc","rnd_reson","","","","" }
};
static const int PAGE_NKNOBS[6] = { 8, 8, 8, 8, 8, 4 };

/* ── Small helpers ───────────────────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float map_exp(float x, float lo, float hi) { /* x in 0..1 */
    return lo * powf(hi / lo, clampf(x, 0.0f, 1.0f));
}
static inline float note_to_freq(float note) {
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}
static inline float sanitize(float x) { return isfinite(x) ? x : 0.0f; }
static inline float soft_limit(float x) { /* ceiling ~+12 dBFS */
    if (!isfinite(x)) return 0.0f;
    const float c = 4.0f;
    return c * tanhf(x * (1.0f / c));
}

/* xorshift RNG -> [0,1) */
static inline float randf(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 0x1234567u;
    return (float)(x & 0x00FFFFFF) / (float)0x01000000;
}
static inline float randbi(uint32_t *s) { return randf(s) * 2.0f - 1.0f; }

/* ── Biquad (one modal mode, RBJ constant 0 dB band-pass) ────────────────────── */

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

static inline float biquad_process(biquad_t *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}
static void biquad_bandpass(biquad_t *f, float freq, float Q) {
    float w0 = TWO_PI * freq * SR_INV;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float a0 = 1.0f + alpha;
    f->b0 =  alpha / a0;
    f->b1 =  0.0f;
    f->b2 = -alpha / a0;
    f->a1 = -2.0f * cw / a0;
    f->a2 = (1.0f - alpha) / a0;
}

/* ── TPT state-variable LPF (exciter color, resonant) ────────────────────────── */

typedef struct { float ic1, ic2; } svf_t;
static inline float svf_lp(svf_t *s, float x, float cutoff, float reso) {
    float g = tanf(PI * clampf(cutoff, 20.0f, 18000.0f) * SR_INV);
    float k = 2.0f - 1.9f * clampf(reso, 0.0f, 1.0f);   /* damping: reso->high Q */
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float v1 = a1 * s->ic1 + a2 * (x - s->ic2);
    float v2 = s->ic2 + g * v1;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return v2;   /* lowpass */
}

/* ── DC blocker (one-pole highpass ~8 Hz), used in the coupling loop ─────────── */

typedef struct { float x1, y1; } dcblk_t;
static inline float dcblk(dcblk_t *d, float x) {
    if (!isfinite(x)) { d->x1 = d->y1 = 0.0f; return 0.0f; }
    float y = x - d->x1 + 0.9989f * d->y1;   /* R for ~8 Hz @ 44.1k */
    d->x1 = x; d->y1 = y;
    return y;
}

/* ── Waveguide string (single loop, dispersive, tension-modulated) ───────────── */

typedef struct {
    float dl[DELAY_MAX];
    int   wpos;
    float lp;                        /* loop one-pole LP state */
    float ap[N_ALLPASS];             /* dispersion allpass state */
    float len, target_len;           /* fractional loop length (smoothed) */
    float apc, target_apc;           /* dispersion coeff (smoothed) */
    float fb_gain, fb_cut, lpc;      /* pitch-normalized loop feedback + LP coeff */
    float pos;                       /* pickup position 0..1 */
    float energy;                    /* smoothed loop energy (tension) */
    float alpha;                     /* tension coupling */
    int   snap;                      /* jump geometry (note start) */
} waveguide_t;

static void wg_reset(waveguide_t *w) {
    memset(w->dl, 0, sizeof(w->dl));
    w->wpos = 0; w->lp = 0.0f; w->energy = 0.0f;
    memset(w->ap, 0, sizeof(w->ap));
    w->snap = 1;
}

/* Fractional read from the delay line, `age` samples behind the write head. */
static inline float wg_read(waveguide_t *w, float age) {
    if (age < 1.0f) age = 1.0f;
    if (age > DELAY_MAX - 2) age = DELAY_MAX - 2;
    float rp = (float)w->wpos - age;
    while (rp < 0.0f) rp += DELAY_MAX;
    int i0 = (int)rp;
    float frac = rp - (float)i0;
    int i1 = i0 + 1; if (i1 >= DELAY_MAX) i1 -= DELAY_MAX;
    return w->dl[i0] + frac * (w->dl[i1] - w->dl[i0]);
}

/* Group delay of a 1st-order allpass (a + z^-1)/(1 + a z^-1) at w. */
static inline float ap_group_delay(float a, float w) {
    return (1.0f - a * a) / (1.0f + 2.0f * a * cosf(w) + a * a);
}

/* Set target coefficients for the played frequency + knob values. */
static void wg_set(waveguide_t *w, float f0, float dispersion, float decay,
                   float tone, float damp, float tension) {
    f0 = clampf(f0, 21.5f, SR * 0.45f);
    float w0 = TWO_PI * f0 * SR_INV;
    float roundtrip = SR / f0;

    /* Feedback gain: pitch-normalized so T60 is constant across the range. */
    float g01 = 0.950f + 0.0495f * clampf(decay, 0.0f, 1.0f);   /* 0.950 .. 0.9995 */
    w->fb_gain = powf(g01, REF_FREQ / f0);

    /* Loop LP cutoff from Tone, reduced by Damp, scaled to a fixed harmonic ratio. */
    float cut = map_exp(tone, 500.0f, 15000.0f) * (1.0f - 0.75f * clampf(damp, 0.0f, 1.0f));
    cut = clampf(cut * (f0 / REF_FREQ), 80.0f, SR * 0.45f);
    w->fb_cut = cut;
    w->lpc = 1.0f - expf(-TWO_PI * cut * SR_INV);   /* precompute per block */

    /* Dispersion allpass coeff (negative -> stiffness/inharmonicity). */
    float ap = -0.85f * clampf(dispersion, 0.0f, 1.0f);
    float max_disp = 0.6f * roundtrip;
    float r = max_disp / (float)N_ALLPASS;
    float acap = fminf(0.0f, (1.0f - r) / (1.0f + r));
    if (ap < acap) ap = acap;
    w->target_apc = ap;

    float disp_delay = (float)N_ALLPASS * ap_group_delay(ap, w0);
    w->target_len = fmaxf(8.0f, roundtrip - disp_delay - 1.0f);

    /* pos (pickup) is set by reso_set() after this call. */
    w->alpha = map_exp(clampf(tension, 0.0f, 1.0f) + 1e-4f, 0.02f, 6.0f) - 0.02f; /* 0..~6 */

    if (w->snap) { w->len = w->target_len; w->apc = w->target_apc; w->snap = 0; }
}

static inline float wg_process(waveguide_t *w, float input) {
    /* Glide geometry (click-free). */
    w->len += 0.02f * (w->target_len - w->len);
    w->apc += 0.02f * (w->target_apc - w->apc);

    /* Tension modulation: raise pitch (shorten loop) with loop energy. */
    float k = 1.0f + w->alpha * w->energy;
    k = clampf(k, 1.0f, 4.0f);
    float Leff = w->len / sqrtf(k);

    float out = wg_read(w, Leff);
    w->energy += 0.0045f * (out * out - w->energy);   /* ~5 ms */

    /* Pickup-position comb (position-dependent timbre, pitch unaffected). */
    float tap = wg_read(w, clampf(w->pos, 0.02f, 0.98f) * Leff);
    float pickup = out - tap;

    /* Loop filter: one-pole LP toward fb_cut (coeff precomputed in wg_set). */
    w->lp += w->lpc * (out - w->lp) + 1e-20f;   /* denormal guard */
    float v = w->fb_gain * w->lp;

    /* Dispersion allpass chain. */
    for (int j = 0; j < N_ALLPASS; j++) {
        float in = v - w->apc * w->ap[j];
        float y  = w->apc * in + w->ap[j];
        w->ap[j] = in;
        v = y;
    }

    /* Write feedback + injected excitation. */
    w->dl[w->wpos] = sanitize(v + input);
    w->wpos++; if (w->wpos >= DELAY_MAX) w->wpos = 0;

    return pickup * 0.45f;   /* trim: waveguide runs hot vs modal */
}

/* ── Modal resonator (Beam / Plate / Membrane) ───────────────────────────────── */

typedef struct {
    biquad_t f[MAX_MODES];
    float    amp[MAX_MODES];
    float    norm;                 /* RMS level normalization */
    int      n;
    /* change-detection signature */
    int   model;
    float f0, mstruct, mdecay, mdamp, mpos;
} modal_t;

static void modal_reset(modal_t *m) {
    for (int i = 0; i < MAX_MODES; i++) { m->f[i].z1 = m->f[i].z2 = 0.0f; }
    m->n = 0; m->norm = 1.0f;
    m->f0 = -1.0f;   /* force recompute */
}

typedef struct { float freq, amp; } cand_t;
static int cand_cmp(const void *a, const void *b) {
    float fa = ((const cand_t *)a)->freq, fb = ((const cand_t *)b)->freq;
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

/* dB-scaled damping: 0->1e-1 (dead), 1->1e-6 (permanent ring). */
static inline float decay_to_zeta(float d) {
    float dB = clampf(d, 0.0f, 1.0f) * 100.0f;
    return 0.1f * powf(10.0f, -dB / 20.0f);
}

static void modal_recompute(modal_t *m, int model, float f0,
                            float mstruct, float mdecay, float mdamp, float mpos) {
    m->model = model; m->f0 = f0; m->mstruct = mstruct;
    m->mdecay = mdecay; m->mdamp = mdamp; m->mpos = mpos;

    float nyq = SR * 0.49f;
    float zeta0 = decay_to_zeta(mdecay);
    float zeta1 = decay_to_zeta(1.0f - mdamp) * 0.5f;   /* HF damping slope */

    cand_t cand[MAX_CAND];
    int nc = 0;

    if (model == MODEL_BEAM) {
        float b = clampf(mstruct, 0.0f, 1.0f);            /* string->beam */
        float in = 0.15f, out = clampf(0.1f + 0.85f * mpos, 0.02f, 0.98f);
        for (int nn = 1; nn <= MAX_CAND && nc < MAX_CAND; nn++) {
            float fn = f0 * (float)nn * sqrtf((1.0f - b) + b * (float)nn * (float)nn);
            float phi = 2.0f * sinf(nn * PI * in) * sinf(nn * PI * out);
            cand[nc].freq = fn; cand[nc].amp = phi; nc++;
            if (fn > nyq) break;
        }
    } else {
        /* Plate / Membrane: 2D rectangular modes. */
        float aspect = 1.0f + 4.0f * clampf(mstruct, 0.0f, 1.0f);   /* 1..5 */
        float inX = 0.15f, inY = 0.15f;
        float outX = clampf(0.1f + 0.85f * mpos, 0.02f, 0.98f), outY = 0.55f;
        int kmax = (int)ceilf(sqrtf((float)MAX_MODES)) + 3;
        int mmax = kmax;
        int nmax = (int)ceilf(kmax * aspect) + 2;
        float dplate = 1.0f + 1.0f / (aspect * aspect);
        float dmemb  = sqrtf(dplate);
        for (int mm = 1; mm <= mmax && nc < MAX_CAND; mm++) {
            for (int nn = 1; nn <= nmax && nc < MAX_CAND; nn++) {
                float fm = (float)mm, fnn = (float)nn / aspect;
                float fr;
                if (model == MODEL_PLATE)
                    fr = f0 * (fm * fm + fnn * fnn) / dplate;
                else
                    fr = f0 * sqrtf(fm * fm + fnn * fnn) / dmemb;
                float phi = (2.0f * sinf(mm * PI * inX) * sinf(nn * PI * inY))
                          * (2.0f * sinf(mm * PI * outX) * sinf(nn * PI * outY));
                cand[nc].freq = fr; cand[nc].amp = phi; nc++;
            }
        }
    }

    qsort(cand, nc, sizeof(cand_t), cand_cmp);

    m->n = 0;
    for (int i = 0; i < nc && m->n < MAX_MODES; i++) {
        float fr = cand[i].freq;
        if (fr < 20.0f || fr > nyq) continue;
        float ratio = (f0 > 0.0f) ? (fr / f0) : 1.0f;
        /* Q capped at 2000 (long ring, no runaway); gain-comp capped modest. */
        float zeta = clampf(zeta0 + zeta1 * (ratio - 1.0f), 2.5e-4f, 0.99f);
        float Q = clampf(1.0f / (2.0f * zeta), 0.5f, 2000.0f);
        biquad_bandpass(&m->f[m->n], fr, Q);
        /* band-pass level compensation ~ sqrt(zetaRef/zeta) */
        float gc = clampf(sqrtf(0.02f / zeta), 0.5f, 16.0f);
        m->amp[m->n] = cand[i].amp * gc;
        m->n++;
    }
    /* Auto-level: drive a fixed reference noise burst through the bank and
     * measure the output peak, then normalize to a target. This makes level
     * consistent across mode count / resonance / pitch — the single biggest
     * balance problem with modal banks (narrow high-Q modes catch little
     * energy from a short excitation, so amplitude-based normalization is
     * wildly off). */
    for (int k = 0; k < m->n; k++) { m->f[k].z1 = 0.0f; m->f[k].z2 = 0.0f; }
    uint32_t seed = 0x2545F491u;
    double sumsq = 0.0;
    const int MW = 4096;                 /* ~93 ms — captures the ring, not just the onset */
    for (int s = 0; s < MW; s++) {
        float in = 0.0f;
        if (s < 128) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                       in = (float)(seed & 0xFFFF) / 32768.0f - 1.0f; }
        float y = 0.0f;
        for (int k = 0; k < m->n; k++) y += m->amp[k] * biquad_process(&m->f[k], in);
        sumsq += (double)y * (double)y;
    }
    for (int k = 0; k < m->n; k++) { m->f[k].z1 = 0.0f; m->f[k].z2 = 0.0f; }
    float rms = (float)sqrt(sumsq / (double)MW);
    /* Normalize to a target RMS: window-RMS equalizes for decay length (long
     * rings measure high -> quieter; short plucks measure low -> louder). */
    m->norm = 0.18f / (rms + 1e-9f);
}

static inline float modal_process(modal_t *m, float input) {
    float y = 0.0f;
    for (int k = 0; k < m->n; k++)
        y += m->amp[k] * biquad_process(&m->f[k], input);
    return y * m->norm;
}

/* ── Resonator wrapper (model selectable) ────────────────────────────────────── */

typedef struct {
    int model;
    waveguide_t wg;
    modal_t     modal;
} resonator_t;

static void reso_reset(resonator_t *r) {
    wg_reset(&r->wg);
    modal_reset(&r->modal);
}

/* Update coefficients each block. f0 already includes the resonator's tune. */
static void reso_set(resonator_t *r, int model, float f0, float mstruct,
                     float decay, float damp, float pos, float tone, float tension) {
    if (model != r->model) {
        r->model = model;
        if (model == MODEL_STRING) wg_reset(&r->wg);
        else                        modal_reset(&r->modal);
    }
    if (model == MODEL_STRING) {
        wg_set(&r->wg, f0, mstruct, decay, tone, damp, tension);
        r->wg.pos = clampf(0.02f + 0.9f * pos, 0.02f, 0.98f);
    } else {
        modal_t *m = &r->modal;
        int dirty = (m->f0 < 0.0f)
            || fabsf(m->f0 - f0) > 0.01f
            || fabsf(m->mstruct - mstruct) > 1e-3f
            || fabsf(m->mdecay - decay)   > 1e-3f
            || fabsf(m->mdamp - damp)     > 1e-3f
            || fabsf(m->mpos - pos)       > 1e-3f;
        if (dirty) modal_recompute(m, model, f0, mstruct, decay, damp, pos);
    }
}

static inline float reso_process(resonator_t *r, float input) {
    return (r->model == MODEL_STRING) ? wg_process(&r->wg, input)
                                      : modal_process(&r->modal, input);
}

/* ── Exciter ─────────────────────────────────────────────────────────────────── */

typedef struct {
    float env;
    int   stage;          /* 0 attack, 1 decay, 2 done */
    float mphase;         /* mallet single-cycle phase */
    int   mallet_on;
    svf_t svf;
    uint32_t rng;
} exciter_t;

static void exciter_note_on(exciter_t *e) {
    e->env = 0.0f; e->stage = 0; e->mphase = 0.0f; e->mallet_on = 1;
}

/* freq: note freq for the mallet cycle. Returns mono excitation sample. */
static inline float exciter_process(exciter_t *e, float freq, float mix, float crackle,
                                    float color_cut, float reso, float attack_ms,
                                    float decay_ms) {
    /* AD (one-shot burst). */
    if (e->stage == 0) {
        e->env += 1.0f / (attack_ms * 0.001f * SR);
        if (e->env >= 1.0f) { e->env = 1.0f; e->stage = 1; }
    } else if (e->stage == 1) {
        e->env -= 1.0f / (decay_ms * 0.001f * SR);
        if (e->env <= 0.0f) { e->env = 0.0f; e->stage = 2; }
    }
    if (e->stage == 2 && e->mallet_on == 0) return 0.0f;

    float noise = randbi(&e->rng);
    float mallet = 0.0f;
    if (e->mallet_on) {
        mallet = sinf(TWO_PI * e->mphase);
        e->mphase += freq * SR_INV;
        if (e->mphase >= 1.0f) e->mallet_on = 0;
    }
    float raw = (1.0f - mix) * noise + mix * mallet;

    /* Crackle impulses. */
    if (crackle > 0.0001f) {
        float p = crackle * 0.03f;
        if (randf(&e->rng) < p) raw += randbi(&e->rng) * 1.5f;
    }

    float filtered = svf_lp(&e->svf, raw, color_cut, reso);
    return filtered * e->env;
}

/* ── Voice ───────────────────────────────────────────────────────────────────── */

typedef struct {
    int    active;
    int    note;
    float  velocity;
    float  freq, freq_target;      /* glide */
    exciter_t exc;
    resonator_t A, B;
    dcblk_t dcA, dcB;
    float  prevA, prevB;           /* 1-sample coupling feedback (guarded) */
    float  amp_env;
    int    amp_stage;              /* 0 attack, 1 hold, 2 release, 3 off */
    float  pan_l, pan_r;
    int    silent;                 /* consecutive near-silent samples */
    int    held;
} voice_t;

/* ── Parameter block (also the preset layout) ────────────────────────────────── */

typedef struct {
    float exc_mix, exc_crackle, exc_color, exc_attack, exc_decay, exc_reso, vel_level, vel_color;
    int   a_model; float a_struct, a_decay, a_damp, a_pos, a_tone; int a_tune; float a_tension;
    int   b_model; float b_struct, b_decay, b_damp, b_pos, b_tone; int b_tune; float b_tension;
    /* NOTE: the 7th Voice slot is the preset's reverb mix (was "space"). */
    float couple, balance, glide, amp_attack, amp_release, spread, rev_mix, level;
    float makeup;   /* per-preset level compensation (not a knob) */
    /* FX extras (defaulted in apply_preset; zero-filled by the preset table). */
    float drive, rev_size, rev_damp, dly_mix, dly_time, dly_fb, dly_tone, width;
} params_t;

/* ── Simple stereo Schroeder reverb (Space) ──────────────────────────────────── */

#define NCOMB 4
#define NAP   2
static const int COMB_LEN[NCOMB] = { 1557, 1617, 1491, 1422 };
static const int AP_LEN[NAP]     = { 225, 556 };

typedef struct {
    float comb[NCOMB][1700];
    int   ci[NCOMB];
    float clp[NCOMB];
    float ap[NAP][600];
    int   ai[NAP];
} reverb_ch_t;

static float reverb_ch(reverb_ch_t *r, float x, float fb, float damp) {
    float acc = 0.0f;
    for (int c = 0; c < NCOMB; c++) {
        int L = COMB_LEN[c];
        float y = r->comb[c][r->ci[c]];
        r->clp[c] = y * (1.0f - damp) + r->clp[c] * damp + 1e-20f;   /* denormal guard */
        r->comb[c][r->ci[c]] = x + r->clp[c] * fb;
        r->ci[c]++; if (r->ci[c] >= L) r->ci[c] = 0;
        acc += y;
    }
    acc *= 0.25f;
    for (int a = 0; a < NAP; a++) {
        int L = AP_LEN[a];
        float bufv = r->ap[a][r->ai[a]];
        float y = -acc + bufv;
        r->ap[a][r->ai[a]] = acc + bufv * 0.5f;
        r->ai[a]++; if (r->ai[a] >= L) r->ai[a] = 0;
        acc = y;
    }
    return acc;
}

/* ── Instance ────────────────────────────────────────────────────────────────── */

typedef struct {
    params_t p;
    voice_t  v[MAX_VOICES];
    int      voice_cursor;
    int      current_page;
    int      preset_idx;
    uint32_t rng;
    reverb_ch_t rvL, rvR;
    /* Stereo delay line. */
    float    dlyL[DLY_MAX], dlyR[DLY_MAX];
    int      dly_pos;
    float    dly_lpL, dly_lpR;
    float    dly_time_smooth;
    int      any_held;
    /* Hidden metering (offline preset-gain calibration): pre-clamp peak/RMS. */
    double   meter_sumsq; float meter_peak; long meter_cnt;
} fizzik_t;

/* ── Presets ─────────────────────────────────────────────────────────────────── */
/* {exc_mix,crackle,color,attack,decay,reso,velL,velC,
    aMdl,aStr,aDec,aDmp,aPos,aTone,aTune,aTens,
    bMdl,bStr,bDec,bDmp,bPos,bTone,bTune,bTens,
    couple,balance,glide,ampA,ampR,spread,space,level, makeup} — makeup baked from offline meter (peak->0.8) */
static const params_t PRESETS[N_PRESETS] = {
  /* AlienChurch */    {0.2f,0.1f,0.7f,0.05f,0.4f,0.3f,0.3f,0.4f, MODEL_MEMBRANE,0.6f,0.9f,0.5f,0.5f,0.7f,0,0.0f, MODEL_PLATE,0.4f,0.85f,0.4f,0.4f,0.6f,7,0.0f, 0.35f,0.5f,0.2f,0.3f,0.7f,0.6f,0.6f,0.6f, 0.323f},
  /* BowedGlass */     {0.0f,0.0f,0.5f,0.4f,0.6f,0.2f,0.2f,0.3f, MODEL_PLATE,0.3f,0.95f,0.6f,0.4f,0.6f,0,0.0f, MODEL_PLATE,0.5f,0.9f,0.5f,0.5f,0.7f,12,0.0f, 0.5f,0.5f,0.1f,0.5f,0.6f,0.5f,0.5f,0.55f, 0.778f},
  /* CaveStrings */    {0.1f,0.05f,0.6f,0.1f,0.5f,0.4f,0.4f,0.3f, MODEL_STRING,0.2f,0.85f,0.4f,0.15f,0.6f,0,0.2f, MODEL_STRING,0.1f,0.8f,0.5f,0.25f,0.55f,-12,0.15f, 0.3f,0.5f,0.15f,0.05f,0.5f,0.6f,0.7f,0.6f, 1.048f},
  /* CouncilsPiano */  {0.6f,0.0f,0.75f,0.01f,0.15f,0.2f,0.6f,0.5f, MODEL_STRING,0.35f,0.75f,0.6f,0.2f,0.7f,0,0.25f, MODEL_BEAM,0.15f,0.7f,0.6f,0.5f,0.65f,12,0.0f, 0.15f,0.35f,0.0f,0.02f,0.4f,0.4f,0.35f,0.6f, 2.172f},
  /* DistortedBass */  {0.4f,0.2f,0.4f,0.01f,0.2f,0.6f,0.7f,0.4f, MODEL_STRING,0.0f,0.7f,0.3f,0.3f,0.4f,-12,0.6f, MODEL_STRING,0.05f,0.65f,0.4f,0.35f,0.35f,-12,0.7f, 0.55f,0.5f,0.05f,0.02f,0.35f,0.3f,0.2f,0.7f, 1.755f},
  /* FeedbackHarp */   {0.2f,0.1f,0.65f,0.02f,0.25f,0.5f,0.5f,0.5f, MODEL_STRING,0.15f,0.92f,0.5f,0.4f,0.7f,0,0.3f, MODEL_STRING,0.2f,0.9f,0.55f,0.6f,0.6f,7,0.35f, 0.7f,0.5f,0.1f,0.03f,0.5f,0.55f,0.5f,0.55f, 1.630f},
  /* JudgementAwaits */{0.15f,0.15f,0.55f,0.08f,0.5f,0.35f,0.3f,0.4f, MODEL_PLATE,0.7f,0.9f,0.6f,0.45f,0.5f,-12,0.0f, MODEL_MEMBRANE,0.5f,0.88f,0.5f,0.5f,0.45f,0,0.0f, 0.45f,0.5f,0.2f,0.2f,0.8f,0.6f,0.75f,0.6f, 0.356f},
  /* OldResonances */  {0.1f,0.1f,0.5f,0.1f,0.55f,0.3f,0.35f,0.35f, MODEL_MEMBRANE,0.4f,0.85f,0.55f,0.5f,0.6f,0,0.0f, MODEL_PLATE,0.6f,0.8f,0.45f,0.4f,0.55f,5,0.0f, 0.4f,0.5f,0.15f,0.15f,0.65f,0.6f,0.65f,0.6f, 0.381f},
  /* PreparedPiano */  {0.5f,0.35f,0.7f,0.01f,0.18f,0.3f,0.6f,0.5f, MODEL_STRING,0.4f,0.7f,0.55f,0.25f,0.7f,0,0.2f, MODEL_MEMBRANE,0.3f,0.6f,0.6f,0.4f,0.5f,12,0.0f, 0.4f,0.45f,0.0f,0.02f,0.4f,0.45f,0.35f,0.62f, 0.613f},
  /* RythmicBow */     {0.05f,0.05f,0.55f,0.3f,0.5f,0.3f,0.3f,0.4f, MODEL_STRING,0.25f,0.9f,0.45f,0.3f,0.6f,0,0.25f, MODEL_PLATE,0.4f,0.85f,0.5f,0.45f,0.6f,7,0.0f, 0.55f,0.5f,0.1f,0.3f,0.55f,0.5f,0.5f,0.55f, 0.423f},
  /* SensitiveSkin */  {0.3f,0.2f,0.6f,0.02f,0.3f,0.45f,0.55f,0.5f, MODEL_MEMBRANE,0.5f,0.75f,0.5f,0.5f,0.55f,0,0.0f, MODEL_STRING,0.15f,0.8f,0.5f,0.35f,0.6f,0,0.3f, 0.45f,0.5f,0.05f,0.05f,0.45f,0.55f,0.45f,0.6f, 0.441f},
  /* Sharp */          {0.5f,0.05f,0.85f,0.005f,0.1f,0.3f,0.7f,0.6f, MODEL_BEAM,0.6f,0.65f,0.7f,0.3f,0.8f,0,0.0f, MODEL_BEAM,0.8f,0.6f,0.75f,0.5f,0.75f,12,0.0f, 0.2f,0.5f,0.0f,0.01f,0.3f,0.5f,0.3f,0.6f, 3.890f},
  /* ShockingPluck */  {0.55f,0.1f,0.7f,0.005f,0.12f,0.4f,0.75f,0.6f, MODEL_STRING,0.3f,0.72f,0.6f,0.2f,0.75f,0,0.4f, MODEL_STRING,0.2f,0.68f,0.6f,0.3f,0.7f,0,0.45f, 0.25f,0.5f,0.0f,0.01f,0.35f,0.5f,0.3f,0.65f, 1.086f},
  /* Slappy */         {0.35f,0.25f,0.6f,0.005f,0.1f,0.5f,0.7f,0.5f, MODEL_MEMBRANE,0.3f,0.55f,0.6f,0.4f,0.45f,0,0.0f, MODEL_STRING,0.1f,0.6f,0.5f,0.3f,0.4f,-12,0.5f, 0.3f,0.45f,0.0f,0.01f,0.3f,0.45f,0.25f,0.68f, 0.297f},
  /* SurroundedByBells*/{0.4f,0.05f,0.8f,0.01f,0.3f,0.25f,0.5f,0.5f, MODEL_PLATE,0.8f,0.92f,0.55f,0.5f,0.75f,12,0.0f, MODEL_PLATE,0.5f,0.9f,0.6f,0.45f,0.7f,19,0.0f, 0.35f,0.5f,0.1f,0.02f,0.7f,0.65f,0.7f,0.55f, 0.591f},
  /* XyloStyle */      {0.6f,0.0f,0.85f,0.005f,0.12f,0.2f,0.65f,0.55f, MODEL_BEAM,0.9f,0.6f,0.7f,0.35f,0.8f,0,0.0f, MODEL_BEAM,0.7f,0.55f,0.75f,0.45f,0.75f,12,0.0f, 0.15f,0.5f,0.0f,0.01f,0.35f,0.5f,0.3f,0.62f, 2.006f},
};

/* ── Parameter descriptor table (float/int fields of params_t) ───────────────── */

typedef struct { const char *key; const char *name; int isint; float mn, mx, step; size_t off; } pdesc_t;
#define PF(k,nm,mn,mx,st,fld) { k, nm, 0, mn, mx, st, offsetof(params_t, fld) }
#define PI_(k,nm,mn,mx,st,fld){ k, nm, 1, mn, mx, st, offsetof(params_t, fld) }

static const pdesc_t PDESC[] = {
    PF("exc_mix","Exc Mix",0,1,0.01f,exc_mix), PF("exc_crackle","Crackle",0,1,0.01f,exc_crackle),
    PF("exc_color","Color",0,1,0.01f,exc_color), PF("exc_attack","Attack",0,1,0.01f,exc_attack),
    PF("exc_decay","Decay",0,1,0.01f,exc_decay), PF("exc_reso","Exc Reso",0,1,0.01f,exc_reso),
    PF("vel_level","Vel Level",0,1,0.01f,vel_level), PF("vel_color","Vel Color",0,1,0.01f,vel_color),

    PI_("a_model","Model A",0,3,1,a_model), PF("a_struct","Structure A",0,1,0.01f,a_struct),
    PF("a_decay","Decay A",0,1,0.01f,a_decay), PF("a_damp","Damp A",0,1,0.01f,a_damp),
    PF("a_pos","Position A",0,1,0.01f,a_pos), PF("a_tone","Tone A",0,1,0.01f,a_tone),
    PI_("a_tune","Tune A",-24,24,1,a_tune), PF("a_tension","Tension A",0,1,0.01f,a_tension),

    PI_("b_model","Model B",0,3,1,b_model), PF("b_struct","Structure B",0,1,0.01f,b_struct),
    PF("b_decay","Decay B",0,1,0.01f,b_decay), PF("b_damp","Damp B",0,1,0.01f,b_damp),
    PF("b_pos","Position B",0,1,0.01f,b_pos), PF("b_tone","Tone B",0,1,0.01f,b_tone),
    PI_("b_tune","Tune B",-24,24,1,b_tune), PF("b_tension","Tension B",0,1,0.01f,b_tension),

    PF("couple","Couple",0,1,0.01f,couple), PF("balance","Balance",0,1,0.01f,balance),
    PF("glide","Glide",0,1,0.01f,glide), PF("amp_attack","Amp Atk",0,1,0.01f,amp_attack),
    PF("amp_release","Amp Rel",0,1,0.01f,amp_release), PF("spread","Spread",0,1,0.01f,spread),
    PF("drive","Drive",0,1,0.01f,drive), PF("level","Level",0,1,0.01f,level),

    PF("rev_mix","Reverb",0,1,0.01f,rev_mix), PF("rev_size","Rev Size",0,1,0.01f,rev_size),
    PF("rev_damp","Rev Damp",0,1,0.01f,rev_damp), PF("dly_mix","Delay",0,1,0.01f,dly_mix),
    PF("dly_time","Dly Time",0,1,0.01f,dly_time), PF("dly_fb","Dly Fbk",0,1,0.01f,dly_fb),
    PF("dly_tone","Dly Tone",0,1,0.01f,dly_tone), PF("width","Width",0,1,0.01f,width),
};
static const int N_PDESC = (int)(sizeof(PDESC) / sizeof(PDESC[0]));

static const pdesc_t *find_pdesc(const char *key) {
    for (int i = 0; i < N_PDESC; i++)
        if (strcmp(PDESC[i].key, key) == 0) return &PDESC[i];
    return NULL;
}
static inline float *pf_ptr(fizzik_t *inst, const pdesc_t *d) {
    return (float *)((char *)&inst->p + d->off);
}
static inline int *pi_ptr(fizzik_t *inst, const pdesc_t *d) {
    return (int *)((char *)&inst->p + d->off);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────────── */

static void apply_preset(fizzik_t *inst, int idx) {
    if (idx < 0) idx = 0; if (idx >= N_PRESETS) idx = N_PRESETS - 1;
    inst->p = PRESETS[idx];
    if (inst->p.makeup < 1e-6f) inst->p.makeup = 1.0f;
    /* FX aren't in the positional preset table — give them musical defaults
     * (rev_mix comes from the preset's own value above). */
    inst->p.drive    = 0.0f;
    inst->p.rev_size = 0.55f;
    inst->p.rev_damp = 0.40f;
    inst->p.dly_mix  = 0.0f;
    inst->p.dly_time = 0.38f;
    inst->p.dly_fb   = 0.35f;
    inst->p.dly_tone = 0.5f;
    inst->p.width    = 0.5f;
    inst->preset_idx = idx;
}

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    fizzik_t *inst = (fizzik_t *)calloc(1, sizeof(fizzik_t));
    if (!inst) return NULL;

    inst->rng = 0xC0FFEEu;
    apply_preset(inst, 2);   /* CaveStrings — pleasant default */
    inst->current_page = 0;
    inst->dly_time_smooth = 16000.0f;

    for (int i = 0; i < MAX_VOICES; i++) {
        voice_t *v = &inst->v[i];
        v->exc.rng = 0x1000u + (uint32_t)i * 2654435761u;
        v->A.model = -1; v->B.model = -1;
        reso_reset(&v->A); reso_reset(&v->B);
        v->pan_l = v->pan_r = 0.70710678f;
    }
    return inst;
}

static void destroy_instance(void *instance) { free(instance); }

/* ── MIDI ────────────────────────────────────────────────────────────────────── */

static void voice_start(fizzik_t *inst, voice_t *v, int note, float vel) {
    v->active = 1; v->held = 1;
    v->note = note; v->velocity = vel;
    v->freq_target = note_to_freq((float)note);
    v->freq = v->freq_target;
    v->amp_env = 0.0f; v->amp_stage = 0;
    v->prevA = v->prevB = 0.0f;
    v->silent = 0;
    v->dcA.x1 = v->dcA.y1 = v->dcB.x1 = v->dcB.y1 = 0.0f;
    exciter_note_on(&v->exc);
    v->A.wg.snap = 1; v->B.wg.snap = 1;
    reso_reset(&v->A); reso_reset(&v->B);
    /* Stereo spread by voice index. */
    float sp = clampf(inst->p.spread, 0.0f, 1.0f);
    float pan = ((float)(note % 7) / 6.0f - 0.5f) * sp;   /* -0.5..0.5 */
    v->pan_l = cosf((pan * 0.5f + 0.5f) * (PI * 0.5f));
    v->pan_r = sinf((pan * 0.5f + 0.5f) * (PI * 0.5f));
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    fizzik_t *inst = (fizzik_t *)instance;
    if (len < 3) return;
    uint8_t status = msg[0] & 0xF0;
    int note = msg[1];
    int vel  = msg[2];

    if (status == 0x90 && vel > 0) {
        /* find free voice, else steal via round-robin cursor */
        int slot = -1;
        for (int i = 0; i < MAX_VOICES; i++) if (!inst->v[i].active) { slot = i; break; }
        if (slot < 0) { slot = inst->voice_cursor; inst->voice_cursor = (slot + 1) % MAX_VOICES; }
        voice_start(inst, &inst->v[slot], note, vel / 127.0f);
    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        for (int i = 0; i < MAX_VOICES; i++)
            if (inst->v[i].active && inst->v[i].held && inst->v[i].note == note) {
                inst->v[i].held = 0;
                if (inst->v[i].amp_stage < 2) inst->v[i].amp_stage = 2;   /* release */
            }
    }
}

/* ── Parameters ──────────────────────────────────────────────────────────────── */

static void rnd_exciter(fizzik_t *inst) {
    uint32_t *s = &inst->rng;
    inst->p.exc_mix = randf(s); inst->p.exc_crackle = randf(s) * 0.5f;
    inst->p.exc_color = 0.3f + 0.6f * randf(s); inst->p.exc_attack = randf(s) * 0.3f;
    inst->p.exc_decay = 0.1f + 0.5f * randf(s); inst->p.exc_reso = randf(s) * 0.6f;
}
static void rnd_one_reso(fizzik_t *inst, int isB) {
    uint32_t *s = &inst->rng;
    int mdl = (int)(randf(s) * 4.0f); if (mdl > 3) mdl = 3;
    float str = randf(s), dec = 0.4f + 0.55f * randf(s), dmp = 0.3f + 0.5f * randf(s);
    float pos = 0.2f + 0.6f * randf(s), tone = 0.3f + 0.6f * randf(s);
    int tune = (int)(randbi(s) * 12.0f);
    float tens = (mdl <= MODEL_BEAM) ? randf(s) * 0.5f : 0.0f;
    if (!isB) { inst->p.a_model=mdl; inst->p.a_struct=str; inst->p.a_decay=dec; inst->p.a_damp=dmp;
                inst->p.a_pos=pos; inst->p.a_tone=tone; inst->p.a_tune=tune; inst->p.a_tension=tens; }
    else      { inst->p.b_model=mdl; inst->p.b_struct=str; inst->p.b_decay=dec; inst->p.b_damp=dmp;
                inst->p.b_pos=pos; inst->p.b_tone=tone; inst->p.b_tune=tune; inst->p.b_tension=tens; }
}
static void rnd_reson(fizzik_t *inst) { rnd_one_reso(inst, 0); rnd_one_reso(inst, 1); }
static void rnd_patch(fizzik_t *inst) {
    rnd_exciter(inst); rnd_reson(inst);
    inst->p.couple = randf(&inst->rng) * 0.7f;
    inst->p.balance = 0.35f + 0.3f * randf(&inst->rng);
    inst->p.makeup = 1.0f;   /* rely on per-resonator auto-level */
}

/* Set one param field from a numeric string. */
static void set_field(fizzik_t *inst, const pdesc_t *d, const char *val) {
    if (d->isint) *pi_ptr(inst, d) = (int)clampf((float)atoi(val), d->mn, d->mx);
    else          *pf_ptr(inst, d) = clampf((float)atof(val), d->mn, d->mx);
}

/* Model enum accepts a name or an index. */
static int parse_model(const char *val) {
    for (int i = 0; i < 4; i++) if (strcmp(val, MODEL_NAMES[i]) == 0) return i;
    int idx = atoi(val); return (idx < 0) ? 0 : (idx > 3) ? 3 : idx;
}

static void set_param(void *instance, const char *key, const char *val) {
    fizzik_t *inst = (fizzik_t *)instance;
    if (!inst || !key || !val) return;

    /* Page navigation. */
    if (strcmp(key, "_level") == 0 || strcmp(key, "current_level") == 0) {
        if      (strcmp(val, "Exciter") == 0 || strcmp(val, "root") == 0 || strcmp(val, "Fizzik") == 0) inst->current_page = 0;
        else if (strcmp(val, "ResonA") == 0) inst->current_page = 1;
        else if (strcmp(val, "ResonB") == 0) inst->current_page = 2;
        else if (strcmp(val, "Voice")  == 0) inst->current_page = 3;
        else if (strcmp(val, "FX")     == 0) inst->current_page = 4;
        else if (strcmp(val, "Patch")  == 0) inst->current_page = 5;
        return;
    }

    /* Knob overlay: knob_N_adjust (page-aware). */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int idx = atoi(key + 5) - 1;
        if (idx < 0 || idx >= PAGE_NKNOBS[inst->current_page]) return;
        const char *pk = PAGE_KEYS[inst->current_page][idx];
        if (!pk || !pk[0]) return;
        int delta = atoi(val);
        /* triggers / preset handled by name below */
        if (strcmp(pk, "preset") == 0) {
            int ni = inst->preset_idx + delta;
            if (ni < 0) ni = 0; if (ni >= N_PRESETS) ni = N_PRESETS - 1;
            apply_preset(inst, ni); return;
        }
        if (strncmp(pk, "rnd_", 4) == 0) {
            if (delta != 0) { if (!strcmp(pk,"rnd_patch")) rnd_patch(inst);
                              else if (!strcmp(pk,"rnd_exc")) rnd_exciter(inst);
                              else if (!strcmp(pk,"rnd_reson")) rnd_reson(inst); }
            return;
        }
        const pdesc_t *d = find_pdesc(pk);
        if (d) {
            if (d->isint) *pi_ptr(inst, d) = (int)clampf((float)(*pi_ptr(inst, d) + delta), d->mn, d->mx);
            else          *pf_ptr(inst, d) = clampf(*pf_ptr(inst, d) + (float)delta * d->step, d->mn, d->mx);
        }
        return;
    }

    /* Triggers (direct set by key). */
    if (strncmp(key, "rnd_", 4) == 0) {
        if (atoi(val) != 0) {
            if (!strcmp(key,"rnd_patch")) rnd_patch(inst);
            else if (!strcmp(key,"rnd_exc")) rnd_exciter(inst);
            else if (!strcmp(key,"rnd_reson")) rnd_reson(inst);
        }
        return;
    }
    if (strcmp(key, "preset") == 0) {
        for (int i = 0; i < N_PRESETS; i++) if (strcmp(val, PRESET_NAMES[i]) == 0) { apply_preset(inst, i); return; }
        apply_preset(inst, atoi(val)); return;
    }
    if (strcmp(key, "a_model") == 0) { inst->p.a_model = parse_model(val); return; }
    if (strcmp(key, "b_model") == 0) { inst->p.b_model = parse_model(val); return; }

    /* State restore: newline-separated key=value. */
    if (strcmp(key, "state") == 0) {
        const char *pp = val;
        while (*pp) {
            const char *eol = strchr(pp, '\n'); if (!eol) eol = pp + strlen(pp);
            const char *eq = (const char *)memchr(pp, '=', eol - pp);
            if (eq && eq > pp) {
                char k[48], v[64];
                int kl = (int)(eq - pp), vl = (int)(eol - eq - 1);
                if (kl > 0 && kl < (int)sizeof(k) && vl >= 0 && vl < (int)sizeof(v)) {
                    memcpy(k, pp, kl); k[kl] = 0; memcpy(v, eq + 1, vl); v[vl] = 0;
                    if (strcmp(k, "state") != 0) set_param(inst, k, v);
                }
            }
            if (!*eol) break; pp = eol + 1;
        }
        return;
    }

    /* Regular fields. */
    const pdesc_t *d = find_pdesc(key);
    if (d) set_field(inst, d, val);
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    fizzik_t *inst = (fizzik_t *)instance;
    if (!inst || !key) return -1;

    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Fizzik");

    /* Hidden calibration meter: "peak rms", resets on read. */
    if (strcmp(key, "__meter") == 0) {
        double rms = inst->meter_cnt ? sqrt(inst->meter_sumsq / (double)inst->meter_cnt) : 0.0;
        int r = snprintf(buf, buf_len, "%.5f %.5f", inst->meter_peak, rms);
        inst->meter_peak = 0.0f; inst->meter_sumsq = 0.0; inst->meter_cnt = 0;
        return r;
    }

    if (strcmp(key, "chain_params") == 0) {
        return snprintf(buf, buf_len,
          "["
          "{\"key\":\"exc_mix\",\"name\":\"Exc Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"exc_crackle\",\"name\":\"Crackle\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"exc_color\",\"name\":\"Color\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"exc_attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"exc_decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"exc_reso\",\"name\":\"Exc Reso\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"vel_level\",\"name\":\"Vel Level\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"vel_color\",\"name\":\"Vel Color\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"a_model\",\"name\":\"Model A\",\"type\":\"enum\",\"options\":[\"String\",\"Beam\",\"Plate\",\"Membrane\"]},"
          "{\"key\":\"a_struct\",\"name\":\"Structure A\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"a_decay\",\"name\":\"Decay A\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"a_damp\",\"name\":\"Damp A\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"a_pos\",\"name\":\"Position A\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"a_tone\",\"name\":\"Tone A\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"a_tune\",\"name\":\"Tune A\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1},"
          "{\"key\":\"a_tension\",\"name\":\"Tension A\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"b_model\",\"name\":\"Model B\",\"type\":\"enum\",\"options\":[\"String\",\"Beam\",\"Plate\",\"Membrane\"]},"
          "{\"key\":\"b_struct\",\"name\":\"Structure B\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"b_decay\",\"name\":\"Decay B\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"b_damp\",\"name\":\"Damp B\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"b_pos\",\"name\":\"Position B\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"b_tone\",\"name\":\"Tone B\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"b_tune\",\"name\":\"Tune B\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1},"
          "{\"key\":\"b_tension\",\"name\":\"Tension B\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"couple\",\"name\":\"Couple\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"balance\",\"name\":\"Balance\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"glide\",\"name\":\"Glide\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"amp_attack\",\"name\":\"Amp Atk\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"amp_release\",\"name\":\"Amp Rel\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"spread\",\"name\":\"Spread\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"drive\",\"name\":\"Drive\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"level\",\"name\":\"Level\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"rev_mix\",\"name\":\"Reverb\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"rev_size\",\"name\":\"Rev Size\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"rev_damp\",\"name\":\"Rev Damp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"dly_mix\",\"name\":\"Delay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"dly_time\",\"name\":\"Dly Time\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"dly_fb\",\"name\":\"Dly Fbk\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"dly_tone\",\"name\":\"Dly Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"width\",\"name\":\"Width\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"enum\",\"options\":[\"AlienChurch\",\"BowedGlass\",\"CaveStrings\",\"CouncilsPiano\",\"DistortedBass\",\"FeedbackHarp\",\"JudgementAwaits\",\"OldResonances\",\"PreparedPiano\",\"RythmicBow\",\"SensitiveSkin\",\"Sharp\",\"ShockingPluck\",\"Slappy\",\"SurroundedByBells\",\"XyloStyle\"]},"
          "{\"key\":\"rnd_patch\",\"name\":\"Rnd Patch\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
          "{\"key\":\"rnd_exc\",\"name\":\"Rnd Exciter\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
          "{\"key\":\"rnd_reson\",\"name\":\"Rnd Reson\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1}"
          "]");
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        return snprintf(buf, buf_len,
          "{\"modes\":null,\"levels\":{"
          "\"root\":{\"name\":\"Fizzik\",\"knobs\":[\"exc_mix\",\"exc_crackle\",\"exc_color\",\"exc_attack\",\"exc_decay\",\"exc_reso\",\"vel_level\",\"vel_color\"],"
          "\"params\":[{\"level\":\"Exciter\",\"label\":\"Exciter\"},{\"level\":\"ResonA\",\"label\":\"Reson A\"},{\"level\":\"ResonB\",\"label\":\"Reson B\"},{\"level\":\"Voice\",\"label\":\"Voice\"},{\"level\":\"FX\",\"label\":\"FX\"},{\"level\":\"Patch\",\"label\":\"Patch\"}]},"
          "\"Exciter\":{\"name\":\"Exciter\",\"knobs\":[\"exc_mix\",\"exc_crackle\",\"exc_color\",\"exc_attack\",\"exc_decay\",\"exc_reso\",\"vel_level\",\"vel_color\"],\"params\":[\"exc_mix\",\"exc_crackle\",\"exc_color\",\"exc_attack\",\"exc_decay\",\"exc_reso\",\"vel_level\",\"vel_color\"]},"
          "\"ResonA\":{\"name\":\"Reson A\",\"knobs\":[\"a_model\",\"a_struct\",\"a_decay\",\"a_damp\",\"a_pos\",\"a_tone\",\"a_tune\",\"a_tension\"],\"params\":[\"a_model\",\"a_struct\",\"a_decay\",\"a_damp\",\"a_pos\",\"a_tone\",\"a_tune\",\"a_tension\"]},"
          "\"ResonB\":{\"name\":\"Reson B\",\"knobs\":[\"b_model\",\"b_struct\",\"b_decay\",\"b_damp\",\"b_pos\",\"b_tone\",\"b_tune\",\"b_tension\"],\"params\":[\"b_model\",\"b_struct\",\"b_decay\",\"b_damp\",\"b_pos\",\"b_tone\",\"b_tune\",\"b_tension\"]},"
          "\"Voice\":{\"name\":\"Voice\",\"knobs\":[\"couple\",\"balance\",\"glide\",\"amp_attack\",\"amp_release\",\"spread\",\"drive\",\"level\"],\"params\":[\"couple\",\"balance\",\"glide\",\"amp_attack\",\"amp_release\",\"spread\",\"drive\",\"level\"]},"
          "\"FX\":{\"name\":\"FX\",\"knobs\":[\"rev_mix\",\"rev_size\",\"rev_damp\",\"dly_mix\",\"dly_time\",\"dly_fb\",\"dly_tone\",\"width\"],\"params\":[\"rev_mix\",\"rev_size\",\"rev_damp\",\"dly_mix\",\"dly_time\",\"dly_fb\",\"dly_tone\",\"width\"]},"
          "\"Patch\":{\"name\":\"Patch\",\"knobs\":[\"preset\",\"rnd_patch\",\"rnd_exc\",\"rnd_reson\"],\"params\":[\"preset\",\"rnd_patch\",\"rnd_exc\",\"rnd_reson\"]}"
          "}}");
    }

    /* Knob overlay label/value (page-aware). */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 5) - 1;
        if (idx < 0 || idx >= PAGE_NKNOBS[inst->current_page]) return 0;
        const char *pk = PAGE_KEYS[inst->current_page][idx];
        if (strcmp(pk, "preset") == 0) return snprintf(buf, buf_len, "Preset");
        if (strcmp(pk, "rnd_patch") == 0) return snprintf(buf, buf_len, "Rnd Patch");
        if (strcmp(pk, "rnd_exc") == 0) return snprintf(buf, buf_len, "Rnd Exc");
        if (strcmp(pk, "rnd_reson") == 0) return snprintf(buf, buf_len, "Rnd Reson");
        const pdesc_t *d = find_pdesc(pk);
        return snprintf(buf, buf_len, "%s", d ? d->name : pk);
    }
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int idx = atoi(key + 5) - 1;
        if (idx < 0 || idx >= PAGE_NKNOBS[inst->current_page]) return 0;
        const char *pk = PAGE_KEYS[inst->current_page][idx];
        if (strcmp(pk, "preset") == 0) return snprintf(buf, buf_len, "%s", PRESET_NAMES[inst->preset_idx]);
        if (strncmp(pk, "rnd_", 4) == 0) return snprintf(buf, buf_len, "Go");
        if (strcmp(pk, "a_model") == 0) return snprintf(buf, buf_len, "%s", MODEL_NAMES[inst->p.a_model & 3]);
        if (strcmp(pk, "b_model") == 0) return snprintf(buf, buf_len, "%s", MODEL_NAMES[inst->p.b_model & 3]);
        const pdesc_t *d = find_pdesc(pk);
        if (d) {
            if (d->isint) return snprintf(buf, buf_len, "%d", *pi_ptr(inst, d));
            return snprintf(buf, buf_len, "%d%%", (int)(*pf_ptr(inst, d) * 100.0f));
        }
        return 0;
    }

    /* State serialize. */
    if (strcmp(key, "state") == 0) {
        int n = 0;
        for (int i = 0; i < N_PDESC; i++) {
            const pdesc_t *d = &PDESC[i];
            if (d->isint) n += snprintf(buf + n, buf_len - n, "%s=%d\n", d->key, *pi_ptr(inst, d));
            else          n += snprintf(buf + n, buf_len - n, "%s=%.5f\n", d->key, (double)*pf_ptr(inst, d));
        }
        return n;
    }

    /* Enums by key -> string names. */
    if (strcmp(key, "a_model") == 0) return snprintf(buf, buf_len, "%s", MODEL_NAMES[inst->p.a_model & 3]);
    if (strcmp(key, "b_model") == 0) return snprintf(buf, buf_len, "%s", MODEL_NAMES[inst->p.b_model & 3]);
    if (strcmp(key, "preset")  == 0) return snprintf(buf, buf_len, "%s", PRESET_NAMES[inst->preset_idx]);
    if (strncmp(key, "rnd_", 4) == 0) return snprintf(buf, buf_len, "0");

    /* Regular fields: raw values (raw for round-trip persistence). */
    const pdesc_t *d = find_pdesc(key);
    if (d) {
        if (d->isint) return snprintf(buf, buf_len, "%d", *pi_ptr(inst, d));
        return snprintf(buf, buf_len, "%.5f", (double)*pf_ptr(inst, d));
    }

    return -1;
}

/* ── Audio ───────────────────────────────────────────────────────────────────── */

static void render_block(void *instance, int16_t *out_lr, int frames) {
    fizzik_t *inst = (fizzik_t *)instance;
    params_t *p = &inst->p;

    /* Map global params once per block. */
    float couple      = clampf(p->couple, 0.0f, 1.0f);
    float balance     = clampf(p->balance, 0.0f, 1.0f);
    float glide_ms    = map_exp(p->glide + 1e-4f, 1.0f, 500.0f) * (p->glide < 1e-3f ? 0.0f : 1.0f);
    float glide_coef  = (glide_ms > 0.5f) ? expf(-1.0f / (glide_ms * 0.001f * SR)) : 0.0f;
    float amp_atk_ms  = map_exp(p->amp_attack, 0.5f, 200.0f);
    float amp_rel_ms  = map_exp(p->amp_release, 20.0f, 3000.0f);
    float atk_inc     = 1.0f / (amp_atk_ms * 0.001f * SR);
    float rel_coef    = expf(-1.0f / (amp_rel_ms * 0.001f * SR));
    float level_gain  = p->level * p->level * 1.4f * p->makeup;
    /* FX params. */
    float rev_mix   = clampf(p->rev_mix, 0.0f, 1.0f);
    float rev_fb    = 0.60f + 0.38f * clampf(p->rev_size, 0.0f, 1.0f);   /* 0.60..0.98 */
    float rev_damp  = 0.05f + 0.60f * clampf(p->rev_damp, 0.0f, 1.0f);   /* 0.05..0.65 */
    float dly_mix   = clampf(p->dly_mix, 0.0f, 1.0f);
    float dly_fb    = clampf(p->dly_fb, 0.0f, 1.0f) * 0.85f;
    float dly_cut   = map_exp(clampf(p->dly_tone, 0.0f, 1.0f), 800.0f, 12000.0f);
    float dly_lpc   = 1.0f - expf(-TWO_PI * dly_cut * SR_INV);
    float dly_target = clampf(map_exp(p->dly_time, 30.0f, 700.0f) * 0.001f * SR, 64.0f, (float)(DLY_MAX - 4));
    float drive     = clampf(p->drive, 0.0f, 1.0f);
    float drive_g   = 1.0f + drive * 4.0f;
    float drive_cmp = 1.0f / (1.0f + drive * 1.2f);
    float width     = clampf(p->width, 0.0f, 1.0f);

    /* Exciter param mapping. */
    float exc_mix    = clampf(p->exc_mix, 0.0f, 1.0f);
    float exc_crk    = clampf(p->exc_crackle, 0.0f, 1.0f);
    float exc_atk_ms = map_exp(p->exc_attack, 0.2f, 40.0f);
    float exc_dec_ms = map_exp(p->exc_decay, 2.0f, 400.0f);
    float exc_reso   = clampf(p->exc_reso, 0.0f, 1.0f);
    float vel_level  = clampf(p->vel_level, 0.0f, 1.0f);
    float vel_color  = clampf(p->vel_color, 0.0f, 1.0f);

    for (int n = 0; n < frames; n++) {
        float mixL = 0.0f, mixR = 0.0f;

        for (int vi = 0; vi < MAX_VOICES; vi++) {
            voice_t *v = &inst->v[vi];
            if (!v->active) continue;

            /* Glide toward target. */
            if (glide_coef > 0.0f) v->freq += (v->freq_target - v->freq) * (1.0f - glide_coef);
            else                   v->freq = v->freq_target;

            /* Per-block resonator retune only on first sample of the block. */
            if (n == 0) {
                float fA = v->freq * powf(2.0f, (float)p->a_tune / 12.0f);
                float fB = v->freq * powf(2.0f, (float)p->b_tune / 12.0f);
                reso_set(&v->A, p->a_model & 3, fA, p->a_struct, p->a_decay, p->a_damp, p->a_pos, p->a_tone, p->a_tension);
                reso_set(&v->B, p->b_model & 3, fB, p->b_struct, p->b_decay, p->b_damp, p->b_pos, p->b_tone, p->b_tension);
            }

            /* Exciter (velocity links). */
            float vcolor = clampf(p->exc_color + vel_color * (v->velocity - 0.5f), 0.0f, 1.0f);
            float color_cut = map_exp(vcolor, 300.0f, 16000.0f);
            float exc = exciter_process(&v->exc, v->freq, exc_mix, exc_crk, color_cut, exc_reso, exc_atk_ms, exc_dec_ms);
            float vgain = 1.0f - vel_level * (1.0f - v->velocity);
            exc *= vgain;

            /* Coupled resonators (1-sample cross feedback). */
            float inA = exc + couple * v->prevB;
            float inB = exc + couple * v->prevA;
            float rawA = reso_process(&v->A, inA);
            float rawB = reso_process(&v->B, inB);
            float gA = soft_limit(dcblk(&v->dcA, sanitize(rawA)));
            float gB = soft_limit(dcblk(&v->dcB, sanitize(rawB)));
            v->prevA = gA; v->prevB = gB;

            float mixed = (1.0f - balance) * gA + balance * gB;

            /* Amp envelope. */
            switch (v->amp_stage) {
                case 0: v->amp_env += atk_inc; if (v->amp_env >= 1.0f) { v->amp_env = 1.0f; v->amp_stage = 1; } break;
                case 1: break;
                case 2: v->amp_env *= rel_coef; if (v->amp_env < 0.0003f) { v->amp_env = 0.0f; } break;
                default: break;
            }

            float s = mixed * v->amp_env;

            /* Silence detection to free the voice (only after release). */
            float a = fabsf(s);
            if (v->amp_stage == 2 && a < 0.0008f) { if (++v->silent > 2205) v->active = 0; }
            else v->silent = 0;

            mixL += s * v->pan_l;
            mixR += s * v->pan_r;
        }

        /* Drive (soft saturation on the dry voice mix). */
        if (drive > 0.001f) {
            mixL = tanhf(mixL * drive_g) * drive_cmp;
            mixR = tanhf(mixR * drive_g) * drive_cmp;
        }

        /* Stereo ping-pong delay (always runs to keep the tail continuous). */
        inst->dly_time_smooth += 0.0006f * (dly_target - inst->dly_time_smooth);
        float rp = (float)inst->dly_pos - inst->dly_time_smooth;
        while (rp < 0.0f) rp += DLY_MAX;
        int di0 = (int)rp; float dfr = rp - (float)di0;
        int di1 = di0 + 1; if (di1 >= DLY_MAX) di1 -= DLY_MAX;
        float dwL = inst->dlyL[di0] + dfr * (inst->dlyL[di1] - inst->dlyL[di0]);
        float dwR = inst->dlyR[di0] + dfr * (inst->dlyR[di1] - inst->dlyR[di0]);
        inst->dly_lpL += dly_lpc * (dwL - inst->dly_lpL) + 1e-20f;
        inst->dly_lpR += dly_lpc * (dwR - inst->dly_lpR) + 1e-20f;
        inst->dlyL[inst->dly_pos] = mixL + inst->dly_lpR * dly_fb;   /* cross = ping-pong */
        inst->dlyR[inst->dly_pos] = mixR + inst->dly_lpL * dly_fb;
        inst->dly_pos++; if (inst->dly_pos >= DLY_MAX) inst->dly_pos = 0;
        float sigL = mixL + dwL * dly_mix;
        float sigR = mixR + dwR * dly_mix;

        /* Reverb (mono send, stereo return). */
        if (rev_mix > 0.001f) {
            float send = (sigL + sigR) * 0.5f;
            float wL = reverb_ch(&inst->rvL, send, rev_fb, rev_damp);
            float wR = reverb_ch(&inst->rvR, send, rev_fb, rev_damp);
            sigL += wL * rev_mix;
            sigR += wR * rev_mix;
        }

        /* Stereo width (M/S). */
        if (fabsf(width - 0.5f) > 0.001f) {
            float mid = (sigL + sigR) * 0.5f;
            float sid = (sigL - sigR) * 0.5f * (width * 2.0f);
            sigL = mid + sid; sigR = mid - sid;
        }

        float outL = sigL * level_gain;
        float outR = sigR * level_gain;

        /* Hidden pre-clamp metering for offline calibration. */
        float ap = fabsf(outL) > fabsf(outR) ? fabsf(outL) : fabsf(outR);
        if (ap > inst->meter_peak) inst->meter_peak = ap;
        inst->meter_sumsq += (double)outL * outL + (double)outR * outR;
        inst->meter_cnt += 2;

        outL = soft_limit(outL);
        outR = soft_limit(outR);
        int32_t sl = (int32_t)(clampf(outL, -1.0f, 1.0f) * 32767.0f);
        int32_t sr = (int32_t)(clampf(outR, -1.0f, 1.0f) * 32767.0f);
        out_lr[n * 2]     = (int16_t)sl;
        out_lr[n * 2 + 1] = (int16_t)sr;
    }
}

/* ── API v2 export ───────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

plugin_api_v2_t* move_plugin_init_v2(const void *host) {
    (void)host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = NULL,
        .render_block     = render_block,
    };
    return &api;
}
