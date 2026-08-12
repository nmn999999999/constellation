"""Python reimplementation of the C++ ParticleCodec encoder.

The deterministic mapping (seed derivation, permutation, Perlin noise,
frame layout) must match the C++ library exactly so that training data
generated here decodes with the real codec. Used by the Kaggle training
script to synthesize particle fields with ground-truth grid labels.
"""

import math
import zlib

MASK32 = 0xFFFFFFFF


def _next_uint32_state(s):
    t = s[3]
    x = s[0]
    s[3] = s[2]
    s[2] = s[1]
    s[1] = x
    t = (t ^ ((t << 11) & MASK32)) & MASK32
    t = (t ^ (t >> 8)) & MASK32
    s[0] = (t ^ x ^ (x >> 19)) & MASK32
    return s[0]


class PseudoRandom:
    """xorshift128 with 20 warm-up rounds (matches C++)."""

    def __init__(self, seed: bytes):
        def word(off):
            v = 0
            for i in range(4):
                if off + i < len(seed):
                    v = ((v << 8) | seed[off + i]) & MASK32
            return (v | 1) & MASK32

        self.s = [word(0), word(4), word(8), word(12)]
        if (self.s[0] | self.s[1] | self.s[2] | self.s[3]) == 0:
            self.s = [1, 0x6C62272E, 0x07B2E512, 0x4B5C4847]
        for _ in range(20):
            _next_uint32_state(self.s)

    def next_uint32(self):
        return _next_uint32_state(self.s)

    def next_int(self, maxv):
        return (self.next_uint32() & 0x7FFFFFFF) % maxv

    def shuffle(self, lst):
        for i in range(len(lst) - 1, 0, -1):
            j = self.next_int(i + 1)
            lst[i], lst[j] = lst[j], lst[i]

    def permutation(self, count):
        p = list(range(count))
        self.shuffle(p)
        return p


def derive_seed(input_str, domain="particle_codec"):
    """8-round mixed hash producing a 32-byte seed (matches C++)."""
    combined = domain + ":" + input_str
    h = [0x811C9DC5, 0x01000193, 0xDEADBEEF, 0xC0DEBABE]
    for rnd in range(8):
        for b in combined.encode():
            h[0] = ((h[0] ^ b) * 0x01000193) & MASK32
            h[1] = ((h[1] ^ b) * 0x811C9DC5) & MASK32
            h[2] = ((h[2] ^ (b ^ (rnd << 8))) * 0x1B873593) & MASK32
            h[3] = ((h[3] ^ (b ^ (rnd * 31))) * 0xCC9E2D51) & MASK32
        tmp = h[0]
        h[0] = (h[1] ^ (h[2] >> 13)) & MASK32
        h[1] = (h[2] ^ ((h[0] << 7) & MASK32)) & MASK32
        h[2] = (h[3] ^ ((h[1] >> 17) & MASK32)) & MASK32
        h[3] = (tmp ^ ((h[2] << 5) & MASK32)) & MASK32

    seed = bytearray(32)
    for i, v in enumerate(h):
        seed[i * 4] = (v >> 24) & 0xFF
        seed[i * 4 + 1] = (v >> 16) & 0xFF
        seed[i * 4 + 2] = (v >> 8) & 0xFF
        seed[i * 4 + 3] = v & 0xFF
    for i in range(16, 32):
        seed[i] = seed[i - 16] ^ seed[i - 12] ^ seed[i - 8] ^ seed[i - 4]
        seed[i] = (((seed[i] << 3) | (seed[i] >> 5)) & 0xFF) ^ (
            (i * 0x9E3779B9) & 0xFF)
    return bytes(seed)


def _fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)


def _lerp(t, a, b):
    return a + t * (b - a)


def _grad(h, x, y):
    h &= 3
    u = x if h < 2 else y
    v = y if h < 2 else x
    return (u if (h & 1) == 0 else -u) + (v if (h & 2) == 0 else -v)


class PerlinNoise:
    def __init__(self, seed: bytes):
        p = list(range(256))
        rng = PseudoRandom(seed)
        rng.shuffle(p)
        self.perm = p + p

    def noise2d(self, x, y):
        xi = int(math.floor(x)) & 255
        yi = int(math.floor(y)) & 255
        xf = x - math.floor(x)
        yf = y - math.floor(y)
        u = _fade(xf)
        v = _fade(yf)
        aa = self.perm[self.perm[xi] + yi]
        ab = self.perm[self.perm[xi] + yi + 1]
        ba = self.perm[self.perm[xi + 1] + yi]
        bb = self.perm[self.perm[xi + 1] + yi + 1]
        return _lerp(v,
                     _lerp(u, _grad(aa, xf, yf), _grad(ba, xf - 1, yf)),
                     _lerp(u, _grad(ab, xf, yf - 1), _grad(bb, xf - 1, yf - 1)))

    def fbm(self, x, y, octaves=3, lacunarity=2.0, gain=0.5):
        total = 0.0
        amp = 1.0
        freq = 1.0
        max_amp = 0.0
        for _ in range(octaves):
            total += self.noise2d(x * freq, y * freq) * amp
            max_amp += amp
            amp *= gain
            freq *= lacunarity
        return total / max_amp if max_amp else 0.0


class ParticleCodecPy:
    """Deterministic encoder: data bytes -> list of particle (x, y) lists."""

    SYNC = b"\xaa\x55\xaa\x55"
    HEADER_SIZE = 12

    def __init__(self, domain="particle_codec", cols=60, rows=60):
        self.domain = domain
        self.cols = cols
        self.rows = rows
        self.seed = derive_seed("demo_user", domain)
        rng = PseudoRandom(self.seed)
        self.perm = rng.permutation(cols * rows)
        self.inv = [0] * (cols * rows)
        for i, p in enumerate(self.perm):
            self.inv[p] = i
        noise_seed = bytes(b ^ 0x55 for b in self.seed[:32])
        self.noise = PerlinNoise(noise_seed)

    def max_payload_bytes(self):
        return (self.cols * self.rows - self.HEADER_SIZE * 8) // 8

    def build_frame(self, seq, payload):
        header = (self.SYNC + bytes([(seq >> 8) & 0xFF, seq & 0xFF,
                                     (len(payload) >> 8) & 0xFF,
                                     len(payload) & 0xFF]))
        crc = zlib.crc32(header + payload) & MASK32
        return header + crc.to_bytes(4, "big") + payload

    def frame_particles(self, frame: bytes, t=0.0):
        """Returns particle (x, y) list in unit-grid coordinates, with the
        same Perlin drift as the C++ encoder."""
        pts = []
        total_cells = self.cols * self.rows
        for i, byte in enumerate(frame):
            for j in range(8):
                bit_idx = i * 8 + j
                if bit_idx >= total_cells:
                    break
                if (byte >> (7 - j)) & 1:
                    si = self.perm[bit_idx]
                    col, row = si % self.cols, si // self.cols
                    cx, cy = col + 0.5, row + 0.5
                    nx = self.noise.fbm(col * 0.1 + t * 0.3, row * 0.1, 3)
                    ny = self.noise.fbm(col * 0.1, row * 0.1 + t * 0.3, 3)
                    pts.append((cx + nx * 0.12, cy + ny * 0.12))
        return pts

    def encode(self, data: bytes, t=0.0):
        chunk = self.max_payload_bytes()
        chunks = [data[i:i + chunk] for i in range(0, len(data), chunk)]
        if not chunks:
            chunks = [b""]
        return [self.frame_particles(self.build_frame(seq, ch), t)
                for seq, ch in enumerate(chunks)]

    def decode_bitmap(self, bitmap):
        """Converts a 60x60 particle bitmap back to frame bytes (no CRC
        verification). Returns None if the sync word is missing."""
        nbytes = (self.cols * self.rows + 7) // 8
        out = bytearray(nbytes)
        for row in range(self.rows):
            for col in range(self.cols):
                if bitmap[row][col]:
                    si = row * self.cols + col
                    bi = self.inv[si]
                    out[bi // 8] |= 1 << (7 - (bi % 8))
        if out[:4] != self.SYNC:
            return None
        seq = (out[4] << 8) | out[5]
        ln = (out[6] << 8) | out[7]
        total = self.HEADER_SIZE + ln
        if total > len(out):
            return None
        fb = bytes(out[:total])
        crc_stored = int.from_bytes(fb[8:12], "big")
        crc_calc = zlib.crc32(fb[:8] + fb[12:]) & MASK32
        return fb, seq, ln, crc_calc == crc_stored
