/*
 * mpc-addin-srate – LD_PRELOAD-Addin für Akai Force / MPC (MPC OS 3.x)
 *
 * Zweck: USB-Audio-Geräte, die nur 48 kHz können (z. B. Roland SH-4d),
 * an der Force nutzbar machen, die fest mit 44,1 kHz arbeitet.
 *
 * Arbeitsweise:
 *   - snd_pcm_open() wird abgefangen. Gehört der Gerätename ("hw:2,0",
 *     "hw:CARD=SH4d,DEV=0", ...) zu einer Karte, deren Name/ID zu "match"
 *     passt, wird im Modus "plug" auf "plughw:..." umgeleitet. Dann rechnet
 *     ALSA selbst um (Abtastrate, Format, Kanalzahl).
 *   - Alle Rate-/Kanal-/Format-Aufrufe und deren Fehler werden mitgeloggt,
 *     ebenso Xruns (snd_pcm_recover). So sieht man, wo "Fehler 22" entsteht.
 *
 * Keine ALSA-Header nötig: Typen sind opak, Original-Funktionen kommen
 * per dlsym(RTLD_NEXT). Das hält den Cross-Build einfach.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define SRATE_VERSION "0.2.1"

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;

/* ---------- Konfiguration ---------- */

enum { MODE_LOG = 0, MODE_PLUG = 1 };

static struct {
    int  mode;                 /* log | plug */
    char match[64];            /* Teilstring von Karten-ID/-Name */
    char logpath[512];
    char dir[512];             /* Ordner der .so */
    int  verbose;              /* 1 = auch Aufrufe ohne Treffer loggen */
} cfg = { MODE_PLUG, "SH4D", "", "", 0 };

static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  init_once = PTHREAD_ONCE_INIT;
static FILE *logf_ = NULL;
static int disabled = 0;

static void slog(const char *fmt, ...)
{
    if (!logf_) return;
    pthread_mutex_lock(&log_mx);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(logf_, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf_, fmt, ap);
    va_end(ap);
    fputc('\n', logf_);
    fflush(logf_);
    pthread_mutex_unlock(&log_mx);
}

/* Nur Buchstaben/Ziffern, klein: "SH-4d" == "sh4d" */
static void normalize(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (; *in && j + 1 < n; in++)
        if (isalnum((unsigned char)*in)) out[j++] = (char)tolower((unsigned char)*in);
    out[j] = 0;
}

static int text_matches(const char *text)
{
    char a[256], b[64];
    normalize(text, a, sizeof a);
    normalize(cfg.match, b, sizeof b);
    return b[0] && strstr(a, b) != NULL;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (*b && isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void load_conf(void)
{
    char path[640];
    snprintf(path, sizeof path, "%s/srate.conf", cfg.dir);
    const char *env = getenv("SRATE_CONF");
    FILE *f = fopen(env ? env : path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if (!strcasecmp(k, "mode"))
            cfg.mode = !strcasecmp(v, "plug") ? MODE_PLUG : MODE_LOG;
        else if (!strcasecmp(k, "match"))
            snprintf(cfg.match, sizeof cfg.match, "%s", v);
        else if (!strcasecmp(k, "log"))
            snprintf(cfg.logpath, sizeof cfg.logpath, "%s", v);
        else if (!strcasecmp(k, "verbose"))
            cfg.verbose = atoi(v);
    }
    fclose(f);
}

static const char *proc_cards_path(void)
{
    const char *p = getenv("SRATE_PROC_CARDS");   /* nur für Tests */
    return p ? p : "/proc/asound/cards";
}

/*
 * /proc/asound/cards:
 *  2 [SH4d           ]: USB-Audio - SH-4d
 *                       Roland SH-4d at usb-xhci-hcd.0.auto-1.2, high speed
 * Liefert Kartennummer oder -1. id_out bekommt die Karten-ID.
 */
static int find_card(char *id_out, size_t idn)
{
    FILE *f = fopen(proc_cards_path(), "r");
    if (!f) return -1;
    char line[512], block[1024] = "";
    int cur = -1, found = -1;
    char curid[64] = "";
    while (fgets(line, sizeof line, f)) {
        int num; char id[64];
        if (sscanf(line, " %d [%63[^]]]", &num, id) == 2) {
            if (cur >= 0 && text_matches(block)) { found = cur; break; }
            cur = num;
            trim(id);
            snprintf(curid, sizeof curid, "%s", id);
            snprintf(block, sizeof block, "%s", line);
        } else {
            strncat(block, line, sizeof block - strlen(block) - 1);
        }
    }
    if (found < 0 && cur >= 0 && text_matches(block)) found = cur;
    fclose(f);
    if (found >= 0 && id_out) snprintf(id_out, idn, "%s", curid);
    return found;
}

/* Gehört ein ALSA-Gerätename zur gesuchten Karte? */
static int name_is_target(const char *name, int card, const char *cardid)
{
    const char *p;
    if (!strncmp(name, "hw:", 3)) p = name + 3;
    else return 0;                       /* plughw:, default, ... nicht anfassen */

    if (!strncasecmp(p, "CARD=", 5)) p += 5;
    char tok[64];
    size_t i = 0;
    while (p[i] && p[i] != ',' && i + 1 < sizeof tok) { tok[i] = p[i]; i++; }
    tok[i] = 0;
    if (!tok[0]) return 0;

    char *end;
    long n = strtol(tok, &end, 10);
    if (*end == 0) return n == card;
    return !strcasecmp(tok, cardid);
}

/* Für Tests auch von außen aufrufbar */
int srate_rewrite(const char *name, char *out, size_t outn)
{
    char id[64] = "";
    int card = find_card(id, sizeof id);
    if (card < 0 || !name_is_target(name, card, id)) return 0;
    snprintf(out, outn, "plug%s", name);
    return 1;
}

static void dump_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { slog("  (%s nicht lesbar)", path); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        slog("  | %s", line);
    }
    fclose(f);
}

static void dump_card_info(void)
{
    char id[64] = "";
    slog("--- /proc/asound/cards ---");
    dump_file(proc_cards_path());
    int card = find_card(id, sizeof id);
    if (card < 0) { slog("Zielkarte '%s' derzeit nicht gefunden", cfg.match); return; }
    slog("Zielkarte '%s' = Karte %d (ID %s)", cfg.match, card, id);
    char path[128];
    for (int s = 0; s < 4; s++) {
        snprintf(path, sizeof path, "/proc/asound/card%d/stream%d", card, s);
        if (access(path, R_OK) == 0) { slog("--- %s ---", path); dump_file(path); }
    }
}

static void do_init(void)
{
    Dl_info di;
    if (dladdr((void *)&do_init, &di) && di.dli_fname) {
        snprintf(cfg.dir, sizeof cfg.dir, "%s", di.dli_fname);
        char *sl = strrchr(cfg.dir, '/');
        if (sl) *sl = 0; else strcpy(cfg.dir, ".");
    } else strcpy(cfg.dir, ".");

    char off[640];
    snprintf(off, sizeof off, "%s/srate.off", cfg.dir);
    if (access(off, F_OK) == 0) { disabled = 1; return; }

    load_conf();
    if (!cfg.logpath[0]) snprintf(cfg.logpath, sizeof cfg.logpath, "%s/srate.log", cfg.dir);
    logf_ = fopen(cfg.logpath, "a");
    if (!logf_) logf_ = fopen("/tmp/srate.log", "a");

    char exe[256] = "?";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) exe[n] = 0;
    slog("=== mpc-addin-srate %s geladen in %s (pid %d), mode=%s, match=%s ===",
         SRATE_VERSION, exe, (int)getpid(),
         cfg.mode == MODE_PLUG ? "plug" : "log", cfg.match);
    dump_card_info();
}

static void init(void) { pthread_once(&init_once, do_init); }

/* ---------- Original-Funktionen ---------- */

#define REAL(name) \
    static __typeof__(&name) real_##name; \
    if (!real_##name) real_##name = (__typeof__(&name))dlsym(RTLD_NEXT, #name)

const char *snd_strerror(int err);
const char *snd_pcm_name(snd_pcm_t *pcm);

static const char *errstr(int e)
{
    static const char *(*f)(int);
    if (!f) f = (const char *(*)(int))dlsym(RTLD_NEXT, "snd_strerror");
    return f ? f(e) : "?";
}

static const char *pname(snd_pcm_t *pcm)
{
    static const char *(*f)(snd_pcm_t *);
    if (!f) f = (const char *(*)(snd_pcm_t *))dlsym(RTLD_NEXT, "snd_pcm_name");
    const char *n = (f && pcm) ? f(pcm) : NULL;
    return n ? n : "?";
}

/* Nur PCMs der Zielkarte (oder alle bei verbose) loggen */
#define MAXTRACK 32
static snd_pcm_t *tracked[MAXTRACK];
static pthread_mutex_t trk_mx = PTHREAD_MUTEX_INITIALIZER;

static void track(snd_pcm_t *p)
{
    pthread_mutex_lock(&trk_mx);
    for (int i = 0; i < MAXTRACK; i++) if (!tracked[i]) { tracked[i] = p; break; }
    pthread_mutex_unlock(&trk_mx);
}
static void untrack(snd_pcm_t *p)
{
    pthread_mutex_lock(&trk_mx);
    for (int i = 0; i < MAXTRACK; i++) if (tracked[i] == p) tracked[i] = NULL;
    pthread_mutex_unlock(&trk_mx);
}
static int is_tracked(snd_pcm_t *p)
{
    if (cfg.verbose) return 1;
    int r = 0;
    pthread_mutex_lock(&trk_mx);
    for (int i = 0; i < MAXTRACK; i++) if (tracked[i] == p) { r = 1; break; }
    pthread_mutex_unlock(&trk_mx);
    return r;
}

/* ---------- Hooks ---------- */

int snd_pcm_open(snd_pcm_t **pcmp, const char *name, int stream, int mode)
{
    REAL(snd_pcm_open);
    static __thread int nested;
    init();
    if (disabled || !name || nested) return real_snd_pcm_open(pcmp, name, stream, mode);

    char newname[256];
    int target = srate_rewrite(name, newname, sizeof newname);
    const char *use = (target && cfg.mode == MODE_PLUG) ? newname : name;
    nested = 1;
    int r = real_snd_pcm_open(pcmp, use, stream, mode);
    nested = 0;

    if (target || cfg.verbose)
        slog("open '%s'%s%s %s mode=0x%x -> %d %s", name,
             use != name ? " => " : "", use != name ? use : "",
             stream ? "CAPTURE" : "PLAYBACK", mode, r, r < 0 ? errstr(r) : "ok");
    if (r == 0 && target) track(*pcmp);
    return r;
}

int snd_pcm_close(snd_pcm_t *pcm)
{
    REAL(snd_pcm_close);
    if (!disabled && is_tracked(pcm)) { slog("close %s", pname(pcm)); untrack(pcm); }
    return real_snd_pcm_close(pcm);
}

int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int val, int dir)
{
    REAL(snd_pcm_hw_params_set_rate);
    int r = real_snd_pcm_hw_params_set_rate(pcm, p, val, dir);
    if (!disabled && is_tracked(pcm))
        slog("set_rate %s %u -> %d %s", pname(pcm), val, r, r < 0 ? errstr(r) : "ok");
    return r;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *val, int *dir)
{
    REAL(snd_pcm_hw_params_set_rate_near);
    unsigned int want = val ? *val : 0;
    int r = real_snd_pcm_hw_params_set_rate_near(pcm, p, val, dir);
    if (!disabled && is_tracked(pcm))
        slog("set_rate_near %s %u -> %d %s (bekommt %u)", pname(pcm), want, r,
             r < 0 ? errstr(r) : "ok", val ? *val : 0);
    return r;
}

int snd_pcm_hw_params_set_rate_resample(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int val)
{
    REAL(snd_pcm_hw_params_set_rate_resample);
    int r = real_snd_pcm_hw_params_set_rate_resample(pcm, p, val);
    if (!disabled && is_tracked(pcm))
        slog("set_rate_resample %s %u -> %d", pname(pcm), val, r);
    return r;
}

int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int val, int dir)
{
    REAL(snd_pcm_hw_params_test_rate);
    int r = real_snd_pcm_hw_params_test_rate(pcm, p, val, dir);
    if (!disabled && is_tracked(pcm))
        slog("test_rate %s %u -> %d %s", pname(pcm), val, r, r < 0 ? errstr(r) : "ok");
    return r;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int val)
{
    REAL(snd_pcm_hw_params_set_channels);
    int r = real_snd_pcm_hw_params_set_channels(pcm, p, val);
    if (!disabled && is_tracked(pcm))
        slog("set_channels %s %u -> %d %s", pname(pcm), val, r, r < 0 ? errstr(r) : "ok");
    return r;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, int fmt)
{
    REAL(snd_pcm_hw_params_set_format);
    int r = real_snd_pcm_hw_params_set_format(pcm, p, fmt);
    if (!disabled && is_tracked(pcm))
        slog("set_format %s %d -> %d %s", pname(pcm), fmt, r, r < 0 ? errstr(r) : "ok");
    return r;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, int acc)
{
    REAL(snd_pcm_hw_params_set_access);
    int r = real_snd_pcm_hw_params_set_access(pcm, p, acc);
    if (!disabled && is_tracked(pcm))
        slog("set_access %s %d -> %d %s", pname(pcm), acc, r, r < 0 ? errstr(r) : "ok");
    return r;
}

/* Bei Fehlschlag: Wünsche der Force und Fähigkeiten des Geräts ins Log */
typedef struct _snd_output snd_output_t;
static void dump_params(const char *title, snd_pcm_t *pcm, snd_pcm_hw_params_t *p)
{
    static int (*attach)(snd_output_t **, FILE *, int);
    static int (*dump)(const snd_pcm_hw_params_t *, snd_output_t *);
    static int (*oclose)(snd_output_t *);
    if (!attach) {
        attach = (int (*)(snd_output_t **, FILE *, int))dlsym(RTLD_DEFAULT, "snd_output_stdio_attach");
        dump   = (int (*)(const snd_pcm_hw_params_t *, snd_output_t *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_dump");
        oclose = (int (*)(snd_output_t *))dlsym(RTLD_DEFAULT, "snd_output_close");
    }
    if (!attach || !dump || !logf_ || !p) return;
    slog("--- %s (%s) ---", title, pname(pcm));
    pthread_mutex_lock(&log_mx);
    snd_output_t *out;
    if (attach(&out, logf_, 0) == 0) { dump(p, out); if (oclose) oclose(out); }
    fflush(logf_);
    pthread_mutex_unlock(&log_mx);
}

/*
 * Rettungsversuch: Die Force verlangt sehr knappe, exakte Puffer
 * (z. B. 3 x 128 Frames). Mit Umrechnung 44,1 -> 48 kHz ergeben sich
 * für das Gerät krumme Werte (128 * 48000/44100 = 139,3), die der
 * USB-Treiber ablehnt (-22). Wir stellen dieselben Grundwerte (Format,
 * Kanäle, Rate) neu ein und lassen ALSA passende Puffer wählen –
 * möglichst nah an der Anforderung, nie kleiner.
 */
typedef unsigned long ufr;
static int try_relaxed(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                       const snd_pcm_hw_params_t *want,
                       int (*real_hw)(snd_pcm_t *, snd_pcm_hw_params_t *),
                       int (*pmalloc)(snd_pcm_hw_params_t **),
                       void (*pfree)(snd_pcm_hw_params_t *),
                       void (*pcopy)(snd_pcm_hw_params_t *, const snd_pcm_hw_params_t *),
                       int (*pany)(snd_pcm_t *, snd_pcm_hw_params_t *))
{
#define SYM(var, name) var = (__typeof__(var))dlsym(RTLD_NEXT, name); if (!var) var = (__typeof__(var))dlsym(RTLD_DEFAULT, name)
    int (*g_access)(const snd_pcm_hw_params_t *, int *);
    int (*g_format)(const snd_pcm_hw_params_t *, int *);
    int (*g_ch)(const snd_pcm_hw_params_t *, unsigned *);
    int (*g_rate)(const snd_pcm_hw_params_t *, unsigned *, int *);
    int (*g_per)(const snd_pcm_hw_params_t *, ufr *, int *);
    int (*g_buf)(const snd_pcm_hw_params_t *, ufr *);
    int (*s_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*s_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*s_ch)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
    int (*s_rate)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int);
    int (*s_per)(snd_pcm_t *, snd_pcm_hw_params_t *, ufr *, int *);
    int (*s_buf)(snd_pcm_t *, snd_pcm_hw_params_t *, ufr *);
    int (*s_buf_min)(snd_pcm_t *, snd_pcm_hw_params_t *, ufr *);
    SYM(g_access, "snd_pcm_hw_params_get_access");
    SYM(g_format, "snd_pcm_hw_params_get_format");
    SYM(g_ch,     "snd_pcm_hw_params_get_channels_min");
    SYM(g_rate,   "snd_pcm_hw_params_get_rate_min");
    SYM(g_per,    "snd_pcm_hw_params_get_period_size_min");
    SYM(g_buf,    "snd_pcm_hw_params_get_buffer_size_min");
    SYM(s_access, "snd_pcm_hw_params_set_access");
    SYM(s_format, "snd_pcm_hw_params_set_format");
    SYM(s_ch,     "snd_pcm_hw_params_set_channels");
    SYM(s_rate,   "snd_pcm_hw_params_set_rate");
    SYM(s_per,    "snd_pcm_hw_params_set_period_size_near");
    SYM(s_buf,    "snd_pcm_hw_params_set_buffer_size_near");
    SYM(s_buf_min,"snd_pcm_hw_params_set_buffer_size_min");
#undef SYM
    if (!want || !g_access || !g_format || !g_ch || !g_rate || !g_per || !g_buf ||
        !s_access || !s_format || !s_ch || !s_rate || !s_per || !s_buf || !s_buf_min)
        return -22;

    int acc = 3, fmt = 10, d = 0;
    unsigned ch = 2, rate = 44100;
    ufr per = 128, buf = 384;
    g_access(want, &acc); g_format(want, &fmt); g_ch(want, &ch);
    g_rate(want, &rate, &d); g_per(want, &per, &d); g_buf(want, &buf);
    if (per < 16) per = 128;
    if (buf < per * 2) buf = per * 2;

    /* Versuche: (Periode, Mindestpuffer) – erst nah dran, dann großzügiger */
    struct { ufr per, buf; } tries[] = {
        { per,     buf     },
        { per,     buf * 2 },
        { per * 2, buf * 2 },
        { per * 2, buf * 4 },
        { 512,     2048    },
    };
    snd_pcm_hw_params_t *t = NULL;
    if (pmalloc(&t) < 0) return -22;
    int r = -22;
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        if (pany(pcm, t) < 0) break;
        if (s_access(pcm, t, acc) < 0 || s_format(pcm, t, fmt) < 0 ||
            s_ch(pcm, t, ch) < 0 || s_rate(pcm, t, rate, 0) < 0) {
            slog("  Rettung: Grundwerte (Format %d, %u Kanaele, %u Hz) nicht setzbar", fmt, ch, rate);
            break;
        }
        ufr pp = tries[i].per, bb = tries[i].buf;
        int dd = 0;
        int e1 = s_per(pcm, t, &pp, &dd);
        int e2 = s_buf_min(pcm, t, &bb);
        ufr bnear = tries[i].buf;
        int e3 = s_buf(pcm, t, &bnear);
        r = real_hw(pcm, t);
        slog("  Rettung %zu: Periode %lu->%lu, Puffer >=%lu ->%lu (%d/%d/%d) -> %d %s",
             i + 1, tries[i].per, pp, tries[i].buf, bnear, e1, e2, e3, r, r < 0 ? errstr(r) : "ok");
        if (r == 0) { pcopy(p, t); break; }
    }
    /* Alles gescheitert: eingrenzen, woran es liegt (nur Diagnose) */
    if (r < 0) {
        int (*hw_free)(snd_pcm_t *) = (int (*)(snd_pcm_t *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_free");
        struct { const char *what; int fmt; unsigned ch, rate; } probe[] = {
            { "ohne Umrechnung (48000 Hz)",          fmt, ch, 48000 },
            { "2 Kanaele, 44100 Hz",                 fmt, 2,  44100 },
            { "2 Kanaele, 48000 Hz",                 fmt, 2,  48000 },
            { "16 Bit, gleiche Kanaele, 44100 Hz",   2,   ch, 44100 },
            { "16 Bit, 2 Kanaele, 44100 Hz",         2,   2,  44100 },
        };
        slog("  Diagnose (wird nicht uebernommen):");
        for (size_t i = 0; i < sizeof probe / sizeof probe[0]; i++) {
            if (pany(pcm, t) < 0) break;
            int e = s_access(pcm, t, acc);
            if (!e) e = s_format(pcm, t, probe[i].fmt);
            if (!e) e = s_ch(pcm, t, probe[i].ch);
            if (!e) e = s_rate(pcm, t, probe[i].rate, 0);
            ufr pp = 256, bb = 1024; int dd = 0;
            if (!e) { s_per(pcm, t, &pp, &dd); s_buf(pcm, t, &bb); e = real_hw(pcm, t); }
            slog("    %-36s -> %d %s", probe[i].what, e, e < 0 ? errstr(e) : "ok");
            if (e == 0 && hw_free) hw_free(pcm);
        }
    }
    pfree(t);
    return r;
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p)
{
    REAL(snd_pcm_hw_params);
    static int (*pmalloc)(snd_pcm_hw_params_t **);
    static void (*pfree)(snd_pcm_hw_params_t *);
    static void (*pcopy)(snd_pcm_hw_params_t *, const snd_pcm_hw_params_t *);
    static int (*pany)(snd_pcm_t *, snd_pcm_hw_params_t *);
    if (!pmalloc) {
        pmalloc = (int (*)(snd_pcm_hw_params_t **))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_malloc");
        pfree   = (void (*)(snd_pcm_hw_params_t *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_free");
        pcopy   = (void (*)(snd_pcm_hw_params_t *, const snd_pcm_hw_params_t *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_copy");
        pany    = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_any");
    }
    int tracked_ = !disabled && is_tracked(pcm);
    snd_pcm_hw_params_t *saved = NULL;
    if (tracked_ && pmalloc && pcopy && pmalloc(&saved) == 0) pcopy(saved, p);

    int r = real_snd_pcm_hw_params(pcm, p);
    if (tracked_ && getenv("SRATE_TEST_FAILFIRST")) { static int once; if (!once++) r = -22; }
    if (tracked_) {
        slog("hw_params %s -> %d %s", pname(pcm), r, r < 0 ? errstr(r) : "ok");
        if (r == -22 && cfg.mode == MODE_PLUG && saved) {
            slog("Rettungsversuch mit passenden Puffergroessen ...");
            r = try_relaxed(pcm, p, saved, real_snd_pcm_hw_params, pmalloc, pfree, pcopy, pany);
            if (r == 0) { dump_params("Eingestellt nach Rettung", pcm, p); if (saved && pfree) pfree(saved); return 0; }
        }
        if (r < 0) {
            dump_params("Anforderung der Force", pcm, saved);
            snd_pcm_hw_params_t *any = NULL;
            if (pmalloc && pany && pmalloc(&any) == 0) {
                if (pany(pcm, any) == 0) dump_params("Moeglich laut Geraet", pcm, any);
                pfree(any);
            }
        } else {
            dump_params("Eingestellt", pcm, p);
        }
    }
    if (saved && pfree) pfree(saved);
    return r;
}

/* Weitere Einstellungen mitloggen */
typedef unsigned long uframes;
#define LOGPCM(fmt, ...) do { if (!disabled && is_tracked(pcm)) slog(fmt, __VA_ARGS__); } while (0)

int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *v)
{
    REAL(snd_pcm_hw_params_set_channels_near);
    unsigned w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_channels_near(pcm, p, v);
    LOGPCM("set_channels_near %s %u -> %d (bekommt %u)", pname(pcm), w, r, v ? *v : 0);
    return r;
}
int snd_pcm_hw_params_set_channels_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *v)
{
    REAL(snd_pcm_hw_params_set_channels_min);
    unsigned w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_channels_min(pcm, p, v);
    LOGPCM("set_channels_min %s %u -> %d", pname(pcm), w, r);
    return r;
}
int snd_pcm_hw_params_set_channels_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *v)
{
    REAL(snd_pcm_hw_params_set_channels_max);
    unsigned w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_channels_max(pcm, p, v);
    LOGPCM("set_channels_max %s %u -> %d", pname(pcm), w, r);
    return r;
}
int snd_pcm_hw_params_set_period_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, uframes v, int dir)
{
    REAL(snd_pcm_hw_params_set_period_size);
    int r = real_snd_pcm_hw_params_set_period_size(pcm, p, v, dir);
    LOGPCM("set_period_size %s %lu -> %d %s", pname(pcm), v, r, r < 0 ? errstr(r) : "ok");
    return r;
}
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, uframes *v, int *dir)
{
    REAL(snd_pcm_hw_params_set_period_size_near);
    uframes w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_period_size_near(pcm, p, v, dir);
    LOGPCM("set_period_size_near %s %lu -> %d (bekommt %lu)", pname(pcm), w, r, v ? *v : 0);
    return r;
}
int snd_pcm_hw_params_set_period_size_integer(snd_pcm_t *pcm, snd_pcm_hw_params_t *p)
{
    REAL(snd_pcm_hw_params_set_period_size_integer);
    int r = real_snd_pcm_hw_params_set_period_size_integer(pcm, p);
    LOGPCM("set_period_size_integer %s -> %d", pname(pcm), r);
    return r;
}
int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, uframes v)
{
    REAL(snd_pcm_hw_params_set_buffer_size);
    int r = real_snd_pcm_hw_params_set_buffer_size(pcm, p, v);
    LOGPCM("set_buffer_size %s %lu -> %d %s", pname(pcm), v, r, r < 0 ? errstr(r) : "ok");
    return r;
}
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, uframes *v)
{
    REAL(snd_pcm_hw_params_set_buffer_size_near);
    uframes w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_buffer_size_near(pcm, p, v);
    LOGPCM("set_buffer_size_near %s %lu -> %d (bekommt %lu)", pname(pcm), w, r, v ? *v : 0);
    return r;
}
int snd_pcm_hw_params_set_periods(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int v, int dir)
{
    REAL(snd_pcm_hw_params_set_periods);
    int r = real_snd_pcm_hw_params_set_periods(pcm, p, v, dir);
    LOGPCM("set_periods %s %u -> %d %s", pname(pcm), v, r, r < 0 ? errstr(r) : "ok");
    return r;
}
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *v, int *dir)
{
    REAL(snd_pcm_hw_params_set_periods_near);
    unsigned w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_periods_near(pcm, p, v, dir);
    LOGPCM("set_periods_near %s %u -> %d (bekommt %u)", pname(pcm), w, r, v ? *v : 0);
    return r;
}
int snd_pcm_hw_params_set_period_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *v, int *dir)
{
    REAL(snd_pcm_hw_params_set_period_time_near);
    unsigned w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_period_time_near(pcm, p, v, dir);
    LOGPCM("set_period_time_near %s %u -> %d (bekommt %u)", pname(pcm), w, r, v ? *v : 0);
    return r;
}
int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *v, int *dir)
{
    REAL(snd_pcm_hw_params_set_buffer_time_near);
    unsigned w = v ? *v : 0;
    int r = real_snd_pcm_hw_params_set_buffer_time_near(pcm, p, v, dir);
    LOGPCM("set_buffer_time_near %s %u -> %d (bekommt %u)", pname(pcm), w, r, v ? *v : 0);
    return r;
}

int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent)
{
    REAL(snd_pcm_recover);
    static unsigned long count;
    if (!disabled && is_tracked(pcm)) {
        count++;
        if (count <= 20 || count % 100 == 0)
            slog("recover %s err=%d %s (Xrun Nr. %lu)", pname(pcm), err, errstr(err), count);
    }
    return real_snd_pcm_recover(pcm, err, silent);
}
