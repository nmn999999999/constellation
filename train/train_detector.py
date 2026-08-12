"""Train a spatial-transformer CNN that maps a particle-field image to the
60x60 grid bitmap, robust to rotation/zoom/shift/compression. Run on Kaggle:

    python train_detector.py --epochs 40 --train-size 8000

Outputs particle_detector.pt (weights) and particle_detector.onnx.
"""

import argparse
import math
import os
import random
import sys

import numpy as np
from PIL import Image
import torch
import torch.nn as nn
import torch.nn.functional as F

try:  # Windows console may choke on UTF-8 progress output from torch
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from particle_codec_py import ParticleCodecPy  # noqa: E402

SIZE = 480          # render resolution
SCALE = 8           # px per grid cell
COLS = ROWS = 60
IMG_SIZE = 240      # model input (render resized down)
BG = (10, 10, 26)
GLOW = (0, 100, 130)
CORE = (0, 188, 212)


def render_field(pts):
    """480x480 RGB numpy image, viewer-export style (glow + core)."""
    img = np.full((SIZE, SIZE, 3), BG, np.uint8)
    for x, y in pts:
        sx = int(round(x * SCALE))
        sy = int(round(y * SCALE))
        x0, y0 = sx - 4, sy - 4
        xs = slice(max(x0, 0), min(x0 + 9, SIZE))
        ys = slice(max(y0, 0), min(y0 + 9, SIZE))
        patch = img[ys, xs]
        if patch.size == 0:
            continue
        pyy, pxx = np.mgrid[:patch.shape[0], :patch.shape[1]]
        d2 = (pxx - (sx - x0)) ** 2 + (pyy - (sy - y0)) ** 2
        patch[d2 <= 16] = GLOW        # glow radius 4
        patch[d2 <= 4] = CORE         # core radius 2
    return img


def label_from_pts(pts):
    """Canonical 60x60 bitmap: cell (col,row) is 1 iff a particle sits there.
    This is the ground truth the decoder consumes — the model must predict it
    from arbitrarily transformed images (implicit geometry calibration)."""
    label = np.zeros((ROWS, COLS), np.uint8)
    for x, y in pts:
        col = int(np.floor(x))
        row = int(np.floor(y))
        if 0 <= col < COLS and 0 <= row < ROWS:
            label[row, col] = 1
    return label


def augment(img):
    """Random rotation/scale/translation applied to the image only; the label
    stays the canonical bitmap so the model learns to undo the geometry."""
    angle = np.random.uniform(-3.0, 3.0)
    sc = np.random.uniform(0.85, 1.15)
    tx, ty = np.random.uniform(-12, 12), np.random.uniform(-12, 12)

    cx = cy = SIZE / 2.0
    rad = math.radians(angle)
    cs, sn = math.cos(rad), math.sin(rad)
    # forward transform: center-rotate/scale then translate
    M = np.array([
        [sc * cs, -sc * sn, cx - sc * cs * cx + sc * sn * cy + tx],
        [sc * sn, sc * cs, cy - sc * sn * cx - sc * cs * cy + ty],
        [0, 0, 1],
    ])

    # image (PIL affine takes the inverse mapping)
    inv = np.linalg.inv(M)[:2].flatten()
    pil = Image.fromarray(img).transform(
        (SIZE, SIZE), Image.AFFINE, tuple(inv.tolist()),
        resample=Image.BILINEAR, fillcolor=BG)
    pil = pil.resize((IMG_SIZE, IMG_SIZE), Image.LANCZOS)

    # mild photometric degradation: blur, noise, brightness, jpeg-like noise
    arr = np.asarray(pil, np.float32) / 255.0
    arr += np.random.normal(0, 0.03, arr.shape)
    arr = np.clip(arr, 0, 1)
    if np.random.random() < 0.5:
        arr = np.clip(arr * np.random.uniform(0.75, 1.0), 0, 1)
    return arr


class ParticleDetector(nn.Module):
    """Spatial-transformer detector: a localization subnet predicts the affine
    transform that rectifies the photographed field, then a CNN maps the
    rectified image to the canonical 60x60 bitmap."""

    def __init__(self):
        super().__init__()
        # Localization: 240x240x3 -> 6 affine parameters (identity init).
        self.loc = nn.Sequential(
            nn.Conv2d(3, 16, 5, padding=2), nn.MaxPool2d(2), nn.ReLU(),
            nn.Conv2d(16, 32, 5, padding=2), nn.MaxPool2d(2), nn.ReLU(),
            nn.Conv2d(32, 64, 5, padding=2), nn.MaxPool2d(2), nn.ReLU(),
            nn.AdaptiveAvgPool2d(1),
        )
        self.fc_loc = nn.Linear(64, 6)
        self.fc_loc.weight.data.zero_()
        self.fc_loc.bias.data.copy_(torch.tensor([1, 0, 0, 0, 1, 0],
                                                 dtype=torch.float))
        # Detection: rectified 240x240x3 -> 60x60 logits.
        self.net = nn.Sequential(
            nn.Conv2d(3, 32, 3, padding=1), nn.BatchNorm2d(32), nn.ReLU(),
            nn.MaxPool2d(2),
            nn.Conv2d(32, 64, 3, padding=1), nn.BatchNorm2d(64), nn.ReLU(),
            nn.MaxPool2d(2),
            nn.Conv2d(64, 128, 3, padding=1), nn.BatchNorm2d(128), nn.ReLU(),
            nn.Conv2d(128, 64, 3, padding=1), nn.ReLU(),
            nn.Conv2d(64, 1, 1),
        )

    def forward(self, x):
        theta = self.fc_loc(self.loc(x).view(x.size(0), -1)).view(-1, 2, 3)
        grid = F.affine_grid(theta, x.size(), align_corners=False)
        rect = F.grid_sample(x, grid, align_corners=False)
        return self.net(rect)  # (N,1,60,60)


def build_dataset(codec, n, seed=0):
    rng = random.Random(seed)
    images = np.empty((n, 3, IMG_SIZE, IMG_SIZE), np.float32)
    labels = np.empty((n, ROWS, COLS), np.uint8)
    payloads = []
    for i in range(n):
        plen = rng.randint(1, 400)
        payload = bytes(rng.randrange(256) for _ in range(plen))
        frames = codec.encode(payload)
        frame = frames[0]
        img = render_field(frame)
        arr = augment(img)
        images[i] = arr.transpose(2, 0, 1)
        labels[i] = label_from_pts(frame)
        payloads.append(payload)
    return torch.from_numpy(images), torch.from_numpy(labels), payloads


def decode_rate(codec, model, loader, device, threshold=0.5):
    model.eval()
    total = 0
    ok = 0
    with torch.no_grad():
        for x, y in loader:
            logits = model(x.to(device))
            probs = torch.sigmoid(logits).squeeze(1).numpy()
            for i in range(probs.shape[0]):
                total += 1
                res = codec.decode_bitmap(probs[i] >= threshold)
                if res is not None and res[3]:
                    ok += 1
    model.train()
    return ok / max(total, 1), ok, total


def bit_accuracy(model, loader, device):
    model.eval()
    correct = total = 0
    with torch.no_grad():
        for x, y in loader:
            logits = model(x.to(device))
            preds = (torch.sigmoid(logits) >= 0.5).float()
            correct += (preds == y.to(device).float().unsqueeze(1)).sum().item()
            total += y.numel()
    model.train()
    return correct / max(total, 1)


def load_or_build_dataset(codec, cache_x, cache_y, shape_x, shape_y, n, seed):
    """Load the cached dataset, rebuilding once if missing/truncated/shape
    mismatch (a killed session can leave partial files)."""
    if os.path.exists(cache_x) and os.path.exists(cache_y):
        try:
            x = np.load(cache_x)
            y = np.load(cache_y)
            if x.shape == shape_x and y.shape == shape_y:
                print("loading cached dataset")
                return x, y
            print("cache shape mismatch, rebuilding")
        except Exception:
            print("cache unreadable, rebuilding")
    print("building dataset (may take ~10 min)...")
    x, y, _ = build_dataset(codec, n, seed)
    x = x.numpy()
    y = y.numpy()
    for path, arr in ((cache_x, x), (cache_y, y)):
        np.save(path + ".tmp", arr)
        os.replace(path + ".tmp", path)
    print("cached dataset to", cache_x)
    return x, y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--train-size", type=int, default=3000)
    ap.add_argument("--val-size", type=int, default=500)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--out", default="particle_detector.pt")
    ap.add_argument("--onnx", default="particle_detector.onnx")
    ap.add_argument("--checkpoint", default="checkpoint_last.pt")
    ap.add_argument("--resume", action="store_true")
    args = ap.parse_args()

    torch.manual_seed(0)
    np.random.seed(0)
    random.seed(0)

    print("building dataset...")
    codec = ParticleCodecPy()
    cache_x = args.out + ".train_x.npy"
    cache_y = args.out + ".train_y.npy"
    shape_x = (args.train_size, 3, IMG_SIZE, IMG_SIZE)
    shape_y = (args.train_size, ROWS, COLS)
    train_x, train_y = load_or_build_dataset(
        codec, cache_x, cache_y, shape_x, shape_y, args.train_size, seed=1)
    train_x = torch.from_numpy(train_x)
    train_y = torch.from_numpy(train_y)
    val_x, val_y, _ = build_dataset(codec, args.val_size, seed=2)

    train_ds = torch.utils.data.TensorDataset(train_x, train_y)
    val_ds = torch.utils.data.TensorDataset(val_x, val_y)
    train_loader = torch.utils.data.DataLoader(
        train_ds, batch_size=args.batch_size, shuffle=True, num_workers=0)
    val_loader = torch.utils.data.DataLoader(
        val_ds, batch_size=args.batch_size, shuffle=False, num_workers=0)

    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = ParticleDetector().to(device)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    loss_fn = nn.BCEWithLogitsLoss()
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        opt, T_max=args.epochs)
    start_epoch = 0
    best = 0.0
    if args.resume and os.path.exists(args.checkpoint):
        ck = torch.load(args.checkpoint, map_location=device)
        model.load_state_dict(ck["model"])
        opt.load_state_dict(ck["opt"])
        start_epoch = ck["epoch"]
        best = ck.get("best", 0.0)
        print("resumed from epoch", start_epoch, "best", best)

    print("device:", device, "params:", sum(p.numel() for p in model.parameters()))
    for epoch in range(start_epoch, args.epochs):
        model.train()
        total_loss = 0.0
        for x, y in train_loader:
            x = x.to(device)
            y = y.to(device).float().unsqueeze(1)
            opt.zero_grad()
            loss = loss_fn(model(x), y)
            loss.backward()
            opt.step()
            total_loss += loss.item() * x.size(0)
        scheduler.step()
        acc = bit_accuracy(model, val_loader, device)
        if (epoch + 1) % 5 == 0 or epoch == start_epoch:
            rate, ok, tot = decode_rate(codec, model, val_loader, device)
            print("epoch %2d loss %.4f  bit-acc %.4f  decode %.1f%% (%d/%d)" %
                  (epoch + 1, total_loss / len(train_ds), acc,
                   rate * 100, ok, tot))
        else:
            print("epoch %2d loss %.4f  bit-acc %.4f" %
                  (epoch + 1, total_loss / len(train_ds), acc))
        if acc > best or epoch == start_epoch:
            best = acc
            torch.save(model.state_dict(), args.out)
            print("  saved", args.out)
        torch.save({"model": model.state_dict(), "opt": opt.state_dict(),
                    "epoch": epoch + 1, "best": best}, args.checkpoint)

    model.load_state_dict(torch.load(args.out))
    model.eval()
    dummy = torch.randn(1, 3, IMG_SIZE, IMG_SIZE)
    try:
        torch.onnx.export(
            model, dummy, args.onnx, opset_version=12,
            input_names=["img"], output_names=["logits"],
            dynamic_axes={"img": {0: "batch"}, "logits": {0: "batch"}})
        print("exported", args.onnx)
    except Exception as e:  # pragma: no cover
        print("onnx export failed:", e)

    print("best decode rate: %.1f%%" % (best * 100))


if __name__ == "__main__":
    main()
