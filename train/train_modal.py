"""Run the particle-detector training on Modal's free cloud GPUs.

Why Modal for this project:
  * Email or GitHub signup - no credit card, no phone, no real-name.
  * Free tier (no card) is $5/month of compute credits, enough for ~25
    T4-GPU-hours. One full training run costs well under $1.

Setup (one time, locally):
    pip install modal
    modal setup          # opens browser; sign in with email or GitHub

Run (from the train/ directory):
    modal run train_modal.py                            # full 8000/40
    modal run train_modal.py -- --epochs 20 --train-size 3000

When the run finishes, particle_detector.pt / .onnx / .onnx.data are
written next to this script automatically (they are returned from the cloud
run, so no persistent volume - and no storage bill - is needed).

Note: no persistent disk means a dropped session has to rerun from scratch
(dataset rebuild ~10-15 min is part of the run). Use Lightning AI instead if
you need checkpoint resume.
"""

import argparse
import os

import modal

# T4 is ~$0.19/h on Modal; the no-card free tier is $5/month.
GPU = "T4"
TIMEOUT = 4 * 3600  # dataset build + training; bump if you raise --epochs

image = (
    modal.Image.debian_slim()
    .pip_install("torch", "numpy", "pillow")
)

app = modal.App("constellation-particle-train")


@app.function(image=image, gpu=GPU, cpu=4, memory=16384, timeout=TIMEOUT)
def train(epochs: int, train_size: int, val_size: int, batch_size: int,
          warmup_steps: int, affine_w: float):
    import sys

    import train_detector

    sys.argv = [
        "train_detector.py",
        "--epochs", str(epochs),
        "--train-size", str(train_size),
        "--val-size", str(val_size),
        "--batch-size", str(batch_size),
        "--warmup-steps", str(warmup_steps),
        "--affine-w", str(affine_w),
        "--out", "/tmp/particle_detector.pt",
        "--onnx", "/tmp/particle_detector.onnx",
        "--checkpoint", "/tmp/checkpoint_last.pt",
    ]
    train_detector.main()

    artifacts = {}
    for name in ("particle_detector.pt", "particle_detector.onnx",
                 "particle_detector.onnx.data"):
        path = os.path.join("/tmp", name)
        if os.path.exists(path):
            with open(path, "rb") as f:
                artifacts[name] = f.read()
    return artifacts


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--train-size", type=int, default=8000)
    ap.add_argument("--val-size", type=int, default=1000)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--warmup-steps", type=int, default=500)
    ap.add_argument("--affine-w", type=float, default=10.0)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    artifacts = train.remote(
        epochs=args.epochs,
        train_size=args.train_size,
        val_size=args.val_size,
        batch_size=args.batch_size,
        warmup_steps=args.warmup_steps,
        affine_w=args.affine_w,
    )
    for name, data in artifacts.items():
        path = os.path.join(here, name)
        with open(path, "wb") as f:
            f.write(data)
        print("saved", path)
