#include "particle_codec/pseudo_random.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cmath>
#include <cstring>

namespace particle_codec {
    PseudoRandom::PseudoRandom(const std::vector<uint8_t> &seed) {
        s0_ = wordFromSeed(seed, 0);
        s1_ = wordFromSeed(seed, 4);
        s2_ = wordFromSeed(seed, 8);
        s3_ = wordFromSeed(seed, 12);
        if ((s0_ | s1_ | s2_ | s3_) == 0) {
            s0_ = 1;
            s1_ = 0x6C62272E;
            s2_ = 0x07B2E512;
            s3_ = 0x4B5C4847;
        }
        for (int i = 0; i < 20; i++) nextUint32();
    }

    uint32_t PseudoRandom::wordFromSeed(const std::vector<uint8_t> &s, int off) {
        uint32_t v = 0;
        for (int i = 0; i < 4 && off + i < static_cast<int>(s.size()); i++)
            v = (v << 8) | s[off + i];
        return (v | 1) & 0xFFFFFFFF;
    }

    uint32_t PseudoRandom::nextUint32() {
        uint32_t t = s3_;
        uint32_t s = s0_;
        s3_ = s2_;
        s2_ = s1_;
        s1_ = s;
        t ^= t << 11;
        t ^= t >> 8;
        s0_ = (t ^ s ^ (s >> 19)) & 0xFFFFFFFF;
        return s0_;
    }

    std::vector<uint8_t> PseudoRandom::nextBytes(size_t count) {
        std::vector<uint8_t> result(count);
        for (size_t i = 0; i < count; i += 4) {
            uint32_t v = nextUint32();
            for (size_t j = 0; j < 4 && i + j < count; j++)
                result[i + j] = static_cast<uint8_t>((v >> (j * 8)) & 0xFF);
        }
        return result;
    }

    std::vector<int> PseudoRandom::permutation(int count) {
        std::vector<int> p(count);
        for (int i = 0; i < count; i++) p[i] = i;
        shuffle(p);
        return p;
    }

    double PseudoRandom::nextGaussian() {
        double u1 = nextDouble();
        double u2 = nextDouble();
        while (u1 == 0) u1 = nextDouble();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    }

    std::vector<int> PseudoRandom::inversePermutation(const std::vector<int> &perm) {
        std::vector<int> inv(perm.size());
        for (size_t i = 0; i < perm.size(); i++)
            inv[perm[i]] = static_cast<int>(i);
        return inv;
    }

    std::vector<uint8_t> PseudoRandom::deriveSeed(const std::string &input, const std::string &domain) {
        std::string combined = domain + ":" + input;
        uint32_t h1 = 0x811c9dc5, h2 = 0x01000193, h3 = 0xdeadbeef, h4 = 0xc0debabe;

        for (int round = 0; round < 8; round++) {
            for (char c: combined) {
                uint8_t b = static_cast<uint8_t>(c);
                h1 ^= b;
                h1 = (h1 * 0x01000193) & 0xFFFFFFFF;
                h2 ^= b;
                h2 = (h2 * 0x811c9dc5) & 0xFFFFFFFF;
                h3 ^= b ^ (round << 8);
                h3 = (h3 * 0x1b873593) & 0xFFFFFFFF;
                h4 ^= b ^ (round * 31);
                h4 = (h4 * 0xcc9e2d51) & 0xFFFFFFFF;
            }
            uint32_t tmp = h1;
            h1 = h2 ^ (h3 >> 13);
            h2 = h3 ^ (h1 << 7);
            h3 = h4 ^ (h2 >> 17);
            h4 = tmp ^ (h3 << 5);
        }

        std::vector<uint8_t> seed(32);
        auto put32 = [&](int off, uint32_t v) {
            seed[off] = (v >> 24) & 0xFF;
            seed[off + 1] = (v >> 16) & 0xFF;
            seed[off + 2] = (v >> 8) & 0xFF;
            seed[off + 3] = v & 0xFF;
        };
        put32(0, h1);
        put32(4, h2);
        put32(8, h3);
        put32(12, h4);

        for (int i = 16; i < 32; i++) {
            seed[i] = seed[i - 16] ^ seed[i - 12] ^ seed[i - 8] ^ seed[i - 4];
            seed[i] = ((seed[i] << 3) | (seed[i] >> 5)) ^ static_cast<uint8_t>(i * 0x9E3779B9);
        }
        return seed;
    }
} // namespace particle_codec

