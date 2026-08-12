#include "particle_codec/particle_net.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace particle_codec {
namespace {

constexpr float kBnEps = 1e-5f;

// 3x3 / 1x1 convolution with optional BatchNorm + ReLU, then optional 2x2
// max pooling (stride 2). Planar (C,H,W) layout throughout.
void convForward(const float *in, int h, int w, int cin,
                 const float *wgt, const float *bias, int cout, int k,
                 bool relu, bool pool,
                 const float *bnGamma = nullptr, const float *bnBeta = nullptr,
                 const float *bnMean = nullptr, const float *bnVar = nullptr,
                 float *out = nullptr, int *outH = nullptr,
                 int *outW = nullptr) {
    const int pad = k / 2;
    const int oh = pool ? h / 2 : h;
    const int ow = pool ? w / 2 : w;
    std::vector<float> tmp(static_cast<size_t>(cout) * h * w);

    for (int oc = 0; oc < cout; oc++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float acc = bias[oc];
                for (int ic = 0; ic < cin; ic++) {
                    // channel base; `yy`/`xx` below are absolute rows/cols
                    const float *inp = in + static_cast<size_t>(ic) * h * w;
                    const float *wp = &wgt[(static_cast<size_t>(oc) * cin + ic) * k * k];
                    for (int ky = 0; ky < k; ky++) {
                        int yy = y + ky - pad;
                        if (yy < 0 || yy >= h) continue;
                        for (int kx = 0; kx < k; kx++) {
                            int xx = x + kx - pad;
                            if (xx < 0 || xx >= w) continue;
                            acc += inp[yy * w + xx] * wp[ky * k + kx];
                        }
                    }
                }
                if (bnGamma) {
                    float scale = bnGamma[oc] / std::sqrt(bnVar[oc] + kBnEps);
                    acc = (acc - bnMean[oc]) * scale + bnBeta[oc];
                }
                float v = relu ? (acc > 0 ? acc : 0.0f) : acc;
            tmp[(static_cast<size_t>(oc) * h + y) * w + x] = v;
        }
    }
    }

    if (!pool) {
        std::memcpy(out, tmp.data(), tmp.size() * sizeof(float));
        *outH = h;
        *outW = w;
        return;
    }
    for (int oc = 0; oc < cout; oc++) {
        for (int y = 0; y < oh; y++) {
            for (int x = 0; x < ow; x++) {
                float m = -1e30f;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        float v = tmp[(static_cast<size_t>(oc) * h + (y * 2 + dy)) * w +
                                      (x * 2 + dx)];
                        if (v > m) m = v;
                    }
                }
                out[(static_cast<size_t>(oc) * oh + y) * ow + x] = m;
            }
        }
    }
    *outH = oh;
    *outW = ow;
}

} // namespace

bool ParticleNet::readTensor(FILE *f, std::vector<float> &out) {
    int32_t n = 0;
    if (std::fread(&n, sizeof(n), 1, f) != 1 || n <= 0 || n > 10 * 1000 * 1000)
        return false;
    out.resize(static_cast<size_t>(n));
    return std::fread(out.data(), sizeof(float), static_cast<size_t>(n), f) ==
           static_cast<size_t>(n);
}

bool ParticleNet::load(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "PNET", 4) == 0;

    struct ConvSpec {
        Conv *c;
    };
    ok = ok && readTensor(f, c1_.w) && readTensor(f, c1_.b);
    ok = ok && readTensor(f, bn1_.gamma) && readTensor(f, bn1_.beta) &&
         readTensor(f, bn1_.mean) && readTensor(f, bn1_.var);
    ok = ok && readTensor(f, c2_.w) && readTensor(f, c2_.b);
    ok = ok && readTensor(f, bn2_.gamma) && readTensor(f, bn2_.beta) &&
         readTensor(f, bn2_.mean) && readTensor(f, bn2_.var);
    ok = ok && readTensor(f, c3_.w) && readTensor(f, c3_.b);
    ok = ok && readTensor(f, bn3_.gamma) && readTensor(f, bn3_.beta) &&
         readTensor(f, bn3_.mean) && readTensor(f, bn3_.var);
    ok = ok && readTensor(f, c4_.w) && readTensor(f, c4_.b);
    ok = ok && readTensor(f, c5_.w) && readTensor(f, c5_.b);
    std::fclose(f);

    if (!ok) return false;

    c1_.cin = 3;
    c1_.cout = 32;
    c1_.k = 3;
    c2_.cin = 32;
    c2_.cout = 64;
    c2_.k = 3;
    c3_.cin = 64;
    c3_.cout = 128;
    c3_.k = 3;
    c4_.cin = 128;
    c4_.cout = 64;
    c4_.k = 3;
    c5_.cin = 64;
    c5_.cout = 1;
    c5_.k = 1;

    loaded_ = true;
    return true;
}

void ParticleNet::detect(const float *rgb, float *logits) const {
    // Convert interleaved RGB to planar (C,H,W).
    const int h0 = kInputSize, w0 = kInputSize;
    std::vector<float> in(static_cast<size_t>(3) * h0 * w0);
    for (int y = 0; y < h0; y++) {
        for (int x = 0; x < w0; x++) {
            const float *p = rgb + (static_cast<size_t>(y) * w0 + x) * 3;
            in[0 * h0 * w0 + y * w0 + x] = p[0];
            in[1 * h0 * w0 + y * w0 + x] = p[1];
            in[2 * h0 * w0 + y * w0 + x] = p[2];
        }
    }

    int h, w;
    std::vector<float> a1(static_cast<size_t>(32) * 120 * 120);
    convForward(in.data(), h0, w0, c1_.cin, c1_.w.data(), c1_.b.data(),
                c1_.cout, c1_.k, true, true,
                bn1_.gamma.data(), bn1_.beta.data(), bn1_.mean.data(),
                bn1_.var.data(), a1.data(), &h, &w);

    std::vector<float> a2(static_cast<size_t>(64) * 60 * 60);
    convForward(a1.data(), h, w, c2_.cin, c2_.w.data(), c2_.b.data(),
                c2_.cout, c2_.k, true, true,
                bn2_.gamma.data(), bn2_.beta.data(), bn2_.mean.data(),
                bn2_.var.data(), a2.data(), &h, &w);

    std::vector<float> a3(static_cast<size_t>(128) * 60 * 60);
    convForward(a2.data(), h, w, c3_.cin, c3_.w.data(), c3_.b.data(),
                c3_.cout, c3_.k, true, false,
                bn3_.gamma.data(), bn3_.beta.data(), bn3_.mean.data(),
                bn3_.var.data(), a3.data(), &h, &w);

    std::vector<float> a4(static_cast<size_t>(64) * 60 * 60);
    convForward(a3.data(), h, w, c4_.cin, c4_.w.data(), c4_.b.data(),
                c4_.cout, c4_.k, true, false,
                nullptr, nullptr, nullptr, nullptr, a4.data(), &h, &w);

    std::vector<float> a5(static_cast<size_t>(1) * 60 * 60);
    convForward(a4.data(), h, w, c5_.cin, c5_.w.data(), c5_.b.data(),
                c5_.cout, c5_.k, false, false,
                nullptr, nullptr, nullptr, nullptr, a5.data(), &h, &w);

    std::memcpy(logits, a5.data(), a5.size() * sizeof(float));
}

} // namespace particle_codec
