#include "eye_scene.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "eye_config.h"

static const char *TAG = "eye_scene";

#define EYE_PI 3.14159265358979323846f

/* ------------------------------------------------------------- geometry --
 * The eye is drawn as an almond-shaped opening between two lid curves that
 * meet at the corners. Everything outside the lids is "skin" (near-black,
 * like the reference GIF); inside is sclera + iris, or -- for the Rinnegan
 * family -- the pattern fills the whole opening.
 */
#define SC_CX      120
#define SC_CY      120
#define EYE_HW     106      /* half-width: lid corners at CX +/- HW          */
#define EYE_HU      64      /* upper lid peak height when fully open         */
#define EYE_HL      46      /* lower lid depth when fully open               */
#define EYE_CLINE    6      /* closure line sits slightly below centre       */
#define LOWR      0.30f     /* lower-lid rest fraction while blinking        */
#define IRIS_R      66

/* Blink / transition timing (ms of *scaled* clock). */
#define DUR_BOOT_OPEN  1100
#define DUR_BLINK_DN     90
#define DUR_BLINK_UP    180
#define DUR_SW_CLOSE    140
#define DUR_SW_OPEN     420
#define DUR_FLARE       650

/* ------------------------------------------------------------------ LUTs */
static uint8_t *s_rad;        /* EYE_FB_PX: radius from (120,120), px       */
static uint8_t *s_ang;        /* EYE_FB_PX: angle, 0..255 == full turn      */
static int8_t   s_cosl[256];  /* cos scaled to [-127,127]                   */

/* ------------------------------------------------------- animation state */
typedef enum { ST_OPEN, ST_IDLE, ST_BCLOSE, ST_BOPEN, ST_SCLOSE, ST_SOPEN } st_t;

static int      s_scene = EYE_SCENE_TOMOE3;
static int      s_pending = EYE_SCENE_TOMOE3;
static st_t     s_st = ST_OPEN;
static uint32_t s_t0;               /* state entry time (scaled clock)      */
static uint32_t s_clk, s_last_t;
static bool     s_first = true;

static uint32_t s_next_blink = 3200;
static uint32_t s_next_gaze  = 2400;
static uint32_t s_next_flare = 7000;
static uint32_t s_flare_t0   = 0xF0000000;   /* far future                  */

static float    s_gx, s_gy, s_tgx, s_tgy;    /* gaze offset + target        */
static float    s_phacc;                     /* iris rotation, 0..256 wraps */

static uint32_t s_rng = 0x243F6A88;
static inline uint32_t rnd(uint32_t n)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (s_rng >> 16) % (n ? n : 1);
}

static inline float ease_out_cubic(float x)
{
    float i = 1.0f - x;
    return 1.0f - i * i * i;
}

/* -------------------------------------------------- per-frame parameters
 * The pixel loop reads these file-statics instead of taking arguments, so
 * the per-pixel shader calls stay cheap.
 */
typedef enum { K_SHARINGAN, K_PINWHEEL, K_PINWHEEL_I, K_RINNEGAN,
               K_RINNEGAN_T, K_BYAKUGAN } kind_t;

static kind_t  s_kind;
static bool    s_fill;               /* pattern fills the whole eye opening */
static int     s_tomoe_n;
static int     s_ph;                 /* iris rotation phase, 0..255         */
static int     s_pupil;
static int     s_orbit;              /* tomoe orbit radius                  */
static int     s_gap;                /* rinnegan ring spacing               */
static int     s_drift;              /* rinnegan ring outward drift, px     */
static int     s_cr, s_cg, s_cb;     /* scene primary color                 */
static int     s_offx, s_offy;       /* iris centre offset from (120,120)   */
static int     s_glx, s_gly;         /* glint centre                        */
static int16_t s_yu[EYE_FB_W], s_yl[EYE_FB_W];
static uint8_t s_cf[EYE_FB_W];       /* sclera corner-darkening per column  */

/* ------------------------------------------------------------- helpers  */
static inline int iclamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint16_t shade(int r, int g, int b, int s)
{
    s = iclamp(s, 0, 255);
    return eye_rgb565((uint8_t)((r * s) >> 8), (uint8_t)((g * s) >> 8),
                      (uint8_t)((b * s) >> 8));
}

static inline uint16_t mix565(uint16_t a, uint16_t b)
{
    return (uint16_t)(((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1));
}

/* Is (r, a) -- polar around the iris centre -- inside one of n tomoe?
 * Head disc via the law of cosines; the tail trails behind along the orbit,
 * thinning smoothly and hugging the orbit (the classic comma shape). Tail is
 * kept short so multiple tomoe never merge into a lumpy ring. */
#define TOMOE_TAIL 42
static inline bool tomoe_hit(int r, int a, int n, int orbit, int headr, int ph)
{
    for (int k = 0; k < n; k++) {
        int ak = (ph + (k * 256) / n) & 255;
        int da = (a - ak) & 255;
        int c  = s_cosl[da];
        int d2 = r * r + orbit * orbit - ((2 * r * orbit * c) / 127);
        if (d2 <= headr * headr) {
            return true;
        }
        if (da <= TOMOE_TAIL) {
            int rc = orbit - ((da * da) >> 10);          /* gentle inward curl */
            int w  = ((headr - 1) * (TOMOE_TAIL - da) * 3) / (TOMOE_TAIL * 4);
            int dr = r - rc;
            if (dr < 0) {
                dr = -dr;
            }
            if (w >= 2 && dr <= w) {
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------- iris shaders   */

static inline uint16_t px_sharingan(int r, int a)
{
    if (r <= s_pupil) {
        return 0;                                   /* pupil: pure black    */
    }
    int rn = (r * 255) / IRIS_R;
    if (rn >= 228) {
        return shade(s_cr, s_cg, s_cb, 40);         /* limbal ring          */
    }
    if (tomoe_hit(r, a, s_tomoe_n, s_orbit, 10, s_ph)) {
        return eye_rgb565(10, 2, 4);                /* tomoe: near black    */
    }
    int dr = r - s_orbit;
    if (dr < 0) {
        dr = -dr;
    }
    if (dr <= 1) {
        return shade(s_cr, s_cg, s_cb, 70);         /* orbit circle         */
    }
    int s = 248 - ((rn * 132) >> 8);                /* bright centre -> rim */
    if (rn >= 214) {
        s = (s * (228 - rn)) / 14;                  /* soften into the ring */
    }
    uint16_t c = shade(s_cr, s_cg, s_cb, s);
    if (r <= s_pupil + 1) {
        c = mix565(c, 0);                           /* pupil edge AA        */
    }
    return c;
}

static inline uint16_t px_pinwheel(int r, int a, bool itachi)
{
    int rn = (r * 255) / IRIS_R;
    if (itachi && rn <= 62) {
        return 0;                                   /* big black hub        */
    }
    if (r <= s_pupil) {
        return 0;
    }
    if (rn >= 228) {
        return shade(s_cr, s_cg, s_cb, 40);
    }
    /* Blade coordinate: angle + radius-coupled sweep = curved blades. */
    int curve = itachi ? r : ((r * 3) >> 1);
    int m = (a - s_ph + curve) & 255;
    int t = (m * 3) & 255;
    int w = itachi ? (116 - ((rn * 84) >> 8)) : (86 - ((rn * 62) >> 8));
    if (t < w) {
        return eye_rgb565(10, 2, 4);                /* blade                */
    }
    if (t < w + 10) {                               /* blade edge AA band   */
        return shade(s_cr, s_cg, s_cb, 90);
    }
    int s = 240 - ((rn * 120) >> 8);
    if (rn >= 214) {
        s = (s * (228 - rn)) / 14;
    }
    return shade(s_cr, s_cg, s_cb, s);
}

static inline uint16_t px_rinnegan(int r, int a, bool with_tomoe)
{
    if (r <= s_pupil) {
        return 0;
    }
    int s = 252 - ((r * 96) / 170);                 /* gentle radial falloff */
    if (with_tomoe && r <= s_gap * 3 + 9) {
        /* the nearest of the first three rings carries 3 tomoe each,
         * every ring's trio offset so the 9 don't line up */
        int ri = (r + s_gap / 2) / s_gap;
        if (ri >= 1 && ri <= 3 &&
            tomoe_hit(r, a, 3, s_gap * ri, 7, s_ph + ri * 43)) {
            return eye_rgb565(24, 10, 36);
        }
    }
    int pos = (r - s_drift) % s_gap;
    if (pos < 0) {
        pos += s_gap;
    }
    if (pos <= 1) {
        return shade(s_cr, s_cg, s_cb, (s * 2) / 5);  /* ripple ring line   */
    }
    uint16_t c = shade(s_cr, s_cg, s_cb, s);
    if (r <= s_pupil + 1) {
        c = mix565(c, 0);
    }
    return c;
}

static inline uint16_t px_byakugan(int r)
{
    int s = 252 - ((r * 64) / 170);
    int dr = r - 56;                                /* whisper of an iris   */
    if (dr < 0) {
        dr = -dr;
    }
    if (dr <= 1) {
        s = (s * 210) >> 8;
    }
    return shade(s_cr, s_cg, s_cb, s);
}

static inline uint16_t iris_pixel(int r, int a)
{
    switch (s_kind) {
    case K_SHARINGAN:  return px_sharingan(r, a);
    case K_PINWHEEL:   return px_pinwheel(r, a, false);
    case K_PINWHEEL_I: return px_pinwheel(r, a, true);
    case K_RINNEGAN:   return px_rinnegan(r, a, false);
    case K_RINNEGAN_T: return px_rinnegan(r, a, true);
    default:           return px_byakugan(r);
    }
}

/* -------------------------------------------------------------- public  */

esp_err_t eye_scene_init(void)
{
    if (s_rad != NULL) {
        return ESP_OK;
    }
    s_rad = (uint8_t *)heap_caps_malloc(EYE_FB_PX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ang = (uint8_t *)heap_caps_malloc(EYE_FB_PX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rad == NULL || s_ang == NULL) {
        free(s_rad);
        free(s_ang);
        s_rad = (uint8_t *)heap_caps_malloc(EYE_FB_PX, MALLOC_CAP_8BIT);
        s_ang = (uint8_t *)heap_caps_malloc(EYE_FB_PX, MALLOC_CAP_8BIT);
    }
    if (s_rad == NULL || s_ang == NULL) {
        ESP_LOGE(TAG, "LUT alloc failed");
        free(s_rad);
        free(s_ang);
        s_rad = s_ang = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < EYE_FB_H; y++) {
        for (int x = 0; x < EYE_FB_W; x++) {
            float dx = (float)x - SC_CX;
            float dy = (float)y - SC_CY;
            int   r  = (int)(sqrtf(dx * dx + dy * dy) + 0.5f);
            int   a  = (int)(atan2f(dy, dx) * (256.0f / (2.0f * EYE_PI)));
            int   i  = y * EYE_FB_W + x;
            s_rad[i] = (uint8_t)(r > 255 ? 255 : r);
            s_ang[i] = (uint8_t)(a & 0xFF);
        }
    }
    for (int i = 0; i < 256; i++) {
        s_cosl[i] = (int8_t)lrintf(cosf((float)i * (2.0f * EYE_PI / 256.0f)) * 127.0f);
    }

    ESP_LOGI(TAG, "scene engine ready (%d scenes, %u-byte polar LUTs)",
             EYE_SCENE_COUNT, (unsigned)(2 * EYE_FB_PX));
    return ESP_OK;
}

int eye_scene_current(void)
{
    return s_scene;
}

void eye_scene_select(int id)
{
    if (id < 0 || id >= EYE_SCENE_COUNT) {
        return;
    }
    s_pending = id;
    if (s_st != ST_SCLOSE) {           /* blink-close, morph, reopen */
        s_st = ST_SCLOSE;
        s_t0 = s_clk;
    }
}

void eye_scene_enter(int id)
{
    if (id < 0 || id >= EYE_SCENE_COUNT) {
        return;
    }
    s_scene = s_pending = id;
    s_st = ST_OPEN;                    /* full dramatic eye-opening  */
    s_t0 = s_clk;
}

/* ------------------------------------------------------------ rendering */

static float update_state(const eye_params_t *p)
{
    float o = 1.0f;
    uint32_t el = s_clk - s_t0;

    switch (s_st) {
    case ST_OPEN:
        if (el >= DUR_BOOT_OPEN) {
            s_st = ST_IDLE;
            s_next_blink = s_clk + 1500 + rnd(3000);
        } else {
            o = ease_out_cubic((float)el / DUR_BOOT_OPEN);
        }
        break;
    case ST_IDLE:
        if (p->blink_ms > 0 && s_clk >= s_next_blink) {
            s_st = ST_BCLOSE;
            s_t0 = s_clk;
        }
        break;
    case ST_BCLOSE:
        if (el >= DUR_BLINK_DN) {
            s_st = ST_BOPEN;
            s_t0 = s_clk;
            o = 0.0f;
        } else {
            o = 1.0f - (float)el / DUR_BLINK_DN;
        }
        break;
    case ST_BOPEN:
        if (el >= DUR_BLINK_UP) {
            s_st = ST_IDLE;
            /* occasional quick double blink feels alive */
            s_next_blink = (rnd(100) < 22)
                         ? s_clk + 260
                         : s_clk + (uint32_t)p->blink_ms / 2 + rnd((uint32_t)p->blink_ms);
            /* blinks often land with a small gaze shift */
            if (p->gaze_px > 0 && rnd(100) < 50) {
                s_tgx = (float)((int)rnd(2 * p->gaze_px + 1) - p->gaze_px);
                s_tgy = 0.6f * (float)((int)rnd(p->gaze_px + 1) - p->gaze_px / 2);
            }
        } else {
            o = ease_out_cubic((float)el / DUR_BLINK_UP);
        }
        break;
    case ST_SCLOSE:
        if (el >= DUR_SW_CLOSE) {
            s_scene = s_pending;       /* morph happens behind closed lids */
            s_st = ST_SOPEN;
            s_t0 = s_clk;
            o = 0.0f;
        } else {
            o = 1.0f - (float)el / DUR_SW_CLOSE;
        }
        break;
    case ST_SOPEN:
        if (el >= DUR_SW_OPEN) {
            s_st = ST_IDLE;
            s_next_blink = s_clk + 1800 + rnd(3000);
        } else {
            o = ease_out_cubic((float)el / DUR_SW_OPEN);
        }
        break;
    }
    return o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
}

void eye_scene_render(uint16_t *fb, uint32_t t_ms, const eye_params_t *p)
{
    if (s_rad == NULL) {
        return;
    }

    /* ---- scaled clock ---- */
    if (s_first) {
        s_last_t = t_ms;
        s_first = false;
    }
    uint32_t dt = t_ms - s_last_t;
    s_last_t = t_ms;
    if (dt > 100) {
        dt = 100;
    }
    dt = dt * (uint32_t)p->speed_pct / 100u;
    s_clk += dt;

    float o = update_state(p);

    /* ---- gaze saccades ---- */
    if (p->gaze_px > 0) {
        if (s_clk >= s_next_gaze) {
            if (rnd(100) < 55) {       /* mostly return near centre */
                s_tgx = (float)((int)rnd(7) - 3);
                s_tgy = (float)((int)rnd(5) - 2);
            } else {
                s_tgx = (float)((int)rnd(2 * p->gaze_px + 1) - p->gaze_px);
                s_tgy = 0.6f * (float)((int)rnd(p->gaze_px + 1) - p->gaze_px / 2);
            }
            s_next_gaze = s_clk + 1600 + rnd(3400);
        }
        float k = 1.0f - expf(-(float)dt / 45.0f);   /* quick saccade */
        s_gx += (s_tgx - s_gx) * k;
        s_gy += (s_tgy - s_gy) * k;
    } else {
        s_gx = s_gy = 0.0f;
    }

    /* ---- iris spin + flare ---- */
    if (p->spin_ms != 0) {
        s_phacc += (float)(int)dt * 256.0f / (float)p->spin_ms;
        while (s_phacc >= 256.0f) s_phacc -= 256.0f;
        while (s_phacc < 0.0f)    s_phacc += 256.0f;
    }
    float flare_add = 0.0f, flare_env = 0.0f;
    if (p->flare_ms > 0 && s_clk >= s_next_flare && s_st == ST_IDLE) {
        s_flare_t0 = s_clk;
        s_next_flare = s_clk + (uint32_t)p->flare_ms / 2 + rnd((uint32_t)p->flare_ms);
    }
    if (s_clk - s_flare_t0 < DUR_FLARE) {
        float e = (float)(s_clk - s_flare_t0) / DUR_FLARE;
        flare_add = 88.0f * ease_out_cubic(e);       /* fast extra 1/3 turn */
        flare_env = sinf(EYE_PI * e);                /* pupil constricts    */
    }
    s_ph = ((int)(s_phacc + flare_add)) & 255;

    /* ---- per-frame parameters ---- */
    static const struct { kind_t kind; bool fill; int tomoe; } k_scene[EYE_SCENE_COUNT] = {
        [EYE_SCENE_TOMOE1]         = { K_SHARINGAN,  false, 1 },
        [EYE_SCENE_TOMOE2]         = { K_SHARINGAN,  false, 2 },
        [EYE_SCENE_TOMOE3]         = { K_SHARINGAN,  false, 3 },
        [EYE_SCENE_MANGEKYOU]      = { K_PINWHEEL,   false, 0 },
        [EYE_SCENE_ITACHI]         = { K_PINWHEEL_I, false, 0 },
        [EYE_SCENE_RINNEGAN]       = { K_RINNEGAN,   true,  0 },
        [EYE_SCENE_TOMOE_RINNEGAN] = { K_RINNEGAN_T, true,  0 },
        [EYE_SCENE_BYAKUGAN]       = { K_BYAKUGAN,   true,  0 },
    };
    s_kind    = k_scene[s_scene].kind;
    s_fill    = k_scene[s_scene].fill;
    s_tomoe_n = k_scene[s_scene].tomoe;
    s_cr = p->col_r[s_scene];
    s_cg = p->col_g[s_scene];
    s_cb = p->col_b[s_scene];
    s_pupil = p->pupil_px - (int)(flare_env * 0.30f * (float)p->pupil_px);
    if (s_kind == K_RINNEGAN || s_kind == K_RINNEGAN_T) {
        s_pupil = (p->pupil_px * 3) / 4;             /* rinnegan: small pupil */
    }
    s_orbit = (s_pupil + IRIS_R) / 2;
    s_gap   = 21;
    /* Plain rinnegan ripples drift slowly outward (hypnotic); the tomoe
     * variant keeps its rings still so the tomoe stay seated on them. */
    s_drift = (s_kind == K_RINNEGAN) ? (int)((s_clk / 90u) % (uint32_t)s_gap) : 0;

    int ix = SC_CX + (int)s_gx;
    int iy = SC_CY + (int)s_gy;
    s_offx = ix - 120;
    s_offy = iy - 120;
    s_glx = ix - 26;
    s_gly = iy - 27;

    /* ---- lid curves per column ---- */
    const int XL = SC_CX - EYE_HW, XR = SC_CX + EYE_HW;
    for (int x = 0; x < EYE_FB_W; x++) {
        if (x < XL || x > XR) {
            s_yu[x] = 0x7FFF;          /* column entirely skin */
            s_yl[x] = -0x7FFF;
            s_cf[x] = 255;
            continue;
        }
        float u  = (float)(x - SC_CX) / (float)EYE_HW;
        float f  = 1.0f - u * u;
        f *= 1.0f - 0.10f * u;         /* slight asymmetry: peak off-centre */
        float ycl = (float)(SC_CY + EYE_CLINE);
        float yu = ycl - o * EYE_HU * f + (1.0f - o) * LOWR * EYE_HL * f;
        float yl = ycl + (LOWR + (1.0f - LOWR) * o) * EYE_HL * f;
        if (yu >= yl - 0.5f) {
            yu = yl + 1.0f;            /* fully closed column: no interior  */
        }
        s_yu[x] = (int16_t)yu;
        s_yl[x] = (int16_t)yl;
        float au = u < 0 ? -u : u;     /* sclera darkens toward the corners */
        int cf = 255;
        if (au > 0.68f) {
            cf = 255 - (int)((au - 0.68f) * 290.0f);
            if (cf < 130) {
                cf = 130;
            }
        }
        s_cf[x] = (uint8_t)cf;
    }

    /* ---- pixel loop ---- */
    const int glr2 = 576;              /* glint ellipse: dx^2*4 + dy^2*9 <= 576 */
    for (int y = 0; y < EYE_FB_H; y++) {
        int ly = iclamp(y - s_offy, 0, EYE_FB_H - 1);
        const uint8_t *radrow = s_rad + ly * EYE_FB_W;
        const uint8_t *angrow = s_ang + ly * EYE_FB_W;
        const uint8_t *scrrow = s_rad + y * EYE_FB_W;   /* screen-centred r */
        uint16_t *out = fb + y * EYE_FB_W;

        for (int x = 0; x < EYE_FB_W; x++) {
            int yu = s_yu[x], yl = s_yl[x];
            uint16_t c;

            if (y < yu || y > yl) {
                /* ---------- skin / lids ---------- */
                int d = (y < yu) ? (yu - y) : (y - yl);
                int rs = scrrow[x];
                int fade = rs > 100 ? 255 - (rs - 100) * 3 : 255;
                if (fade < 30) {
                    fade = 30;
                }
                if (d <= 1) {
                    c = 0;                                   /* lash liner  */
                } else if (y < yu && d <= 6) {
                    c = shade(70, 66, 78, fade);             /* upper fold  */
                } else if (y < yu && d <= 12) {
                    c = shade(34, 32, 40, fade);
                } else if (y > yl && d <= 4) {
                    c = shade(42, 40, 48, fade);             /* lower edge  */
                } else {
                    c = shade(15, 12, 17, fade);             /* skin        */
                }
                out[x] = c;
                continue;
            }

            /* ---------- inside the eye ---------- */
            int lx = iclamp(x - s_offx, 0, EYE_FB_W - 1);
            int r = radrow[lx];
            int a = angrow[lx];

            if (s_fill) {
                c = iris_pixel(r, a);
            } else if (r <= IRIS_R) {
                c = iris_pixel(r, a);
                if (r >= IRIS_R - 1) {
                    c = mix565(c, shade(228, 231, 240, s_cf[x]));
                }
            } else {
                /* sclera with corner + lower-lid shading */
                int sh = s_cf[x];
                if (yl - y < 4) {
                    sh = (sh * 200) >> 8;
                }
                c = shade(228, 231, 240, sh);
            }

            /* upper-lid shadow falls on everything inside */
            int dI = y - yu;
            if (dI < 4) {
                c = (uint16_t)((c >> 1) & 0x7BEF);           /* 50%  */
            } else if (dI < 8) {
                c = (uint16_t)(c - ((c >> 2) & 0x39E7));     /* 75%  */
            } else if (dI < 13) {
                c = (uint16_t)(c - ((c >> 3) & 0x18E3));     /* 87%  */
            }

            /* specular glint (after shadow so it stays bright) */
            int gdx = x - s_glx;
            if (gdx >= -14 && gdx <= 14) {
                int gdy = y - s_gly;
                if (gdy >= -9 && gdy <= 9) {
                    int e = gdx * gdx * 4 + gdy * gdy * 9;
                    if (e <= glr2) {
                        c = 0xFFFF;
                    } else if (e <= glr2 + 170) {
                        c = mix565(c, 0xFFFF);
                    }
                }
            }
            int sdx = x - (s_glx + 44), sdy = y - (s_gly + 43);
            if (sdx * sdx + sdy * sdy <= 9) {
                c = mix565(c, 0xFFFF);                       /* small dot   */
            }

            out[x] = c;
        }
    }
}
