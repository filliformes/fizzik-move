/* Offline level meter for Fizzik presets. Links against fizzik.c (x86) and
 * drives the public API to measure peak/RMS per preset. Not shipped. */
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

static const char *NAMES[16] = {
    "AlienChurch","BowedGlass","CaveStrings","CouncilsPiano","DistortedBass",
    "FeedbackHarp","JudgementAwaits","OldResonances","PreparedPiano","RythmicBow",
    "SensitiveSkin","Sharp","ShockingPluck","Slappy","SurroundedByBells","XyloStyle"
};

int main(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(0);
    printf("%-18s  %8s  %8s\n", "preset", "peak", "rms");
    for (int pi = 0; pi < 16; pi++) {
        void *inst = api->create_instance("/tmp", "");
        api->set_param(inst, "preset", NAMES[pi]);
        uint8_t on[3]  = {0x90, 60, 100};
        uint8_t off[3] = {0x80, 60, 0};
        api->on_midi(inst, on, 3, 0);
        int16_t buf[256];
        int total = 690;            /* ~2 s of 128-frame blocks */
        char mb[64]; api->get_param(inst, "__meter", mb, sizeof(mb)); /* reset */
        for (int b = 0; b < total; b++) {
            if (b == 345) api->on_midi(inst, off, 3, 0);   /* release at ~1 s */
            api->render_block(inst, buf, 128);
        }
        double peak = 0, rms = 0;
        api->get_param(inst, "__meter", mb, sizeof(mb));    /* pre-clamp peak/rms */
        sscanf(mb, "%lf %lf", &peak, &rms);
        float makeup = 0.8 / (peak + 1e-9);
        if (makeup < 0.15f) makeup = 0.15f;
        if (makeup > 6.0f)  makeup = 6.0f;
        printf("%-18s  peak=%8.3f  rms=%8.4f  makeup=%.3ff\n", NAMES[pi], peak, rms, makeup);
        api->destroy_instance(inst);
    }
    return 0;
}
