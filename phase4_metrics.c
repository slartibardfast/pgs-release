/*
 * Phase 4: comprehensive color distance metric comparison.
 * Compare all candidate metrics for sparse palette nearest-neighbor.
 *
 * Metrics tested:
 *   1. sRGB Euclidean (dvdsubenc.c baseline)
 *   2. OkLab Euclidean (current quantizer)
 *   3. HyAB: |ΔL| + √(Δa²+Δb²)
 *   4. Weighted HyAB: |ΔL| + w·√(Δa²+Δb²), w=2,3,4
 *   5. Weighted OkLab Euclidean: ΔL² + w·(Δa²+Δb²), w=2,3,4
 *   6. OkLab chroma-priority: w·ΔL² + Δa² + Δb², w=0.5,0.25
 *   7. CIEDE2000 (in CIELAB)
 *
 * Build: gcc -O2 -o phase4_metrics phase4_metrics.c -lm
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- OkLab ---- */

struct Lab { double L, a, b; };

static struct Lab srgb_to_oklab(uint32_t srgb)
{
    double r = ((srgb >> 16) & 0xFF) / 255.0;
    double g = ((srgb >>  8) & 0xFF) / 255.0;
    double b = ((srgb >>  0) & 0xFF) / 255.0;

    r = r <= 0.04045 ? r / 12.92 : pow((r + 0.055) / 1.055, 2.4);
    g = g <= 0.04045 ? g / 12.92 : pow((g + 0.055) / 1.055, 2.4);
    b = b <= 0.04045 ? b / 12.92 : pow((b + 0.055) / 1.055, 2.4);

    double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

    l = cbrt(l); m = cbrt(m); s = cbrt(s);

    struct Lab result;
    result.L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    result.a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    result.b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return result;
}

/* ---- CIELAB ---- */

static double lab_f(double t)
{
    const double d = 6.0 / 29.0;
    if (t > d * d * d)
        return cbrt(t);
    return t / (3.0 * d * d) + 4.0 / 29.0;
}

static struct Lab srgb_to_cielab(uint32_t srgb)
{
    double r = ((srgb >> 16) & 0xFF) / 255.0;
    double g = ((srgb >>  8) & 0xFF) / 255.0;
    double b = ((srgb >>  0) & 0xFF) / 255.0;

    /* sRGB to linear */
    r = r <= 0.04045 ? r / 12.92 : pow((r + 0.055) / 1.055, 2.4);
    g = g <= 0.04045 ? g / 12.92 : pow((g + 0.055) / 1.055, 2.4);
    b = b <= 0.04045 ? b / 12.92 : pow((b + 0.055) / 1.055, 2.4);

    /* Linear sRGB to XYZ (D65) */
    double X = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
    double Y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    double Z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;

    /* D65 reference white */
    const double Xn = 0.95047, Yn = 1.0, Zn = 1.08883;

    double fx = lab_f(X / Xn);
    double fy = lab_f(Y / Yn);
    double fz = lab_f(Z / Zn);

    struct Lab result;
    result.L = 116.0 * fy - 16.0;
    result.a = 500.0 * (fx - fy);
    result.b = 200.0 * (fy - fz);
    return result;
}

/* ---- CIEDE2000 ---- */

static double deg2rad(double d) { return d * M_PI / 180.0; }
static double rad2deg(double r) { return r * 180.0 / M_PI; }

static double ciede2000(uint32_t c1, uint32_t c2)
{
    struct Lab lab1 = srgb_to_cielab(c1);
    struct Lab lab2 = srgb_to_cielab(c2);

    double L1 = lab1.L, a1 = lab1.a, b1 = lab1.b;
    double L2 = lab2.L, a2 = lab2.a, b2 = lab2.b;

    double C1 = sqrt(a1*a1 + b1*b1);
    double C2 = sqrt(a2*a2 + b2*b2);
    double Cab = (C1 + C2) / 2.0;

    double Cab7 = pow(Cab, 7.0);
    double G = 0.5 * (1.0 - sqrt(Cab7 / (Cab7 + pow(25.0, 7.0))));

    double a1p = a1 * (1.0 + G);
    double a2p = a2 * (1.0 + G);

    double C1p = sqrt(a1p*a1p + b1*b1);
    double C2p = sqrt(a2p*a2p + b2*b2);

    double h1p = (fabs(a1p) + fabs(b1) == 0.0) ? 0.0 : rad2deg(atan2(b1, a1p));
    if (h1p < 0.0) h1p += 360.0;
    double h2p = (fabs(a2p) + fabs(b2) == 0.0) ? 0.0 : rad2deg(atan2(b2, a2p));
    if (h2p < 0.0) h2p += 360.0;

    double dLp = L2 - L1;
    double dCp = C2p - C1p;

    double dhp;
    if (C1p * C2p == 0.0) {
        dhp = 0.0;
    } else if (fabs(h2p - h1p) <= 180.0) {
        dhp = h2p - h1p;
    } else if (h2p - h1p > 180.0) {
        dhp = h2p - h1p - 360.0;
    } else {
        dhp = h2p - h1p + 360.0;
    }

    double dHp = 2.0 * sqrt(C1p * C2p) * sin(deg2rad(dhp / 2.0));

    double Lp = (L1 + L2) / 2.0;
    double Cp = (C1p + C2p) / 2.0;

    double hp;
    if (C1p * C2p == 0.0) {
        hp = h1p + h2p;
    } else if (fabs(h1p - h2p) <= 180.0) {
        hp = (h1p + h2p) / 2.0;
    } else if (h1p + h2p < 360.0) {
        hp = (h1p + h2p + 360.0) / 2.0;
    } else {
        hp = (h1p + h2p - 360.0) / 2.0;
    }

    double T = 1.0
        - 0.17 * cos(deg2rad(hp - 30.0))
        + 0.24 * cos(deg2rad(2.0 * hp))
        + 0.32 * cos(deg2rad(3.0 * hp + 6.0))
        - 0.20 * cos(deg2rad(4.0 * hp - 63.0));

    double Lp50sq = (Lp - 50.0) * (Lp - 50.0);
    double SL = 1.0 + 0.015 * Lp50sq / sqrt(20.0 + Lp50sq);
    double SC = 1.0 + 0.045 * Cp;
    double SH = 1.0 + 0.015 * Cp * T;

    double Cp7 = pow(Cp, 7.0);
    double RT = -2.0 * sqrt(Cp7 / (Cp7 + pow(25.0, 7.0)))
                * sin(deg2rad(60.0 * exp(-pow((hp - 275.0) / 25.0, 2.0))));

    /* kL = kC = kH = 1 (graphic arts) */
    double dE = sqrt(
        (dLp / SL) * (dLp / SL) +
        (dCp / SC) * (dCp / SC) +
        (dHp / SH) * (dHp / SH) +
        RT * (dCp / SC) * (dHp / SH)
    );

    return dE;
}

/* ---- Distance metric definitions ---- */

typedef double (*dist_fn)(uint32_t, uint32_t);

static double d_srgb(uint32_t a, uint32_t b)
{
    double dr = (double)((a>>16)&0xFF) - (double)((b>>16)&0xFF);
    double dg = (double)((a>>8)&0xFF)  - (double)((b>>8)&0xFF);
    double db = (double)((a>>0)&0xFF)  - (double)((b>>0)&0xFF);
    return dr*dr + dg*dg + db*db;
}

static double d_oklab(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    return (a.L-b.L)*(a.L-b.L) + (a.a-b.a)*(a.a-b.a) + (a.b-b.b)*(a.b-b.b);
}

static double d_hyab(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double da = a.a-b.a, db = a.b-b.b;
    return fabs(a.L-b.L) + sqrt(da*da + db*db);
}

/* HyAB with chroma weight w: |ΔL| + w·√(Δa²+Δb²) */
static double d_hyab_w2(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double da = a.a-b.a, db = a.b-b.b;
    return fabs(a.L-b.L) + 2.0 * sqrt(da*da + db*db);
}

static double d_hyab_w3(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double da = a.a-b.a, db = a.b-b.b;
    return fabs(a.L-b.L) + 3.0 * sqrt(da*da + db*db);
}

static double d_hyab_w4(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double da = a.a-b.a, db = a.b-b.b;
    return fabs(a.L-b.L) + 4.0 * sqrt(da*da + db*db);
}

/* Weighted OkLab Euclidean: ΔL² + w·(Δa²+Δb²) */
static double d_oklab_cw2(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = a.L-b.L, da = a.a-b.a, db = a.b-b.b;
    return dL*dL + 2.0*(da*da + db*db);
}

static double d_oklab_cw3(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = a.L-b.L, da = a.a-b.a, db = a.b-b.b;
    return dL*dL + 3.0*(da*da + db*db);
}

static double d_oklab_cw4(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = a.L-b.L, da = a.a-b.a, db = a.b-b.b;
    return dL*dL + 4.0*(da*da + db*db);
}

/* Lightness-reduced OkLab: w·ΔL² + Δa² + Δb² */
static double d_oklab_lw05(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = a.L-b.L, da = a.a-b.a, db = a.b-b.b;
    return 0.5*dL*dL + da*da + db*db;
}

static double d_oklab_lw025(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = a.L-b.L, da = a.a-b.a, db = a.b-b.b;
    return 0.25*dL*dL + da*da + db*db;
}

static double d_ciede2000(uint32_t c1, uint32_t c2)
{
    return ciede2000(c1, c2);
}

/* ---- Palette ---- */

static const uint32_t pal[16] = {
    0x000000, 0x0000FF, 0x00FF00, 0xFF0000,
    0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    0x808000, 0x8080FF, 0x800080, 0x80FF80,
    0x008080, 0xFF8080, 0x555555, 0xAAAAAA,
};

static const char *pal_names[16] = {
    "black", "blue", "green", "red",
    "yellow", "magenta", "cyan", "white",
    "olive", "lt-blue", "purple", "lt-green",
    "teal", "salmon", "dk-gray", "lt-gray",
};

static int nearest(uint32_t color, dist_fn fn)
{
    int best = 0;
    double best_d = fn(color, pal[0]);
    for (int j = 1; j < 16; j++) {
        double d = fn(color, pal[j]);
        if (d < best_d) { best_d = d; best = j; }
    }
    return best;
}

/* ---- Test harness ---- */

struct metric {
    const char *name;
    const char *short_name;
    dist_fn fn;
};

static struct metric metrics[] = {
    { "sRGB Euclidean",          "sRGB",      d_srgb },
    { "OkLab Euclidean",         "OkLab",     d_oklab },
    { "HyAB (w=1)",             "HyAB1",     d_hyab },
    { "HyAB (w=2)",             "HyAB2",     d_hyab_w2 },
    { "HyAB (w=3)",             "HyAB3",     d_hyab_w3 },
    { "HyAB (w=4)",             "HyAB4",     d_hyab_w4 },
    { "OkLab chroma×2",         "OkCw2",     d_oklab_cw2 },
    { "OkLab chroma×3",         "OkCw3",     d_oklab_cw3 },
    { "OkLab chroma×4",         "OkCw4",     d_oklab_cw4 },
    { "OkLab light×0.5",        "OkLw.5",    d_oklab_lw05 },
    { "OkLab light×0.25",       "OkLw.25",   d_oklab_lw025 },
    { "CIEDE2000",               "CIE00",     d_ciede2000 },
};
#define N_METRICS (sizeof(metrics)/sizeof(metrics[0]))

/* Human-judged problem cases with expected best answer */
struct test_case {
    uint32_t color;
    const char *name;
    int expected_idx;   /* palette index human would pick */
    const char *reason;
};

static struct test_case cases[] = {
    { 0x808080, "mid gray",     15, "gray should map to gray, not teal" },
    { 0xE0B090, "skin tone",    13, "warm color should stay warm (salmon)" },
    { 0xC0C000, "dark yellow",   4, "desaturated yellow should map to yellow" },
    { 0xC08040, "orange",        8, "orange is between salmon and olive (olive ok)" },
    { 0x40C080, "sea green",    11, "green-ish should map to light-green" },
    { 0x400000, "v.dark red",    0, "very dark should map to black" },
    { 0x202020, "near black",   14, "dark gray AA, either black or dk-gray ok" },
    { 0x606060, "med-dk gray",  14, "medium-dark gray -> dark-gray" },
    { 0xA0A0A0, "med-lt gray",  15, "medium-light gray -> light-gray" },
    { 0x804000, "brown",         8, "brown is closest to olive" },
    { 0x004080, "dark azure",   12, "dark cyan-ish -> teal" },
    { 0x800040, "dark rose",    10, "dark rose -> purple" },
    { 0xC060C0, "orchid",        5, "orchid -> magenta" },
    { 0x60C0C0, "light teal",   12, "light teal -> teal or cyan" },
    { 0xFFC080, "peach",        13, "peach -> salmon" },
    { 0x80C0FF, "sky blue",      9, "sky blue -> light-blue" },
    { 0xC0FF80, "lime",         11, "lime -> light-green" },
    { 0xFF8040, "tangerine",    13, "orange-red -> salmon" },
    { 0x404080, "dark slate",   10, "dark blue-purple -> purple" },
    { 0x408040, "forest green",  8, "dark green -> olive" },
};
#define N_CASES (sizeof(cases)/sizeof(cases[0]))

int main(void)
{
    /* Test 1: Problem cases — which metrics agree with human judgment? */
    printf("=== Test 1: Human-judged cases (%d colors) ===\n\n", N_CASES);

    /* Header */
    printf("%-14s %-8s %-10s", "Color", "Hex", "Expected");
    for (int m = 0; m < (int)N_METRICS; m++)
        printf("  %-7s", metrics[m].short_name);
    printf("\n");
    for (int i = 0; i < 14+8+10; i++) printf("-");
    for (int m = 0; m < (int)N_METRICS; m++) printf("  -------");
    printf("\n");

    int correct[N_METRICS];
    memset(correct, 0, sizeof(correct));

    for (int i = 0; i < N_CASES; i++) {
        printf("%-14s #%06X %-10s",
               cases[i].name, cases[i].color, pal_names[cases[i].expected_idx]);
        for (int m = 0; m < (int)N_METRICS; m++) {
            int pick = nearest(cases[i].color, metrics[m].fn);
            int ok = (pick == cases[i].expected_idx);
            /* Also accept adjacent reasonable picks for ambiguous cases */
            if (cases[i].color == 0xC08040) /* orange: salmon or olive ok */
                ok = (pick == 8 || pick == 13);
            if (cases[i].color == 0x202020) /* near black: black or dk-gray ok */
                ok = (pick == 0 || pick == 14);
            if (cases[i].color == 0x60C0C0) /* light teal: teal or cyan ok */
                ok = (pick == 6 || pick == 12);
            if (ok) correct[m]++;
            printf("  %s%-7s", ok ? " " : "*", pal_names[pick]);
        }
        printf("\n");
    }

    printf("\nCorrect picks:\n");
    for (int m = 0; m < (int)N_METRICS; m++)
        printf("  %-22s %2d/%d (%.0f%%)\n",
               metrics[m].name, correct[m], N_CASES,
               100.0 * correct[m] / N_CASES);

    /* Test 2: Exhaustive divergence from sRGB (our baseline) */
    printf("\n=== Test 2: Exhaustive sweep (262K colors, step=4) ===\n");
    printf("Divergence from sRGB baseline:\n\n");

    long total = 0;
    int divergence[N_METRICS];
    memset(divergence, 0, sizeof(divergence));

    for (int r = 0; r < 256; r += 4) {
        for (int g = 0; g < 256; g += 4) {
            for (int b = 0; b < 256; b += 4) {
                uint32_t color = (r << 16) | (g << 8) | b;
                int srgb_pick = nearest(color, d_srgb);
                for (int m = 0; m < (int)N_METRICS; m++) {
                    int pick = nearest(color, metrics[m].fn);
                    if (pick != srgb_pick) divergence[m]++;
                }
                total++;
            }
        }
    }

    for (int m = 0; m < (int)N_METRICS; m++)
        printf("  %-22s %6d / %ld  (%.2f%%)\n",
               metrics[m].name, divergence[m], total,
               100.0 * divergence[m] / total);

    /* Test 3: Agreement with CIEDE2000 (gold standard) */
    printf("\n=== Test 3: Agreement with CIEDE2000 ===\n");

    int agree_ciede[N_METRICS];
    memset(agree_ciede, 0, sizeof(agree_ciede));

    for (int r = 0; r < 256; r += 4) {
        for (int g = 0; g < 256; g += 4) {
            for (int b = 0; b < 256; b += 4) {
                uint32_t color = (r << 16) | (g << 8) | b;
                int ciede_pick = nearest(color, d_ciede2000);
                for (int m = 0; m < (int)N_METRICS; m++) {
                    int pick = nearest(color, metrics[m].fn);
                    if (pick == ciede_pick) agree_ciede[m]++;
                }
            }
        }
    }

    printf("Agreement rate with CIEDE2000:\n");
    for (int m = 0; m < (int)N_METRICS; m++)
        printf("  %-22s %6d / %ld  (%.2f%%)\n",
               metrics[m].name, agree_ciede[m], total,
               100.0 * agree_ciede[m] / total);

    /* Test 4: CIEDE2000 on the problem cases */
    printf("\n=== Test 4: CIEDE2000 on problem cases ===\n");
    printf("  (Does the gold standard agree with our human judgment?)\n\n");
    int ciede_correct = 0;
    for (int i = 0; i < N_CASES; i++) {
        int pick = nearest(cases[i].color, d_ciede2000);
        int ok = (pick == cases[i].expected_idx);
        if (cases[i].color == 0xC08040) ok = (pick == 8 || pick == 13);
        if (cases[i].color == 0x202020) ok = (pick == 0 || pick == 14);
        if (cases[i].color == 0x60C0C0) ok = (pick == 6 || pick == 12);
        if (ok) ciede_correct++;
        printf("  %-14s #%06X -> %-10s %s  (expected: %s%s)\n",
               cases[i].name, cases[i].color, pal_names[pick],
               ok ? "OK" : "MISS",
               pal_names[cases[i].expected_idx],
               ok ? "" : " !");
    }
    printf("  CIEDE2000 correct: %d/%d\n", ciede_correct, N_CASES);

    return 0;
}
