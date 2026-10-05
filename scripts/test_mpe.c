/* MPE behaviour tests. Links against fizzik.c (x86).
 * Pitch is asserted via zero-crossing rate (ZCR) over an IDENTICAL timeline in
 * separate runs (note-on at t=0, event at 100 ms, window 200-600 ms) so the
 * model's natural decay cancels in the ratio: a +12 st bend must ~double the
 * ZCR. Covers: member-channel bend (live + seeded before note-on), bend
 * isolation (an unused member channel must not move a note), plain global
 * pitch bend with MPE Off, channel-aware note-off (two bowed voices released
 * one channel at a time), and CC74->Bow sustain. Not shipped. */
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

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void *mk(plugin_api_v2_t *a, int mpe_on) {
    void *i = a->create_instance("/tmp", "");
    a->set_param(i, "preset", "Init");
    a->set_param(i, "mpe", mpe_on ? "On" : "Off");
    a->set_param(i, "mpe_bend", "12");
    a->set_param(i, "mpe_mbend", "12");
    a->set_param(i, "mpe_smooth", "0");
    int16_t b[256]; for (int k = 0; k < 20; k++) a->render_block(i, b, 128);
    return i;
}
static void run_ms(plugin_api_v2_t *a, void *i, int ms) {
    int16_t b[256];
    int blocks = (ms * 44100) / (1000 * 128);
    for (int k = 0; k < blocks; k++) a->render_block(i, b, 128);
}
/* ZCR of the LEFT channel over a window, with hysteresis against noise. */
static double zcr_ms(plugin_api_v2_t *a, void *i, int ms) {
    int16_t b[256];
    int blocks = (ms * 44100) / (1000 * 128);
    long cross = 0; int prev = 0;
    for (int k = 0; k < blocks; k++) {
        a->render_block(i, b, 128);
        for (int s = 0; s < 256; s += 2) {
            int x = b[s];
            if (x > 20 && prev <= 0)       { if (prev < 0) cross++; prev = 1; }
            else if (x < -20 && prev >= 0) { if (prev > 0) cross++; prev = -1; }
        }
    }
    return (double)cross / ((double)blocks * 128.0 / 44100.0);
}
static double rms_ms(plugin_api_v2_t *a, void *i, int ms) {
    char m[64]; double pk, rms;
    a->get_param(i, "__meter", m, 64);          /* reset */
    run_ms(a, i, ms);
    a->get_param(i, "__meter", m, 64);
    sscanf(m, "%lf %lf", &pk, &rms);
    return rms;
}
static void note_on(plugin_api_v2_t *a, void *i, int ch, int note) {
    uint8_t m[3] = { (uint8_t)(0x90 | ch), (uint8_t)note, 100 }; a->on_midi(i, m, 3, 0);
}
static void note_off(plugin_api_v2_t *a, void *i, int ch, int note) {
    uint8_t m[3] = { (uint8_t)(0x80 | ch), (uint8_t)note, 0 }; a->on_midi(i, m, 3, 0);
}
static void bend(plugin_api_v2_t *a, void *i, int ch, int v14) {   /* 0..16383 */
    uint8_t m[3] = { (uint8_t)(0xE0 | ch), (uint8_t)(v14 & 0x7F), (uint8_t)(v14 >> 7) };
    a->on_midi(i, m, 3, 0);
}
static void cc(plugin_api_v2_t *a, void *i, int ch, int num, int val) {
    uint8_t m[3] = { (uint8_t)(0xB0 | ch), (uint8_t)num, (uint8_t)val }; a->on_midi(i, m, 3, 0);
}

/* One timeline: note-on (opt. pre-bend) -> 100 ms -> event -> 20 ms settle ->
 * ZCR over 150 ms. Identical in every run, and EARLY: an octave-up string
 * decays about twice as fast (pitch-normalized feedback), so a late window
 * would measure silence instead of pitch. */
static double timeline_zcr(plugin_api_v2_t *a, int mpe_on, int note_ch,
                           int prebend_ch, int event_bend_ch) {
    void *i = mk(a, mpe_on);
    if (prebend_ch >= 0) bend(a, i, prebend_ch, 16383);
    note_on(a, i, note_ch, 60);
    run_ms(a, i, 100);
    if (event_bend_ch >= 0) bend(a, i, event_bend_ch, 16383);
    run_ms(a, i, 20);
    double z = zcr_ms(a, i, 150);
    a->destroy_instance(i);
    return z;
}

int main(void) {
    plugin_api_v2_t *a = move_plugin_init_v2(0);

    double ref = timeline_zcr(a, 1, 1, -1, -1);
    CHECK(ref > 100.0, "reference ZCR %.0f implausibly low — no signal?", ref);

    double r1 = timeline_zcr(a, 1, 1, -1, 1) / ref;    /* live member bend   */
    printf("live member bend  : ZCR ratio %.2f (want ~2)\n", r1);
    CHECK(r1 > 1.5 && r1 < 2.7, "live bend ratio %.2f outside 1.5-2.7", r1);

    double r2 = timeline_zcr(a, 1, 1, 1, -1) / ref;    /* seeded before note */
    printf("seeded pre-note   : ZCR ratio %.2f (want ~2)\n", r2);
    CHECK(r2 > 1.5 && r2 < 2.7, "seeded bend ratio %.2f outside 1.5-2.7", r2);

    double r3 = timeline_zcr(a, 1, 1, -1, 2) / ref;    /* bend other channel */
    printf("channel isolation : ZCR ratio %.2f (want ~1)\n", r3);
    CHECK(r3 > 0.85 && r3 < 1.15, "unused-channel bend leaked: ratio %.2f", r3);

    double off_ref = timeline_zcr(a, 0, 0, -1, -1);
    double r4 = timeline_zcr(a, 0, 0, -1, 0) / off_ref; /* MPE Off: global    */
    printf("global bend (Off) : ZCR ratio %.2f (want ~2)\n", r4);
    CHECK(r4 > 1.5 && r4 < 2.7, "MPE-Off global bend ratio %.2f outside 1.5-2.7", r4);

    /* MPE On with no CC74 ever sent must sound like MPE Off (regression: an
     * unset channel used to read as timbre 0 = "fully dark" in Bright mode). */
    {
        void *i = mk(a, 1); note_on(a, i, 1, 60); double on_rms = rms_ms(a, i, 300); a->destroy_instance(i);
        i = mk(a, 0);       note_on(a, i, 0, 60); double off_rms = rms_ms(a, i, 300); a->destroy_instance(i);
        printf("neutral timbre    : rms MPE-on %.4f vs off %.4f\n", on_rms, off_rms);
        CHECK(on_rms > off_rms * 0.95 && on_rms < off_rms * 1.05, "MPE-on note differs from MPE-off without any CC74");
    }

    /* Channel-aware note-off: the SAME note on two member channels, bowed
     * into steady sustain (CC74 -> Bow), released one CHANNEL at a time.
     * After the first off the other channel's voice must still sound
     * (a channel-blind synth kills both); after the second, silence (a voice
     * the first off failed to release would be bowed forever). */
    {
        void *i = mk(a, 1);
        a->set_param(i, "mpe_cc74_tgt", "Bow");
        a->set_param(i, "mpe_cc74", "1.0");
        note_on(a, i, 1, 60); cc(a, i, 1, 74, 127);
        note_on(a, i, 2, 60); cc(a, i, 2, 74, 127);
        run_ms(a, i, 1200);
        double both = rms_ms(a, i, 400);
        note_off(a, i, 1, 60); run_ms(a, i, 1500);
        double one = rms_ms(a, i, 400);
        note_off(a, i, 2, 60); run_ms(a, i, 2500);
        double none = rms_ms(a, i, 400);
        a->destroy_instance(i);
        printf("chan note-off     : both %.4f -> one %.4f -> none %.4f\n", both, one, none);
        CHECK(both > 0.01, "bowed voices made no sound");
        CHECK(one > both * 0.2, "releasing one channel's note killed the other channel's voice");
        CHECK(none < one * 0.15, "a voice stayed bowed after both note-offs (missed release)");
    }

    /* CC74 -> Bow sustains the pluck (late tail clearly louder than dry). */
    {
        void *i = mk(a, 1);
        a->set_param(i, "mpe_cc74_tgt", "Bow");
        a->set_param(i, "mpe_cc74", "1.0");
        note_on(a, i, 1, 60); run_ms(a, i, 1500);
        double dry_tail = rms_ms(a, i, 500);
        a->destroy_instance(i);
        i = mk(a, 1);
        a->set_param(i, "mpe_cc74_tgt", "Bow");
        a->set_param(i, "mpe_cc74", "1.0");
        note_on(a, i, 1, 60); cc(a, i, 1, 74, 127); run_ms(a, i, 1500);
        double bow_tail = rms_ms(a, i, 500);
        a->destroy_instance(i);
        printf("cc74 bow          : tail rms %.4f -> %.4f\n", dry_tail, bow_tail);
        CHECK(bow_tail > dry_tail * 1.5 + 0.001, "CC74 bow did not sustain the note");
    }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all MPE tests passed\n");
    return 0;
}
