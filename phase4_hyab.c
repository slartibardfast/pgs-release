/*
 * Phase 4: verify HyAB fixes Euclidean OkLab's sparse palette failures.
 * Compare all three metrics: sRGB, Euclidean OkLab, HyAB-in-OkLab.
 *
 * Build: gcc -O2 -o phase4_hyab phase4_hyab.c -lm
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static double dist_srgb(uint32_t a, uint32_t b)
{
    double dr = (double)((a>>16)&0xFF) - (double)((b>>16)&0xFF);
    double dg = (double)((a>>8)&0xFF)  - (double)((b>>8)&0xFF);
    double db = (double)((a>>0)&0xFF)  - (double)((b>>0)&0xFF);
    return dr*dr + dg*dg + db*db;
}

static double dist_oklab_euclidean(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
    return dL*dL + da*da + db*db;
}

static double dist_hyab(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = fabs(a.L - b.L);
    double da = a.a - b.a, db = a.b - b.b;
    double chroma_dist = sqrt(da*da + db*db);
    return dL + chroma_dist;
}

static const uint32_t pal[16] = {
    0x000000, 0x0000FF, 0x00FF00, 0xFF0000,
    0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    0x808000, 0x8080FF, 0x800080, 0x80FF80,
    0x008080, 0xFF8080, 0x555555, 0xAAAAAA,
};

static const char *pal_names[16] = {
    "black", "blue", "green", "red",
    "yellow", "magenta", "cyan", "white",
    "olive", "light-blue", "purple", "light-green",
    "teal", "salmon", "dark-gray", "light-gray",
};

typedef double (*dist_fn)(uint32_t, uint32_t);

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

static void analyze(const char *name, uint32_t color, const char *expected)
{
    int s = nearest(color, dist_srgb);
    int o = nearest(color, dist_oklab_euclidean);
    int h = nearest(color, dist_hyab);

    printf("  %-16s #%06X -> sRGB: %-12s  OkLab: %-12s  HyAB: %-12s  (expect: %s)%s\n",
           name, color,
           pal_names[s], pal_names[o], pal_names[h], expected,
           (s != o || s != h) ? "" : "");
}

int main(void)
{
    /* Test 1: The six problem cases from our investigation */
    printf("=== Problem cases from Phase 4 investigation ===\n");
    printf("  %-16s %-8s    %-14s %-14s %-14s %s\n",
           "Color", "Hex", "sRGB", "OkLab-Euclid", "HyAB-OkLab", "Expected");
    printf("  %-16s %-8s    %-14s %-14s %-14s %s\n",
           "-----", "---", "----", "------------", "----------", "--------");

    analyze("mid gray",    0x808080, "light-gray");
    analyze("skin tone",   0xE0B090, "salmon");
    analyze("dark yellow", 0xC0C000, "yellow");
    analyze("orange",      0xC08040, "salmon/olive");
    analyze("sea green",   0x40C080, "light-green");
    analyze("dark red #40", 0x400000, "black");

    /* Test 2: Exhaustive divergence rates */
    printf("\n=== Exhaustive sweep (262K colors, step=4) ===\n");
    int srgb_vs_oklab = 0, srgb_vs_hyab = 0, oklab_vs_hyab = 0;
    int hyab_matches_srgb_when_oklab_wrong = 0;
    int hyab_wrong_when_oklab_right = 0;
    long total = 0;

    for (int r = 0; r < 256; r += 4) {
        for (int g = 0; g < 256; g += 4) {
            for (int b = 0; b < 256; b += 4) {
                uint32_t color = (r << 16) | (g << 8) | b;
                int s = nearest(color, dist_srgb);
                int o = nearest(color, dist_oklab_euclidean);
                int h = nearest(color, dist_hyab);
                if (s != o) srgb_vs_oklab++;
                if (s != h) srgb_vs_hyab++;
                if (o != h) oklab_vs_hyab++;
                if (s != o && h == s) hyab_matches_srgb_when_oklab_wrong++;
                if (s == o && h != s) hyab_wrong_when_oklab_right++;
                total++;
            }
        }
    }

    printf("  Total sampled: %ld\n", total);
    printf("  sRGB vs OkLab-Euclid divergence: %d (%.2f%%)\n",
           srgb_vs_oklab, 100.0 * srgb_vs_oklab / total);
    printf("  sRGB vs HyAB divergence:         %d (%.2f%%)\n",
           srgb_vs_hyab, 100.0 * srgb_vs_hyab / total);
    printf("  OkLab vs HyAB divergence:        %d (%.2f%%)\n",
           oklab_vs_hyab, 100.0 * oklab_vs_hyab / total);
    printf("\n");
    printf("  When OkLab-Euclid disagrees with sRGB:\n");
    printf("    HyAB agrees with sRGB: %d / %d (%.1f%%)\n",
           hyab_matches_srgb_when_oklab_wrong, srgb_vs_oklab,
           100.0 * hyab_matches_srgb_when_oklab_wrong / srgb_vs_oklab);
    printf("  When OkLab-Euclid agrees with sRGB:\n");
    printf("    HyAB disagrees (regression): %d / %ld (%.2f%%)\n",
           hyab_wrong_when_oklab_right, total - srgb_vs_oklab,
           100.0 * hyab_wrong_when_oklab_right / (total - srgb_vs_oklab));

    /* Test 3: Detail dump for the gray->teal case */
    printf("\n=== Detail: mid gray #808080 distances ===\n");
    printf("  %-12s  %10s  %10s  %10s\n", "palette", "sRGB", "OkLab", "HyAB");
    for (int i = 0; i < 16; i++) {
        double sd = dist_srgb(0x808080, pal[i]);
        double od = dist_oklab_euclidean(0x808080, pal[i]);
        double hd = dist_hyab(0x808080, pal[i]);
        printf("  %-12s  %10.1f  %10.6f  %10.6f%s\n",
               pal_names[i], sd, od, hd,
               (i == 12 || i == 14 || i == 15) ? "  <--" : "");
    }

    return 0;
}
