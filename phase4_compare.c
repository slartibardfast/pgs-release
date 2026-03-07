/*
 * Phase 4 investigation: compare sRGB vs OkLab color distance
 * for DVD subtitle palette selection.
 *
 * Standalone program — does not require FFmpeg build.
 * Reproduces dvdsubenc.c's color_distance() and compares against
 * OkLab perceptual distance for all 16x16 palette pairs plus
 * synthetic test colors.
 *
 * Build: gcc -O2 -o phase4_compare phase4_compare.c -lm
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- dvdsubenc.c's color_distance (verbatim) ---- */

static int color_distance_srgb(uint32_t a, uint32_t b)
{
    int r = 0, d, i;
    int alpha_a = 8, alpha_b = 8;

    for (i = 24; i >= 0; i -= 8) {
        d = alpha_a * (int)((a >> i) & 0xFF) -
            alpha_b * (int)((b >> i) & 0xFF);
        r += d * d;
        alpha_a = a >> 28;
        alpha_b = b >> 28;
    }
    return r;
}

/* ---- OkLab implementation (from libavutil/palette.c, simplified) ---- */

/* sRGB gamma decode LUT: 8-bit sRGB -> 16-bit linear */
static int32_t srgb_to_linear[256];
/* sRGB gamma encode LUT: 16-bit linear -> 8-bit sRGB (not needed here) */

static void init_luts(void)
{
    for (int i = 0; i < 256; i++) {
        double s = i / 255.0;
        double linear;
        if (s <= 0.04045)
            linear = s / 12.92;
        else
            linear = pow((s + 0.055) / 1.055, 2.4);
        srgb_to_linear[i] = (int32_t)(linear * 65535.0 + 0.5);
    }
}

struct Lab { int32_t L, a, b; };

static struct Lab srgb_u8_to_oklab(uint32_t srgb)
{
    int32_t r = srgb_to_linear[(srgb >> 16) & 0xFF];
    int32_t g = srgb_to_linear[(srgb >>  8) & 0xFF];
    int32_t b = srgb_to_linear[(srgb >>  0) & 0xFF];

    /* OkLab matrix (fixed-point, scale 1<<15) */
    /* M1: sRGB linear -> LMS (approximate cube root via cbrt) */
    double rd = r / 65535.0, gd = g / 65535.0, bd = b / 65535.0;

    double l = 0.4122214708 * rd + 0.5363325363 * gd + 0.0514459929 * bd;
    double m = 0.2119034982 * rd + 0.6806995451 * gd + 0.1073969566 * bd;
    double s = 0.0883024619 * rd + 0.2817188376 * gd + 0.6299787005 * bd;

    double l_ = cbrt(l), m_ = cbrt(m), s_ = cbrt(s);

    double L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    double A = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    double B = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;

    /* Scale to fixed-point: L in [0,1], a,b in [-0.5,0.5] approx */
    struct Lab result;
    result.L = (int32_t)(L * 65535.0);
    result.a = (int32_t)(A * 65535.0);
    result.b = (int32_t)(B * 65535.0);
    return result;
}

static int64_t color_distance_oklab(uint32_t a_color, uint32_t b_color)
{
    /* Strip alpha for color comparison, handle alpha separately */
    struct Lab la = srgb_u8_to_oklab(a_color & 0x00FFFFFF);
    struct Lab lb = srgb_u8_to_oklab(b_color & 0x00FFFFFF);

    int64_t dL = la.L - lb.L;
    int64_t da = la.a - lb.a;
    int64_t db = la.b - lb.b;

    /* Alpha difference (same scale as dvdsubenc) */
    int alpha_a = (a_color >> 24) & 0xFF;
    int alpha_b = (b_color >> 24) & 0xFF;
    int64_t d_alpha = (alpha_a - alpha_b);

    /* Weighted: color distance + alpha distance */
    return dL * dL + da * da + db * db + d_alpha * d_alpha * 256;
}

/* ---- DVD default palette (from dvdsubenc.c dvdsub_init) ---- */

static const uint32_t default_palette[16] = {
    0x000000, 0x0000FF, 0x00FF00, 0xFF0000,
    0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    0x808000, 0x8080FF, 0x800080, 0x80FF80,
    0x008080, 0xFF8080, 0x555555, 0xAAAAAA,
};

/* ---- Test: for a set of input colors, find nearest palette entry ---- */

static int nearest_srgb(uint32_t color, const uint32_t *palette, int n)
{
    int best = 0, best_d = color_distance_srgb(color, palette[0]);
    for (int j = 1; j < n; j++) {
        int d = color_distance_srgb(color, palette[j]);
        if (d < best_d) { best_d = d; best = j; }
    }
    return best;
}

static int nearest_oklab(uint32_t color, const uint32_t *palette, int n)
{
    int best = 0;
    int64_t best_d = color_distance_oklab(color, palette[0]);
    for (int j = 1; j < n; j++) {
        int64_t d = color_distance_oklab(color, palette[j]);
        if (d < best_d) { best_d = d; best = j; }
    }
    return best;
}

/* ---- Exhaustive and targeted tests ---- */

int main(void)
{
    init_luts();

    /* Make palette entries opaque for comparison (dvdsubenc masks with
       0xFF000000 in count_colors and build_color_map) */
    uint32_t palette[16];
    for (int i = 0; i < 16; i++)
        palette[i] = 0xFF000000 | default_palette[i];

    /* Test 1: Exhaustive — all 256^3 RGB colors (no alpha variation) */
    printf("=== Test 1: Exhaustive RGB sweep (16M colors) ===\n");
    int divergent = 0;
    long total = 0;
    /* Sample every 4th value to keep runtime reasonable: 64^3 = 262144 */
    for (int r = 0; r < 256; r += 4) {
        for (int g = 0; g < 256; g += 4) {
            for (int b = 0; b < 256; b += 4) {
                uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
                int s = nearest_srgb(color, palette, 16);
                int o = nearest_oklab(color, palette, 16);
                if (s != o) divergent++;
                total++;
            }
        }
    }
    printf("  Sampled: %ld colors (step=4)\n", total);
    printf("  Divergent: %d (%.2f%%)\n", divergent,
           100.0 * divergent / total);

    /* Test 2: Targeted — colors known to expose sRGB vs perceptual diffs */
    printf("\n=== Test 2: Targeted problem colors ===\n");
    /* These are colors where sRGB distance is known to disagree with
       perceptual distance: mid-grays, skin tones, dark blues vs dark reds */
    struct { uint32_t color; const char *name; } targets[] = {
        { 0xFF404040, "dark gray" },
        { 0xFF808080, "mid gray" },
        { 0xFFC0C0C0, "light gray" },
        { 0xFFE0B090, "skin tone" },
        { 0xFF705030, "dark skin" },
        { 0xFF000040, "very dark blue" },
        { 0xFF400000, "very dark red" },
        { 0xFF004000, "very dark green" },
        { 0xFF202060, "dark blue-gray" },
        { 0xFF602020, "dark red-gray" },
        { 0xFFC08040, "orange" },
        { 0xFF40C080, "sea green" },
        { 0xFF8060C0, "purple" },
        { 0xFFC0C000, "dark yellow" },
        { 0xFF006080, "teal" },
        { 0xFFA0A060, "olive" },
        { 0xFF6060A0, "blue-gray" },
        { 0xFFA06060, "red-gray" },
        { 0xFF60A060, "green-gray" },
        { 0xFFA060A0, "mauve" },
    };
    int targeted_div = 0;
    int targeted_total = sizeof(targets) / sizeof(targets[0]);
    for (int i = 0; i < targeted_total; i++) {
        int s = nearest_srgb(targets[i].color, palette, 16);
        int o = nearest_oklab(targets[i].color, palette, 16);
        if (s != o) {
            printf("  DIVERGENT %-16s #%06X -> sRGB: #%06X [%d] vs OkLab: #%06X [%d]\n",
                   targets[i].name, targets[i].color & 0xFFFFFF,
                   palette[s] & 0xFFFFFF, s,
                   palette[o] & 0xFFFFFF, o);
            targeted_div++;
        }
    }
    printf("  Divergent: %d/%d\n", targeted_div, targeted_total);

    /* Test 3: For divergent cases from Test 1, categorize what changes */
    printf("\n=== Test 3: Divergence by palette entry swap ===\n");
    int swap_matrix[16][16];
    memset(swap_matrix, 0, sizeof(swap_matrix));
    for (int r = 0; r < 256; r += 4) {
        for (int g = 0; g < 256; g += 4) {
            for (int b = 0; b < 256; b += 4) {
                uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
                int s = nearest_srgb(color, palette, 16);
                int o = nearest_oklab(color, palette, 16);
                if (s != o) swap_matrix[s][o]++;
            }
        }
    }
    printf("  Most common swaps (sRGB->OkLab):\n");
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            if (swap_matrix[i][j] > 100)
                printf("    #%06X [%2d] -> #%06X [%2d]: %d colors\n",
                       palette[i] & 0xFFFFFF, i,
                       palette[j] & 0xFFFFFF, j,
                       swap_matrix[i][j]);

    /* Test 4: Simulate full DVD encoder pipeline — select 4 best colors
       for a synthetic subtitle with a known 256-color palette */
    printf("\n=== Test 4: Full pipeline — select 4 from 16 ===\n");
    printf("  (Simulates count_colors + select_palette for anti-aliased text)\n");

    /* Simulate anti-aliased white text on transparent background:
       most pixels transparent, some white, some gray (anti-aliasing) */
    uint32_t synth_palette[256];
    int synth_counts[256];
    memset(synth_counts, 0, sizeof(synth_counts));

    /* Color 0: transparent */
    synth_palette[0] = 0x00000000;
    synth_counts[0] = 10000;  /* background */

    /* Color 1: white (main text) */
    synth_palette[1] = 0xFFFFFFFF;
    synth_counts[1] = 3000;

    /* Color 2: black (outline) */
    synth_palette[2] = 0xFF000000;
    synth_counts[2] = 1500;

    /* Colors 3-10: anti-aliasing grays */
    for (int i = 3; i <= 10; i++) {
        int v = 32 * (i - 2);  /* 64, 96, 128, ... 256->255 */
        if (v > 255) v = 255;
        synth_palette[i] = 0xFF000000 | (v << 16) | (v << 8) | v;
        synth_counts[i] = 200;
    }

    /* For each input color, find nearest in global palette with both metrics */
    printf("  Input colors and their nearest global palette entry:\n");
    for (int i = 0; i <= 10; i++) {
        if (synth_counts[i] == 0) continue;
        uint32_t c = synth_palette[i];
        /* For transparent, skip distance (dvdsubenc bins it separately) */
        if ((c >> 24) < 0x33) {
            printf("    [%2d] #%08X (count=%5d) -> transparent bin\n",
                   i, c, synth_counts[i]);
            continue;
        }
        int s = nearest_srgb(0xFF000000 | c, palette, 16);
        int o = nearest_oklab(0xFF000000 | c, palette, 16);
        printf("    [%2d] #%08X (count=%5d) -> sRGB: #%06X [%2d]%s OkLab: #%06X [%2d]%s\n",
               i, c, synth_counts[i],
               palette[s] & 0xFFFFFF, s, s == o ? " " : "*",
               palette[o] & 0xFFFFFF, o, s == o ? "" : " <-- DIFFERENT");
    }

    printf("\n=== Summary ===\n");
    printf("Exhaustive divergence rate: %.2f%% (%d / %ld)\n",
           100.0 * divergent / total, divergent, total);
    printf("Targeted divergence: %d / %d\n", targeted_div, targeted_total);

    return 0;
}
