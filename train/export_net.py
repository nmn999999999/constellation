"""Export the detection-net weights to the binary format consumed by the C++
hybrid decoder (demo/decode_ml.cpp -> ParticleNet).

Only the detection subnet ("net.*") is exported: in the hybrid pipeline the
geometry comes from the classical GridCalibrator, so the STN localization
head is not needed at inference.

Usage:
    python export_net.py particle_detector.pt [out.bin]

The output file contains the tensors in the fixed order below (little-endian
float32, each preceded by an int32 element count):
    conv1 w/b, bn1 gamma/beta/mean/var,
    conv2 w/b, bn2 gamma/beta/mean/var,
    conv3 w/b, bn3 gamma/beta/mean/var,
    conv4 w/b, conv5 w/b
"""

import struct
import sys

import torch

# Order of Sequential indices in ParticleDetector.net:
#   0 conv3-32  1 bn  2 relu  3 pool
#   4 conv32-64 5 bn  6 relu  7 pool
#   8 conv64-128 9 bn 10 relu
#   11 conv128-64 12 relu
#   13 conv64-1 (1x1)
KEYS = [
    "net.0.weight", "net.0.bias",
    "net.1.weight", "net.1.bias",
    "net.1.running_mean", "net.1.running_var",
    "net.4.weight", "net.4.bias",
    "net.5.weight", "net.5.bias",
    "net.5.running_mean", "net.5.running_var",
    "net.8.weight", "net.8.bias",
    "net.9.weight", "net.9.bias",
    "net.9.running_mean", "net.9.running_var",
    "net.11.weight", "net.11.bias",
    "net.13.weight", "net.13.bias",
]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "particle_detector.pt"
    dst = sys.argv[2] if len(sys.argv) > 2 else "particle_detector.bin"

    sd = torch.load(src, map_location="cpu")
    if "model" in sd:  # full checkpoint dict
        sd = sd["model"]

    missing = [k for k in KEYS if k not in sd]
    if missing:
        print("ERROR: missing keys:", missing, file=sys.stderr)
        sys.exit(1)

    with open(dst, "wb") as f:
        f.write(b"PNET")
        total = 0
        for key in KEYS:
            t = sd[key].float().contiguous().numpy()
            f.write(struct.pack("<i", t.size))
            f.write(t.astype("<f4").tobytes())
            total += t.size
    print("wrote", dst, "tensors:", len(KEYS), "floats:", total)


if __name__ == "__main__":
    main()
