/* Dump any get_param key from a fresh instance (default: ui_hierarchy).
 * Links against fizzik.c (x86, docker fizzik-native). Used to validate the
 * DSP-served ui_hierarchy/chain_params JSON offline. Not shipped.
 *   gcc -O2 -ffast-math -o /tmp/du scripts/dump_param.c src/dsp/fizzik.c -lm
 *   /tmp/du chain_params | python -m json.tool */
#include <stdio.h>
#include <stdint.h>
typedef struct { uint32_t v; void*(*ci)(const char*,const char*); void(*di)(void*); void(*om)(void*,const uint8_t*,int,int); void(*sp)(void*,const char*,const char*); int(*gp)(void*,const char*,char*,int); int(*ge)(void*,char*,int); void(*rb)(void*,int16_t*,int);} api_t;
extern api_t* move_plugin_init_v2(const void*);
int main(int argc, char**argv){ api_t*a=move_plugin_init_v2(0); void*i=a->ci("/tmp",""); static char b[131072]; int n=a->gp(i,argc>1?argv[1]:"ui_hierarchy",b,sizeof(b)); fwrite(b,1,n<0?0:n,stdout); return n<=0; }
