/* Host-Test: echter ALSA-Ablauf über das "null"-Gerät, mit LD_PRELOAD
 * der Addin-Bibliothek. Prüft, dass die Hooks durchreichen und loggen.
 * Ohne ALSA-Header: Prototypen von Hand, gelinkt gegen libasound.so.2. */
#include <stdio.h>

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
int snd_pcm_open(snd_pcm_t **, const char *, int, int);
int snd_pcm_close(snd_pcm_t *);
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **);
void snd_pcm_hw_params_free(snd_pcm_hw_params_t *);
int snd_pcm_hw_params_any(snd_pcm_t *, snd_pcm_hw_params_t *);
int snd_pcm_hw_params_set_access(snd_pcm_t *, snd_pcm_hw_params_t *, int);
int snd_pcm_hw_params_set_format(snd_pcm_t *, snd_pcm_hw_params_t *, int);
int snd_pcm_hw_params_set_channels(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
int snd_pcm_hw_params_set_rate(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int);
int snd_pcm_hw_params(snd_pcm_t *, snd_pcm_hw_params_t *);
long snd_pcm_writei(snd_pcm_t *, const void *, unsigned long);
#include <stdlib.h>
#include <string.h>

int main(void)
{
    snd_pcm_t *pcm;
    snd_pcm_hw_params_t *hw;
    int r = snd_pcm_open(&pcm, "null", 0, 0);
    if (r < 0) { printf("open null: %d\n", r); return 1; }
    snd_pcm_hw_params_malloc(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, 3);   /* RW_INTERLEAVED */
    snd_pcm_hw_params_set_format(pcm, hw, 2);   /* S16_LE */
    unsigned ch = getenv("TEST_CH") ? (unsigned)atoi(getenv("TEST_CH")) : 2;
    snd_pcm_hw_params_set_channels(pcm, hw, ch);
    r = snd_pcm_hw_params_set_rate(pcm, hw, 44100, 0);
    printf("set_rate 44100 -> %d\n", r);
    r = snd_pcm_hw_params(pcm, hw);
    printf("hw_params -> %d\n", r);
    if (r == 0) {
        static short buf[256 * 32];
        memset(buf, 0, sizeof buf);
        long w = snd_pcm_writei(pcm, buf, 256);
        printf("writei 256 -> %ld\n", w);
    }
    snd_pcm_hw_params_free(hw);
    snd_pcm_close(pcm);
    return 0;
}
