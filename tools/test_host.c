/* Host-Test: prüft Umleitungslogik und dass die Hooks greifen.
 * Läuft auf dem Build-Rechner (x86_64), nicht auf der Force. */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_host <libsrate.so>\n"); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*rw)(const char *, char *, size_t) = dlsym(lib, "srate_rewrite");
    if (!rw) { fprintf(stderr, "srate_rewrite fehlt\n"); return 1; }

    struct { const char *in; const char *want; } t[] = {
        { "hw:2",                 "plughw:2" },
        { "hw:2,0",               "plughw:2,0" },
        { "hw:CARD=SH4d,DEV=0",   "plughw:CARD=SH4d,DEV=0" },
        { "hw:SH4d",              "plughw:SH4d" },
        { "hw:1,0",               NULL },          /* andere Karte */
        { "hw:Force",             NULL },
        { "plughw:2,0",           NULL },          /* schon plug */
        { "default",              NULL },
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        char out[256] = "";
        int r = rw(t[i].in, out, sizeof out);
        int ok = t[i].want ? (r && !strcmp(out, t[i].want)) : !r;
        printf("%-24s -> %-26s %s\n", t[i].in, r ? out : "(unverändert)", ok ? "OK" : "FEHLER");
        if (!ok) fail = 1;
    }
    return fail;
}
