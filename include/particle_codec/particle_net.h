#pragma once

#include <string>
#include <vector>

namespace particle_codec {

    // Hand-written inference for the detection subnet of the trained
    // particle-field CNN (see train/train_detector.py).
    //
    //   conv3-32 + BN + ReLU + MaxPool2
    //   conv3-64 + BN + ReLU + MaxPool2
    //   conv3-128 + BN + ReLU
    //   conv3-64 + ReLU
    //   conv1-1
    //
    // Input: 240x240x3 RGB floats in [0,1] (interleaved), canonical grid
    // image. Output: 60x60 logits (row-major); sigmoid >= 0.5 marks a cell
    // as containing a particle. The weights come from
    // train/export_net.py -> particle_detector.bin.
    //
    // The localization/STN head is intentionally not implemented: in the
    // hybrid decoder the geometry comes from GridCalibrator, so the model
    // only has to map an already-rectified image to the canonical bitmap.
    class ParticleNet {
    public:
        static constexpr int kInputSize = 240;   // px
        static constexpr int kGrid = 60;         // cells

        ParticleNet() = default;

        // Loads weights from a .bin file written by train/export_net.py.
        // Returns false on I/O errors or a malformed file.
        bool load(const std::string &path);

        bool loaded() const { return loaded_; }

        // Runs the detection subnet. `rgb` is 240*240*3 interleaved floats
        // in [0,1]; `logits` receives 60*60 row-major logits.
        void detect(const float *rgb, float *logits) const;

    private:
        struct Conv {
            int cin = 0, cout = 0, k = 0;
            std::vector<float> w; // (cout, cin, k, k)
            std::vector<float> b; // (cout)
        };
        struct BatchNorm {
            std::vector<float> gamma, beta, mean, var;
        };

        Conv c1_, c2_, c3_, c4_, c5_;
        BatchNorm bn1_, bn2_, bn3_;
        bool loaded_ = false;

        bool readTensor(FILE *f, std::vector<float> &out);
    };

} // namespace particle_codec
