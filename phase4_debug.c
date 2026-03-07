/*
 * Phase 4 debug: dump OkLab values for suspicious mappings
 * Build: gcc -O2 -o phase4_debug phase4_debug.c -lm
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

struct Lab { double L, a, b; };

static struct Lab srgb_to_oklab(uint32_t srgb)
{
    double r = ((srgb >> 16) & 0xFF) / 255.0;
    double g = ((srgb >>  8) & 0xFF) / 255.0;
    double b = ((srgb >>  0) & 0xFF) / 255.0;

    /* sRGB gamma decode */
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

static double oklab_dist(uint32_t c1, uint32_t c2)
{
    struct Lab a = srgb_to_oklab(c1);
    struct Lab b = srgb_to_oklab(c2);
    double dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
    return dL*dL + da*da + db*db;
}

static double srgb_dist(uint32_t a, uint32_t b)
{
    /* Simplified — no alpha (both opaque) */
    double dr = (double)((a>>16)&0xFF) - (double)((b>>16)&0xFF);
    double dg = (double)((a>>8)&0xFF)  - (double)((b>>8)&0xFF);
    double db = (double)((a>>0)&0xFF)  - (double)((b>>0)&0xFF);
    return dr*dr + dg*dg + db*db;
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

static void analyze(const char *name, uint32_t color)
{
    printf("\n--- %s (#%06X) ---\n", name, color);
    struct Lab c = srgb_to_oklab(color);
    printf("  OkLab: L=%.4f a=%.4f b=%.4f\n", c.L, c.a, c.b);

    printf("  Distances to palette:\n");
    printf("  %4s  %-12s  %12s  %12s\n", "idx", "name", "sRGB_dist", "OkLab_dist");
    int best_s = 0, best_o = 0;
    double best_sd = 1e30, best_od = 1e30;
    for (int i = 0; i < 16; i++) {
        double sd = srgb_dist(color, pal[i]);
        double od = oklab_dist(color, pal[i]);
        printf("  [%2d]  %-12s  %12.1f  %12.6f", i, pal_names[i], sd, od);
        if (sd < best_sd) { best_sd = sd; best_s = i; }
        if (od < best_od) { best_od = od; best_o = i; }
        if (i == best_s || i == best_o) printf("  <--");
        printf("\n");
    }
    printf("  sRGB nearest:  [%2d] %s\n", best_s, pal_names[best_s]);
    printf("  OkLab nearest: [%2d] %s\n", best_o, pal_names[best_o]);
    if (best_s != best_o)
        printf("  ** DIVERGENT **\n");
}

int main(void)
{
    analyze("mid gray",    0x808080);
    analyze("dark yellow", 0xC0C000);
    analyze("skin tone",   0xE0B090);
    analyze("orange",      0xC08040);
    analyze("sea green",   0x40C080);
    analyze("dark gray",   0x404040);
    analyze("dark red #20", 0x200000);
    analyze("dark red #40", 0x400000);
    return 0;
}
