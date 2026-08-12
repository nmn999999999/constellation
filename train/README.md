# Constellation ML training

Train a spatial-transformer CNN that decodes particle fields directly from
photographed images (rotated / zoomed / shifted), replacing the hand-written
detection + geometry-calibration pipeline with a learned equivalent.

## What the model does

Input: a 240x240 photo of a particle field, arbitrarily transformed.
Output: the canonical 60x60 grid bitmap (which cell has a particle).

The localization subnet predicts the affine transform that rectifies the
photo; the detection subnet then maps the rectified image to the bitmap.
Training data is synthesized with the Python reimplementation of the C++
encoder (`particle_codec_py.py`), which is verified byte-for-byte against the
C++ library (see `consistency_check.py`), so whatever the model learns decodes
with the real codec.

## Recipe (v2, 2026-08-12)

- Canonical 480x480 renders are cached once; every batch is warped on-the-fly
  on the GPU (rotation +-3 deg, zoom 0.85-1.15, shift +-12 px, noise,
  brightness), so each epoch sees fresh geometry.
- The STN localization head is **directly supervised**: it predicts the
  generative parameters (angle, log_scale, tx, ty) that undo the warp, with
  MSE against the true augmentation parameters on top of the detection BCE.
  Parametrizing instead of regressing raw matrix entries is essential
  (without direct supervision it never learned: 78.8% bit-acc / 0% decode
  after 10 epochs).
- **Teacher-forcing warmup**: the first `--warmup-steps` optimizer steps
  rectify with the TRUE affine, so the detection head learns on canonical
  images (98.7% bit-acc on unseen frames after only 150 training images)
  while the loc head learns the affine from its MSE alone. Joint training
  starts once both heads are sensible.
- Curriculum: first 5 epochs use only +-1 deg / +-3% zoom / +-4 px, then the
  full range (controlled by `--curriculum-epochs`).
- `--affine-w` weights the affine MSE (default 10.0).

## Run on Lightning AI (recommended, free)

1. Go to <https://lightning.ai/sign-up> and register with any email. The free
   plan does not need a credit card. Free monthly credits pay for GPU hours;
   an `.edu` / company email usually verifies instantly.
2. Create a Studio (blank), then upload these two files from `train/`:
   `train_detector.py` and `particle_codec_py.py` (drag & drop into the file
   panel, or use the terminal).
3. Open the Studio terminal and install deps:

   ```bash
   pip install torch numpy pillow
   ```

4. Start a GPU machine (menu on the right / top), then run:

   ```bash
   python train_detector.py --epochs 40 --train-size 8000 --val-size 1000 --batch-size 64
   ```

   First run builds + caches the dataset (~10-15 min), then trains on GPU.
   A checkpoint is saved every epoch (`checkpoint_last.pt`); if the session
   drops, rerun with `--resume`.
5. Download `particle_detector.pt` (weights) and `particle_detector.onnx`
   (inference model) when done.

## Run on Kaggle (alternative)

1. Create a Notebook (GPU accelerator: T4 x2 / P100).
2. First cell:

   ```python
   !git clone https://github.com/nmn999999999/constellation.git
   %cd constellation/train
   ```

3. Train:

   ```python
   !python train_detector.py --epochs 40 --train-size 8000 --val-size 1000 --batch-size 64
   ```

   The code writes caches to `/kaggle/working` (writable), not `/kaggle/input`.

## Run on Modal (free, no credit card)

Modal's no-card free tier is $5/month of compute credits (email or GitHub
signup, no phone, no real-name) - enough for ~25 T4-GPU-hours, and one full
training run costs well under $1.

```bash
pip install modal
python -m modal setup                        # browser sign-in (Windows-safe)
cd train
python -m modal run train_modal.py           # full 8000/40 run
# smaller run: python -m modal run train_modal.py -- --epochs 20 --train-size 3000
```

`particle_detector.pt` / `.onnx` / `.onnx.data` are returned and written into
`train/` automatically. No persistent volume is used, so there is no storage
bill - but a dropped session reruns from scratch (use Lightning AI for
resume). Always launch with `modal run` / `python -m modal run`; running the
script directly with `python` cannot hydrate the cloud app.

## Expected behavior

- `bce` (detection loss) and `affine` (STN geometry MSE) should both drop.
- `bit-acc` (per-cell accuracy) climbs steadily; `decode` (full-frame CRC
  success) starts at 0% and turns on once geometry is learned.
- With 8000 samples and 40 epochs the decode rate should reach >90% on the
  validation set, including rotated / zoomed inputs.

## Local smoke test

```bash
pip install torch --index-url https://download.pytorch.org/whl/cpu pillow numpy
python consistency_check.py            # verifies Python encoder == C++
python train_detector.py --epochs 2 --train-size 60 --val-size 20 --batch-size 8
```

## Integrating the model into C++

`particle_detector.onnx` is consumed by `onnxruntime` from the C++ codec: run
the model on an image to get the 60x60 bitmap, then feed the bitmap through
the existing inverse-permutation + CRC path (`ParticleCodec::decodeCentroidsRaw`
accepts a bitmap expressed as centroids). This integration is a follow-up.
