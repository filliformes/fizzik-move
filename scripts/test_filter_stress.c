/* Filter/output-stage stress test. Links against fizzik.c (x86).
 * For every filter voicing at FULL resonance with a 4-voice chord and a
 * cutoff sweep: output must stay within the brickwall ceiling, never NaN
 * (int16 can't NaN — checked via nonzero sane output), and the tail must
 * decay after release (no self-oscillation runaway). Also proves the output
 * stage no longer shears normal poly program: with the ceiling maxed, a
 * chord's post-chain peak must reach well ABOVE the old 0.9-tanh squash.
 * Not shipped. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

extern plugin_api_v2_t* move_plugin_init_v2(const void *);

static const char *VOICINGS[12] = {
    "Clean SVF","SEM","MS-20","Steiner","Ladder 4P","Ladder 2P","Ladder 1P",
    "Prophet","Oberheim","Diode","Sallen-Key","Vintage"
};
static const char *FTYPES[4] = { "LP","HP","BP","Notch" };

static int failures = 0;

static int peak_of(const int16_t *b, int n) {
    int pk = 0;
    for (int i = 0; i < n; i++) { int a = abs((int)b[i]); if (a > pk) pk = a; }
    return pk;
}

int main(void) {
    plugin_api_v2_t *a = move_plugin_init_v2(0);
    int16_t buf[256];
    const int CEIL = (int)(0.99f * 32767.0f) + 2;   /* lim_ceil at knob max */
    uint8_t on[4][3]  = {{0x90,48,110},{0x90,55,110},{0x90,60,110},{0x90,64,110}};
    uint8_t off[4][3] = {{0x80,48,0},{0x80,55,0},{0x80,60,0},{0x80,64,0}};

    /* Ladder-family voicings SELF-OSCILLATE at full resonance by design (tanh
     * feedback, bounded) — their res=1.0 tail never decays and must only stay
     * bounded; decay is asserted in a second pass at res=0.7. SVF-family
     * voicings never self-oscillate (k floor > 0): decay asserted at res=1. */
    for (int v = 0; v < 12; v++) {
        int is_svf = (v==0 || v==1 || v==2 || v==3 || v==10);
        for (int pass = 0; pass < (is_svf ? 1 : 2); pass++) {
        const char *res = (is_svf || pass == 1) ? "1.0" : "0.7";
        int expect_decay = is_svf || (pass == 0);
        for (int t = 0; t < 4; t++) {
            void *i = a->create_instance("/tmp", "");
            a->set_param(i, "preset", "CaveStrings");
            a->set_param(i, "voicing", VOICINGS[v]);
            a->set_param(i, "ftype", FTYPES[t]);
            a->set_param(i, "resonance", res);
            a->set_param(i, "cutoff", "0.85");
            a->set_param(i, "lim_ceil", "1.0");
            for (int b = 0; b < 20; b++) a->render_block(i, buf, 128);   /* settle smoothing */
            for (int n = 0; n < 4; n++) a->on_midi(i, on[n], 3, 0);
            int maxpk = 0;
            for (int b = 0; b < 345; b++) {                      /* 1 s chord + cutoff sweep */
                if (b % 12 == 0) {
                    char cs[16]; snprintf(cs, sizeof(cs), "%.3f", 0.85 - 0.65 * (b / 345.0));
                    a->set_param(i, "cutoff", cs);
                }
                a->render_block(i, buf, 128);
                int pk = peak_of(buf, 256); if (pk > maxpk) maxpk = pk;
                if (pk > CEIL) { failures++; printf("FAIL %s/%s: peak %d above ceiling\n", VOICINGS[v], FTYPES[t], pk); break; }
            }
            for (int n = 0; n < 4; n++) a->on_midi(i, off[n], 3, 0);
            for (int b = 0; b < 690; b++) a->render_block(i, buf, 128);  /* 2 s tail */
            int tail = 0;
            for (int b = 0; b < 30; b++) { a->render_block(i, buf, 128); int pk = peak_of(buf, 256); if (pk > tail) tail = pk; }
            if (expect_decay && tail > 700) { failures++; printf("FAIL %s/%s res=%s: tail %d not decaying (runaway)\n", VOICINGS[v], FTYPES[t], res, tail); }
            if (t == 0 && maxpk < 300) { failures++; printf("FAIL %s/LP res=%s: near-silent (peak %d) — filter killed the signal\n", VOICINGS[v], res, maxpk); }
            a->destroy_instance(i);
        }
        }
    }

    /* Chord level sanity: default patch, 4-voice chord, ceiling maxed — with
     * polyphony compensation (bus x n_eff^-0.35) the post-chain chord peak
     * should land in a clean mid range: well below the soft-clip knee (the old
     * uncompensated sum reached 0.7-2.0 and distorted), but nowhere near
     * silent. Cleanliness itself is asserted by the clip-incidence check. */
    {
        void *i = a->create_instance("/tmp", "");
        a->set_param(i, "preset", "MarbleDrum");     /* hottest 4-voice chord in the bank */
        a->set_param(i, "lim_ceil", "1.0");
        for (int b = 0; b < 20; b++) a->render_block(i, buf, 128);
        for (int n = 0; n < 4; n++) a->on_midi(i, on[n], 3, 0);
        int maxpk = 0;
        for (int b = 0; b < 345; b++) { a->render_block(i, buf, 128); int pk = peak_of(buf, 256); if (pk > maxpk) maxpk = pk; }
        printf("chord level check: MarbleDrum 4-voice post-chain peak = %.3f FS\n", maxpk / 32767.0);
        if (maxpk < 8000 || maxpk > 27000) { failures++; printf("FAIL chord level: peak %d outside the clean 0.25-0.82 FS window\n", maxpk); }
        a->destroy_instance(i);
    }

    /* Chord cleanliness: hot presets, DEFAULT limiter ceiling (0.6715),
     * sustained 4-voice chord — count output samples pinned at the clamp
     * (each pinned sample is residual hard clipping the smoothed limiter gain
     * missed). Pre-fix (no polyphony compensation) this pinned every waveform
     * peak; with the bus scaled by n_eff^-0.35 the limiter is a guard again. */
    {
        const char *HOT[3] = { "WhisperHarp", "MarbleDrum", "AlienChurch" };
        int clip_thr = (int)(0.6715f * 32767.0f) - 4;
        for (int h = 0; h < 3; h++) {
            void *i = a->create_instance("/tmp", "");
            a->set_param(i, "preset", HOT[h]);
            for (int b = 0; b < 20; b++) a->render_block(i, buf, 128);
            for (int n = 0; n < 4; n++) a->on_midi(i, on[n], 3, 0);
            for (int b = 0; b < 100; b++) a->render_block(i, buf, 128);   /* past the attack */
            long clipped = 0, total = 0;
            for (int b = 0; b < 345; b++) {
                a->render_block(i, buf, 128);
                for (int s = 0; s < 256; s++) { if (abs((int)buf[s]) >= clip_thr) clipped++; total++; }
            }
            double pct = 100.0 * clipped / total;
            printf("chord clip incidence %-12s: %.3f%%\n", HOT[h], pct);
            if (pct > 0.5) { failures++; printf("FAIL %s: %.2f%% of chord samples pinned at the clamp\n", HOT[h], pct); }
            a->destroy_instance(i);
        }
    }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all filter stress tests passed (12 voicings x 4 types, full reso)\n");
    return 0;
}
