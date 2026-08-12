# Constellation ML training (Kaggle)

Train a spatial-transformer CNN that decodes particle fields directly from
photographed images (rotated / zoomed / shifted), replacing the hand-written
detection + geometry-calibration pipeline with a learned equivalent.

## What the model does

Input: a 240x240 image of a particle field, arbitrarily transformed.
Output: the canonical 60x60 grid bitmap (which cell has a particle).

The localization subnet predicts the affine transform that rectifies the
photo; the detection subnet then maps the rectified image to the bitmap.
Training data is synthesized with the Python reimplementation of the C++
encoder (`particle_codec_py.py`), which is verified byte-for-byte against the
C++ library (see `consistency_check.py`), so whatever the model learns decodes
with the real codec.

## Run on Kaggle

1. Create a Notebook at kaggle.com (GPU accelerator: T4 x2 / P100).
2. In the first cell:

   ```python
   !git clone https://github.com/nmn999999999/constellation.git
   %cd constellation/train
   ```

3. Train:

   ```python
   !python train_detector.py --epochs 40 --train-size 8000 --val-size 1000 --batch-size 64
   ```

   - First run generates and caches the dataset (~10-15 min on CPU workers);
     later runs load the cache instantly.
   - A checkpoint is saved every epoch (`checkpoint_last.pt`), so if the
     session disconnects, rerun with `--resume` to continue.

4. Results: `particle_detector.pt` + `particle_detector.onnx`. Download them
   from the Notebook output panel.

## Expected behavior

- `bit-acc` (per-cell accuracy) should climb steadily; `decode` (full-frame
  CRC success) starts at 0% and only turns on once the geometry is learned.
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
