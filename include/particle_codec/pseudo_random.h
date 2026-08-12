#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace particle_codec {
    class PseudoRandom {
        uint32_t s0_, s1_, s2_, s3_;

    public:
        explicit PseudoRandom(const std::vector<uint8_t> &seed);

        uint32_t nextUint32();

        int nextInt(int max) { return (nextUint32() & 0x7FFFFFFF) % max; }
        double nextDouble() { return static_cast<double>(nextUint32()) / 0x100000000ULL; }

        std::vector<uint8_t> nextBytes(size_t count);

        template<typename T>
        void shuffle(std::vector<T> &list) {
            for (int i = static_cast<int>(list.size()) - 1; i > 0; i--) {
                int j = nextInt(i + 1);
                std::swap(list[i], list[j]);
            }
        }

        std::vector<int> permutation(int count);

        double nextGaussian();

        static std::vector<int> inversePermutation(const std::vector<int> &perm);

        // Generic seed derivation from two public inputs. The codec always
        // passes a fixed built-in value, so no user secret is involved.
        static std::vector<uint8_t> deriveSeed(const std::string &input,
                                               const std::string &domain = "particle_codec");

    private:
        static uint32_t wordFromSeed(const std::vector<uint8_t> &s, int off);
    };

    class PerlinNoise {
        std::vector<uint8_t> perm_;

    public:
        explicit PerlinNoise(const std::vector<uint8_t> &seed);

        double noise2D(double x, double y) const;

        double fbm(double x, double y, int octaves = 4, double lacunarity = 2.0, double gain = 0.5) const;
    };
} // namespace particle_codec
