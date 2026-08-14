// nebula_render.h — shared pure-pixel nebula renderer for all Constellation
// image/video producers (video generator, viewer-export tests, precision
// benchmark, demo images). A procedural star chart: Perlin cloud background,
// radial-gradient star halos with cross-ray spikes on big stars, bright cyan
// cores (the only thing is_cyan() detects), and density-modulated dust.
#pragma once

#include <particle_codec/pseudo_random.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nebula {

struct Star {
    double gx, gy;    // grid coordinates (already at the irregular centre)
    double size = 1.0;
    double bright = 1.0;
    double alpha = 1.0;
    bool isDust = false;
};

inline uint8_t mix8(int bg, int target, double a) {
    return static_cast<uint8_t>(bg + (target - bg) * a);
}

// Background: low-frequency Perlin cloud sampled every 4 px, bilinearly
// interpolated; deep blue-violet, strong contrast, never cyan (G stays low).
inline void paint_background(std::vector<uint8_t> &px, int W, int H,
                             int margin, const particle_codec::PerlinNoise &noise,
                             double t) {
    const int step = 4;
    const int gw = W / step + 1, gh = H / step + 1;
    std::vector<float> cloud(gw * gh);
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            double cx = (gx * step) / 8.0;
            double cy = (gy * step) / 8.0;
            double n1 = noise.fbm(cx * 0.045 + t * 0.008, cy * 0.045, 2);
            double n2 = noise.fbm(cx * 0.16 + 9.3, cy * 0.16 + 2.7, 2);
            cloud[gy * gw + gx] = static_cast<float>(n1 * 0.65 + n2 * 0.35);
        }
    for (int y = 0; y < H; y++) {
        double fy = static_cast<double>(y) / step;
        int gy0 = static_cast<int>(fy);
        double ty = fy - gy0;
        for (int x = 0; x < W; x++) {
            double fx = static_cast<double>(x) / step;
            int gx0 = static_cast<int>(fx);
            double tx = fx - gx0;
            auto sample = [&](int gxi, int gyi) {
                gxi = std::clamp(gxi, 0, gw - 1);
                gyi = std::clamp(gyi, 0, gh - 1);
                return cloud[gyi * gw + gxi];
            };
            float v00 = sample(gx0, gy0), v10 = sample(gx0 + 1, gy0);
            float v01 = sample(gx0, gy0 + 1), v11 = sample(gx0 + 1, gy0 + 1);
            float v = (v00 * (1 - tx) + v10 * tx) * (1 - ty) +
                      (v01 * (1 - tx) + v11 * tx) * ty;
            double k = v * 0.5 + 0.5;
            double kc = k * k; // darken the voids, keep clouds glowing
            int idx = (y * W + x) * 3;
            px[idx] = mix8(3, 36, kc);
            px[idx + 1] = mix8(3, 18, kc);
            px[idx + 2] = mix8(12, 78, kc);
        }
    }
    // Black margin area (carries corner markers) stays clean dark.
    if (margin > 0) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (x < margin || x >= W - margin || y < margin || y >= H - margin) {
                    int idx = (y * W + x) * 3;
                    px[idx] = 4;
                    px[idx + 1] = 4;
                    px[idx + 2] = 14;
                }
            }
    }
}

// Soft radial-gradient halo (non-cyan) plus faint cross-ray spikes on big
// stars. Halo green target is kept low so stacked halos never reach the
// is_cyan threshold.
inline void draw_halo(std::vector<uint8_t> &px, int W, int H, int cx, int cy,
                      double size, double alpha) {
    int gr = static_cast<int>(6.5 * size) + 2;
    for (int dy = -gr; dy <= gr; dy++)
        for (int dx = -gr; dx <= gr; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            double d = std::sqrt(dx * dx + dy * dy);
            if (d > gr) continue;
            double fade = 1.0 - d / gr;
            double a = alpha * fade * fade * fade;
            int idx = (y * W + x) * 3;
            if (d < gr * 0.45) {
                px[idx] = mix8(px[idx], 0, a);
                px[idx + 1] = mix8(px[idx + 1], 28, a);
                px[idx + 2] = mix8(px[idx + 2], 88, a);
            } else {
                px[idx] = mix8(px[idx], 26, a);
                px[idx + 1] = mix8(px[idx + 1], 16, a);
                px[idx + 2] = mix8(px[idx + 2], 58, a);
            }
        }
    // NOTE: cross-ray spikes are intentionally NOT drawn — stacked halos plus
    // rays pushed the green channel past is_cyan() and merged into the core
    // blob, shifting centroids. The nebula background + size contrast + dust
    // already give the star-chart chaos.
}

// Bright cyan core: symmetric filled disc, centroid lands exactly on centre.
inline void draw_core(std::vector<uint8_t> &px, int W, int H, int cx, int cy,
                      double bright, double alpha) {
    const int cr = 2;
    int g = mix8(0, 185, bright);
    int b = mix8(0, 212, bright);
    for (int dy = -cr; dy <= cr; dy++)
        for (int dx = -cr; dx <= cr; dx++) {
            if (dx * dx + dy * dy > cr * cr + 1) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            int idx = (y * W + x) * 3;
            px[idx] = mix8(px[idx], 0, alpha);
            px[idx + 1] = mix8(px[idx + 1], g, alpha);
            px[idx + 2] = mix8(px[idx + 2], b, alpha);
        }
}

// Render a star/dust list to a 24-bit BMP with the nebula look.
// `gridCols/rows` give the px-per-cell scale: W = gridCols*8 + 2*margin.
// margin>0 also draws the 4 corner markers when `markers` is true.
inline void render(const std::vector<Star> &stars, int gridCols, int gridRows,
                   int margin, bool markers, double t,
                   const particle_codec::PerlinNoise &noise,
                   const std::string &path) {
    const int kScale = 8;
    const int W = gridCols * kScale + 2 * margin;
    const int H = gridRows * kScale + 2 * margin;
    std::vector<uint8_t> px(W * H * 3);
    paint_background(px, W, H, margin, noise, t);

    // Dust first (bottom layer).
    for (const auto &s : stars) {
        if (!s.isDust) continue;
        int sx = static_cast<int>(s.gx * kScale) + margin;
        int sy = static_cast<int>(s.gy * kScale) + margin;
        double a = s.alpha * s.bright * 0.55;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx * dx + dy * dy > 1) continue;
                int x = sx + dx, y = sy + dy;
                if (x < 0 || x >= W || y < 0 || y >= H) continue;
                int idx = (y * W + x) * 3;
                px[idx] = mix8(px[idx], 34, a);
                px[idx + 1] = mix8(px[idx + 1], 42, a);
                px[idx + 2] = mix8(px[idx + 2], 76, a);
            }
    }
    // Halos.
    for (const auto &s : stars) {
        if (s.isDust) continue;
        int sx = static_cast<int>(s.gx * kScale) + margin;
        int sy = static_cast<int>(s.gy * kScale) + margin;
        draw_halo(px, W, H, sx, sy, s.size, s.alpha);
    }
    // Cores on top.
    for (const auto &s : stars) {
        if (s.isDust) continue;
        int sx = static_cast<int>(s.gx * kScale) + margin;
        int sy = static_cast<int>(s.gy * kScale) + margin;
        draw_core(px, W, H, sx, sy, s.bright, s.alpha);
    }
    // Corner markers in the margin.
    if (markers && margin > 0) {
        const int ms = 4, mi = 2;
        const int corners[4][2] = {
            {mi, mi}, {W - mi - ms, mi}, {mi, H - mi - ms}, {W - mi - ms, H - mi - ms}};
        for (int c = 0; c < 4; c++)
            for (int dy = 0; dy < ms; dy++)
                for (int dx = 0; dx < ms; dx++) {
                    int x = corners[c][0] + dx, y = corners[c][1] + dy;
                    int idx = (y * W + x) * 3;
                    px[idx] = 210;
                    px[idx + 1] = 50;
                    px[idx + 2] = 230;
                }
    }

    // Write BMP (storage is BGR, our buffer is RGB -> swap).
    std::vector<uint8_t> bmpData(px.size());
    for (size_t i = 0; i < px.size(); i += 3) {
        bmpData[i] = px[i + 2];
        bmpData[i + 1] = px[i + 1];
        bmpData[i + 2] = px[i];
    }
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        // 14-byte BITMAPFILEHEADER + 40-byte BITMAPINFOHEADER, byte-packed.
        unsigned char hdr[54] = {0};
        unsigned int fileSize = 54 + static_cast<unsigned int>(bmpData.size());
        hdr[0] = 'B';
        hdr[1] = 'M';
        hdr[2] = fileSize & 0xFF;
        hdr[3] = (fileSize >> 8) & 0xFF;
        hdr[4] = (fileSize >> 16) & 0xFF;
        hdr[5] = (fileSize >> 24) & 0xFF;
        hdr[10] = 54; // pixel offset
        hdr[14] = 40; // info header size
        hdr[18] = W & 0xFF;
        hdr[19] = (W >> 8) & 0xFF;
        hdr[20] = (W >> 16) & 0xFF;
        hdr[21] = (W >> 24) & 0xFF;
        unsigned int negH = static_cast<unsigned int>(-H);
        hdr[22] = negH & 0xFF;
        hdr[23] = (negH >> 8) & 0xFF;
        hdr[24] = (negH >> 16) & 0xFF;
        hdr[25] = (negH >> 24) & 0xFF;
        hdr[26] = 1;  // planes
        hdr[28] = 24; // bpp
        fwrite(hdr, 1, sizeof(hdr), f);
        fwrite(bmpData.data(), 1, bmpData.size(), f);
        fclose(f);
    }
}

} // namespace nebula
