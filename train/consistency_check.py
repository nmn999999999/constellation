"""Verifies the Python reimplementation matches the C++ encoder.

Generates particle coordinates for a known payload; the C++ tool
decode_coords.cpp decodes them and compares against the original text.
"""

import sys
sys.path.insert(0, ".")

from particle_codec_py import ParticleCodecPy  # noqa: E402


def main():
    message = b"consistency check 12345"
    codec = ParticleCodecPy()
    frames = codec.encode(message)
    with open("coords.txt", "w") as f:
        f.write("%d\n" % len(frames))
        for frame in frames:
            f.write("%d\n" % len(frame))
            for x, y in frame:
                f.write("%.6f %.6f\n" % (x, y))
    print("wrote %d frame(s), %d particles" % (len(frames), len(frames[0])))
    print("SEED " + codec.seed.hex())
    print("PERM" + "".join(" %d" % p for p in codec.perm[:30]))
    print("NOISE" + "".join(
        " %.8f" % codec.noise.fbm((i % 4) * 0.1, (i / 4) * 0.1, 3)
        for i in range(8)))


if __name__ == "__main__":
    main()
