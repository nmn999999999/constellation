#include "particle_codec/pseudo_random.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cmath>

namespace particle_codec {
    PerlinNoise::PerlinNoise(const std::vector<uint8_t> &seed) : perm_(512) {
        std::vector<uint8_t> p(256);
        for (int i = 0; i < 256; i++) p[i] = static_cast<uint8_t>(i);
        PseudoRandom rng(seed);
        rng.shuffle(p);
        for (int i = 0; i < 256; i++) {
            perm_[i] = p[i];
            perm_[i + 256] = p[i];
        }
    }

    static double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
    static double lerp(double t, double a, double b) { return a + t * (b - a); }

    static double grad(int hash, double x, double y) {
        int h = hash & 3;
        double u = h < 2 ? x : y;
        double v = h < 2 ? y : x;
        return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }

    double PerlinNoise::noise2D(double x, double y) const {
        int xi = static_cast<int>(std::floor(x)) & 255;
        int yi = static_cast<int>(std::floor(y)) & 255;
        double xf = x - std::floor(x);
        double yf = y - std::floor(y);
        double u = fade(xf);
        double v = fade(yf);

        int aa = perm_[perm_[xi] + yi];
        int ab = perm_[perm_[xi] + yi + 1];
        int ba = perm_[perm_[xi + 1] + yi];
        int bb = perm_[perm_[xi + 1] + yi + 1];

        return lerp(v,
                    lerp(u, grad(aa, xf, yf), grad(ba, xf - 1, yf)),
                    lerp(u, grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1)));
    }

    double PerlinNoise::fbm(double x, double y, int octaves, double lacunarity, double gain) const {
        double sum = 0, amp = 1.0, freq = 1.0, maxAmp = 0;
        for (int i = 0; i < octaves; i++) {
            sum += noise2D(x * freq, y * freq) * amp;
            maxAmp += amp;
            amp *= gain;
            freq *= lacunarity;
        }
        return sum / maxAmp;
    }
} // namespace particle_codec


