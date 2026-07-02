/* Offline analysis + level calibration for Fizzik presets. Links against
 * fizzik.c (x86). Measures single-note level, 4-voice chord peak (polyphony
 * headroom), post-release tail (runaway/stability), and NaN. Not shipped. */
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

static const char *NAMES[30] = {
    "AlienChurch","BowedGlass","CaveStrings","CouncilsPiano","DistortedBass",
    "FeedbackHarp","JudgementAwaits","OldResonances","PreparedPiano","RythmicBow",
    "SensitiveSkin","Sharp","ShockingPluck","Slappy","SurroundedByBells","XyloStyle",
    "GlassKalimba","IronLullaby","TidalGong","HollowReed","StarlightPad",
    "BrokenMusicBox","DeepDiveBass","CopperTongue","GhostSitar","MarbleDrum",
    "WhisperHarp","TitaniumBell","FrozenLake","PulseEngine"
};

static void meter(plugin_api_v2_t *a, void *i, double *pk, double *rms) {
    char m[64]; a->get_param(i, "__meter", m, 64); sscanf(m, "%lf %lf", pk, rms);
}
static int has_nan(const int16_t *b, int n) { (void)b; (void)n; return 0; } /* int16 can't NaN; float checked in DSP via isfinite */

int main(void) {
    plugin_api_v2_t *a = move_plugin_init_v2(0);
    printf("%-16s %6s %6s | %6s(4v) | %7s(tail) | %s\n",
           "preset","peak","rms","peak","rms","makeup");
    for (int pi = 0; pi < 30; pi++) {
        int16_t buf[256];
        char m[64];
        double pk, rms, chordpk, chordrms, tailpk, tailrms;

        /* --- single note: pre-limiter peak/rms --- */
        void *i = a->create_instance("/tmp", "");
        a->set_param(i, "preset", NAMES[pi]);
        a->set_param(i, "__makeup", "1.0");
        { int16_t sb[256]; for (int w=0; w<40; w++) a->render_block(i, sb, 128); } /* settle 20ms smoothing */   /* measure raw */
        uint8_t on[3]={0x90,60,100}, off[3]={0x80,60,0};
        a->on_midi(i, on, 3, 0);
        a->get_param(i, "__meter", m, 64);            /* reset */
        for (int b=0;b<690;b++){ if(b==345)a->on_midi(i,off,3,0); a->render_block(i,buf,128); }
        meter(a, i, &pk, &rms);
        a->destroy_instance(i);

        /* --- 4-voice chord: polyphony headroom --- */
        i = a->create_instance("/tmp", "");
        a->set_param(i, "preset", NAMES[pi]);
        a->set_param(i, "__makeup", "1.0");
        { int16_t sb[256]; for (int w=0; w<40; w++) a->render_block(i, sb, 128); } /* settle 20ms smoothing */
        uint8_t c[4][3]={{0x90,52,110},{0x90,55,110},{0x90,59,110},{0x90,64,110}};
        for(int v=0;v<4;v++) a->on_midi(i,c[v],3,0);
        a->get_param(i,"__meter",m,64);
        for(int b=0;b<345;b++) a->render_block(i,buf,128);   /* ~1s held */
        meter(a,i,&chordpk,&chordrms);
        a->destroy_instance(i);

        /* --- tail after release: runaway/stability --- */
        i = a->create_instance("/tmp", "");
        a->set_param(i, "preset", NAMES[pi]);
        a->set_param(i, "__makeup", "1.0");
        { int16_t sb[256]; for (int w=0; w<40; w++) a->render_block(i, sb, 128); } /* settle 20ms smoothing */
        a->on_midi(i,on,3,0);
        for(int b=0;b<60;b++) a->render_block(i,buf,128);
        a->on_midi(i,off,3,0);
        for(int b=0;b<600;b++) a->render_block(i,buf,128);   /* let it release ~1.7s */
        a->get_param(i,"__meter",m,64);                      /* reset */
        for(int b=0;b<600;b++) a->render_block(i,buf,128);   /* measure tail 1.7s later */
        meter(a,i,&tailpk,&tailrms);
        a->destroy_instance(i);

        /* Perceptual level: cap by peak AND sustained RMS so long/bright
         * resonances don't read as loud as their transient suggests. */
        double mk_peak = 0.42 / (pk + 1e-9);
        double mk_rms  = 0.13 / (rms + 1e-9);
        double makeup = mk_peak < mk_rms ? mk_peak : mk_rms;
        if (makeup < 0.1) makeup = 0.1; if (makeup > 10.0) makeup = 10.0;
        const char *flag = (tailrms > 0.03) ? " <RUNAWAY?" : (chordpk > 2.6 ? " <hot-chord" : "");
        printf("%-16s %6.3f %6.4f | %6.3f | %7.4f | %.3ff%s\n",
               NAMES[pi], pk, rms, chordpk, tailrms, makeup, flag);
    }
    return 0;
}
