/* State persistence round-trip test for Fizzik. Links against fizzik.c (x86).
 * Verifies the fix for the "saved track reopens as a factory preset" bug:
 *   1. get_param("state") -> fresh instance -> set_param("state") reproduces
 *      the exact state (including preset name + baked makeup gain).
 *   2. A host echoing set_param("preset", <same>) AFTER the restore (per-key
 *      restore pass, knob-grid sync) must NOT stomp the restored patch.
 *   3. Selecting a DIFFERENT preset by key applies immediately (no deferred
 *      apply left pending to fire later).
 *   4. Garbage / empty / truncated state must not crash and keeps defaults.
 * Not shipped. */
#include <stdio.h>
#include <stdint.h>
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

static void render_ms(plugin_api_v2_t *a, void *i, int blocks) {
    int16_t buf[256];
    for (int b = 0; b < blocks; b++) a->render_block(i, buf, 128);
}

int main(void) {
    plugin_api_v2_t *a = move_plugin_init_v2(0);
    char s1[8192], s2[8192], v[128];

    /* ── 1. Round-trip: preset + tweaks + globals survive exactly ─────────── */
    void *A = a->create_instance("/tmp", "");
    a->set_param(A, "preset", "BrokenMusicBox");   /* makeup 10.0 — the worst case for lost makeup */
    render_ms(a, A, 20);
    a->set_param(A, "a_decay", "0.9100");          /* user tweaks on top of the preset */
    a->set_param(A, "exc_color", "0.1200");
    a->set_param(A, "b_model", "Membrane");
    a->set_param(A, "b_tune", "-7");
    a->set_param(A, "cutoff", "0.3300");           /* global layer */
    a->set_param(A, "voicing", "MS-20");
    a->set_param(A, "lfo1_target", "Couple");
    a->set_param(A, "at_preset", "Cello");
    a->set_param(A, "at_bow", "0.7700");           /* tweak AFTER the AT preset */
    int n1 = a->get_param(A, "state", s1, sizeof(s1));
    CHECK(n1 > 0, "get_param(state) returned %d", n1);
    CHECK(strstr(s1, "preset=BrokenMusicBox\n") == s1, "state must start with the preset name, got: %.40s", s1);
    CHECK(strstr(s1, "__makeup=") != NULL, "state must carry the baked makeup gain");
    CHECK(strstr(s1, "a_decay=0.91000") != NULL, "tweaked a_decay missing from state");

    void *B = a->create_instance("/tmp", "");
    a->set_param(B, "state", s1);
    render_ms(a, B, 20);                            /* let any (wrongly) deferred apply fire */
    int n2 = a->get_param(B, "state", s2, sizeof(s2));
    CHECK(n2 == n1 && strcmp(s1, s2) == 0, "state does not round-trip:\n--- saved ---\n%s--- restored ---\n%s", s1, s2);
    a->get_param(B, "preset", v, sizeof(v));
    CHECK(strcmp(v, "BrokenMusicBox") == 0, "restored preset name is '%s'", v);

    /* ── 2. Host echoes the preset key after restore: must be a no-op ─────── */
    a->set_param(B, "preset", "BrokenMusicBox");
    render_ms(a, B, 20);
    a->get_param(B, "state", s2, sizeof(s2));
    CHECK(strcmp(s1, s2) == 0, "re-selecting the current preset by key stomped the restored patch");

    /* ── 3. A different preset by key applies immediately, nothing pending ── */
    a->set_param(B, "preset", "Sharp");
    a->get_param(B, "a_decay", v, sizeof(v));       /* before any render */
    CHECK(strstr(v, "0.9100") == NULL, "preset apply is still deferred (a_decay unchanged before render)");
    a->get_param(B, "cutoff", v, sizeof(v));
    CHECK(strstr(v, "0.3300") == v, "preset apply must preserve the GLOBAL layer (cutoff), got %s", v);

    /* ── 4. Garbage state: no crash, defaults kept ────────────────────────── */
    void *C = a->create_instance("/tmp", "");
    a->set_param(C, "state", "");
    a->set_param(C, "state", "garbage\n====\npreset=\nnope");
    a->set_param(C, "state", "{\"json\": \"object\"}");
    render_ms(a, C, 20);
    a->get_param(C, "preset", v, sizeof(v));
    CHECK(strcmp(v, "CaveStrings") == 0, "garbage state changed the default preset to '%s'", v);

    /* ── 5. Tiny buffer: refuse rather than truncate ──────────────────────── */
    CHECK(a->get_param(A, "state", v, 64) == -1, "small-buffer state read must return -1");

    a->destroy_instance(A); a->destroy_instance(B); a->destroy_instance(C);
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all state round-trip tests passed\n");
    return 0;
}
