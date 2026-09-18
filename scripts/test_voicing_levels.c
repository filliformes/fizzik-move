/* Per-voicing loudness match. Links against fizzik.c (x86).
 * Plays the same sustained 4-voice chord through every filter voicing (LP,
 * res 0, cutoff open — the "just switched voicing" case) and reports RMS +
 * the trim factor needed to match Clean SVF. The input tanh drive of the
 * driven voicings compresses loud poly program (2-11 dB), which the old
 * 0.9-tanh output squash masked; bake the suggested trims into
 * filter_process so switching voicings doesn't read as a volume drop.
 * Second column: same at cutoff 0.45 / res 0.35 (musical setting, info only —
 * a 4-pole LP passing less HF than a 2-pole is correct, not a trim error).
 * Not shipped. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
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

static double measure(plugin_api_v2_t *a, const char *voicing, const char *cut, const char *res) {
    int16_t buf[256]; char m[64]; double pk, rms;
    void *i = a->create_instance("/tmp", "");
    a->set_param(i, "preset", "CaveStrings");
    a->set_param(i, "voicing", voicing);
    a->set_param(i, "ftype", "LP");
    a->set_param(i, "cutoff", cut);
    a->set_param(i, "resonance", res);
    for (int b = 0; b < 40; b++) a->render_block(i, buf, 128);   /* settle smoothing */
    uint8_t on[4][3] = {{0x90,48,110},{0x90,55,110},{0x90,60,110},{0x90,64,110}};
    for (int n = 0; n < 4; n++) a->on_midi(i, on[n], 3, 0);
    for (int b = 0; b < 170; b++) a->render_block(i, buf, 128);  /* skip attack */
    a->get_param(i, "__meter", m, 64);                            /* reset meter */
    for (int b = 0; b < 345; b++) a->render_block(i, buf, 128);  /* 1 s sustain */
    a->get_param(i, "__meter", m, 64);
    sscanf(m, "%lf %lf", &pk, &rms);
    a->destroy_instance(i);
    return rms;
}

int main(void) {
    plugin_api_v2_t *a = move_plugin_init_v2(0);
    double open_rms[12], mid_rms[12];
    for (int v = 0; v < 12; v++) {
        open_rms[v] = measure(a, VOICINGS[v], "1.0", "0.0");
        mid_rms[v]  = measure(a, VOICINGS[v], "0.45", "0.35");
    }
    printf("%-11s %9s %9s | %9s %9s | %s\n", "voicing", "openRMS", "dB", "midRMS", "dB", "trim->Clean(open)");
    for (int v = 0; v < 12; v++) {
        double dbo = 20.0 * log10(open_rms[v] / open_rms[0] + 1e-12);
        double dbm = 20.0 * log10(mid_rms[v]  / mid_rms[0]  + 1e-12);
        printf("%-11s %9.4f %+8.1f | %9.4f %+8.1f | x%.3f\n",
               VOICINGS[v], open_rms[v], dbo, mid_rms[v], dbm, open_rms[0] / (open_rms[v] + 1e-12));
    }
    return 0;
}
