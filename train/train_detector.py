"""Train a spatial-transformer CNN that maps a particle-field photo to the
60x60 grid bitmap, robust to rotation/zoom/shift/compression.

Key design points (v2, 2026-08-12):
  * Canonical 480x480 renders are cached once; every batch is warped
    on-the-fly on the GPU (random rotation/zoom/shift + photometric noise),
    so each epoch sees fresh geometry.
  * The STN localization head is DIRECTLY supervised: the warp function
    returns the exact forward affine (the STN must apply the same M again to
    undo the warp), and the head is trained with MSE against it in addition
    to the detection BCE. It predicts the generative parameters
    (angle, log_scale, tx, ty) rather than raw matrix entries - far easier
    to regress. Without this the localization never learns (observed: stuck
    at 78.8% bit-acc / 0% decode).
  * Teacher-forcing warmup: the first --warmup-steps optimizer steps rectify
    with the TRUE affine (so the detection head learns on canonical images)
    while the loc head learns the affine from its MSE alone. Joint training
    only starts once both heads are sensible (joint BCE gradients otherwise
    overwhelm the affine signal and the model collapses to the marginal
    distribution).
  * Curriculum: the first --curriculum-epochs epochs use only +-1 degree
    rotation / +-3% zoom / +-4px shift, then the full +-3 deg / 0.85-1.15
    zoom / +-12px shift range.

Run on a cloud GPU (Kaggle / Lightning AI):

    python train_detector.py --epochs 40 --train-size 8000

Outputs particle_detector.pt (weights) and particle_detector.onnx.
"""

import argparse
import math
import os
import random
import sys

import numpy as np
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

SIZE = 480          # canonical render resolution
SCALE = 8           # px per grid cell
COLS = ROWS = 60
IMG_SIZE = 240      # model input (warped to this size)
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
    This is the ground truth the decoder consumes - the model must predict it
    from arbitrarily transformed images (implicit geometry calibration)."""
    label = np.zeros((ROWS, COLS), np.uint8)
    for x, y in pts:
        col = int(np.floor(x))
        row = int(np.floor(y))
        if 0 <= col < COLS and 0 <= row < ROWS:
            label[row, col] = 1
    return label


def build_dataset(codec, n, seed=0):
    """Canonical (unwarped) 480x480 renders + labels.

    Geometry is NOT baked in here anymore: each training batch is warped
    on-the-fly by warp_batch(), so the model sees fresh transforms every
    epoch and the curriculum can start with small rotations."""
    rng = random.Random(seed)
    images = np.empty((n, 3, SIZE, SIZE), np.uint8)
    labels = np.empty((n, ROWS, COLS), np.uint8)
    payloads = []
    for i in range(n):
        plen = rng.randint(1, 400)
        payload = bytes(rng.randrange(256) for _ in range(plen))
        frame = codec.encode(payload)[0]
        images[i] = render_field(frame).transpose(2, 0, 1)
        labels[i] = label_from_pts(frame)
        payloads.append(payload)
    return images, labels, payloads


def warp_batch(x, angle_range, device, scale_range=(0.85, 1.15), shift=12.0):
    """Warp a batch of canonical 480x480 images into 240x240 photos.

    Args:
        x: (B,3,480,480) uint8 canonical renders (CHW).
        angle_range: max rotation in degrees.
        device: torch device.
        scale_range: (lo, hi) random zoom factor.
        shift: max translation in 480-image pixels.

    Returns:
        photo: (B,3,240,240) float in [0,1].
        theta_target: (B,2,3) FORWARD warp matrix. The photo was built by
            sampling canon at inv(M)*u, so applying M again rectifies it:
            rect(u) = photo(M u) = canon(inv(M) M u) = canon(u).
            This is the supervision target for the STN loc head.
    """
    x = x.to(device).float().div_(255.0)
    b = x.shape[0]

    angle = np.random.uniform(-angle_range, angle_range, b)
    sc = np.random.uniform(scale_range[0], scale_range[1], b)
    tx = np.random.uniform(-shift, shift, b)
    ty = np.random.uniform(-shift, shift, b)

    # forward affine in normalized coords (uniform resize keeps coords same:
    # u_480 = u_240, so translation is tx/240 in both spaces)
    m_fwd = np.zeros((b, 3, 3), np.float32)
    m_fwd[:, 2, 2] = 1.0
    for i in range(b):
        rad = math.radians(angle[i])
        cs, sn = math.cos(rad), math.sin(rad)
        m_fwd[i, 0] = [sc[i] * cs, -sc[i] * sn, tx[i] / 240.0]
        m_fwd[i, 1] = [sc[i] * sn, sc[i] * cs, ty[i] / 240.0]
    theta_target = torch.from_numpy(m_fwd[:, :2]).float().to(device)

    grid = F.affine_grid(torch.from_numpy(
        np.linalg.inv(m_fwd)[:, :2]).float().to(device),
        (b, 3, IMG_SIZE, IMG_SIZE),
                         align_corners=False)
    photo = F.grid_sample(x, grid, align_corners=False)

    # mild photometric degradation: noise, random brightness
    photo = photo + torch.randn_like(photo) * 0.03
    photo = torch.clamp(photo, 0.0, 1.0)
    if torch.rand(1, device=device).item() < 0.5:
        photo = torch.clamp(
            photo * (0.75 + 0.25 * torch.rand(b, 1, 1, 1, device=device)),
            0.0, 1.0)
    return photo, theta_target


def rectify(photo, theta):
    """Sample `photo` with an affine rectification theta (output->input)."""
    grid = F.affine_grid(theta, photo.size(), align_corners=False)
    return F.grid_sample(photo, grid, align_corners=False)


def theta_from_params(p):
    """Build (N,2,3) affine matrices from (angle, log_scale, tx, ty)."""
    a, q, tx, ty = p[:, 0], p[:, 1], p[:, 2], p[:, 3]
    cs, sn = torch.cos(a), torch.sin(a)
    s = torch.exp(q)
    return torch.stack([s * cs, -s * sn, tx,
                        s * sn, s * cs, ty], dim=1).view(-1, 2, 3)


def params_from_theta(theta):
    """Inverse of theta_from_params: (N,2,3) -> (angle, log_scale, tx, ty)."""
    a = torch.atan2(theta[:, 1, 0], theta[:, 0, 0])
    s = torch.sqrt(theta[:, 0, 0] ** 2 + theta[:, 1, 0] ** 2)
    return torch.stack([a, torch.log(s),
                        theta[:, 0, 2], theta[:, 1, 2]], dim=1)


class CanonDataset(torch.utils.data.Dataset):
    """Holds canonical uint8 renders; warp_batch does the augmentation."""

    def __init__(self, images, labels):
        self.images = images
        self.labels = labels

    def __len__(self):
        return len(self.labels)

    def __getitem__(self, i):
        return torch.from_numpy(self.images[i]), torch.from_numpy(self.labels[i])


class ParticleDetector(nn.Module):
    """Spatial-transformer detector: a localization subnet predicts the affine
    transform that rectifies the photographed field, then a CNN maps the
    rectified image to the canonical 60x60 bitmap."""

    def __init__(self):
        super().__init__()
        # Localization: 240x240x3 -> 4 affine parameters
        # (angle, log_scale, tx, ty); init = identity transform.
        self.loc = nn.Sequential(
            nn.Conv2d(3, 32, 5, padding=2), nn.MaxPool2d(2), nn.ReLU(),
            nn.Conv2d(32, 64, 5, padding=2), nn.MaxPool2d(2), nn.ReLU(),
            nn.Conv2d(64, 128, 5, padding=2), nn.MaxPool2d(2), nn.ReLU(),
            nn.Conv2d(128, 128, 3, padding=1), nn.MaxPool2d(2), nn.ReLU(),
            nn.AdaptiveAvgPool2d(1),
        )
        self.fc_loc = nn.Linear(128, 4)
        self.fc_loc.weight.data.normal_(0, 0.01)
        self.fc_loc.bias.data.zero_()
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
        theta = theta_from_params(
            self.fc_loc(self.loc(x).view(x.size(0), -1)))
        return theta, self.net(rectify(x, theta))  # ((N,2,3), (N,1,60,60))


@torch.no_grad()
def decode_rate(codec, model, loader, device, angle_range, threshold=0.5,
                detector_only=False):
    model.eval()
    total = 0
    ok = 0
    for x, y in loader:
        if detector_only:
            # Canonical 480 -> 240, no geometry warp: GridCalibrator handles
            # geometry at inference, so the detector only ever sees clean
            # canonical images (warping here would add artificial black
            # borders at zoom-in, hurting edge cells).
            photo = F.interpolate(x.to(device).float().div_(255.0),
                                  size=(IMG_SIZE, IMG_SIZE),
                                  mode="bilinear", align_corners=False)
            logits = model.net(photo)
        else:
            photo, theta_target = warp_batch(x, angle_range, device)
            _, logits = model(photo)
        probs = torch.sigmoid(logits).squeeze(1).cpu().numpy()
        for i in range(probs.shape[0]):
            total += 1
            res = codec.decode_bitmap(probs[i] >= threshold)
            if res is not None and res[3]:
                ok += 1
    model.train()
    return ok / max(total, 1), ok, total


@torch.no_grad()
def bit_accuracy(model, loader, device, angle_range, detector_only=False):
    model.eval()
    correct = total = 0
    for x, y in loader:
        if detector_only:
            photo = F.interpolate(x.to(device).float().div_(255.0),
                                  size=(IMG_SIZE, IMG_SIZE),
                                  mode="bilinear", align_corners=False)
            logits = model.net(photo)
        else:
            photo, theta_target = warp_batch(x, angle_range, device)
            _, logits = model(photo)
        preds = (torch.sigmoid(logits) >= 0.5).float()
        correct += (preds == y.to(device).float().unsqueeze(1)).sum().item()
        total += y.numel()
    model.train()
    return correct / max(total, 1)


def load_or_build_dataset(codec, cache_x, cache_y, shape_x, shape_y, n, seed):
    """Load the cached canonical dataset, rebuilding once if missing/truncated/
    shape mismatch (a killed session can leave partial files)."""
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
    for path, arr in ((cache_x, x), (cache_y, y)):
        # np.save appends .npy unless the name already ends with it
        tmp = path + ".tmp"
        np.save(tmp, arr)
        os.replace(tmp + ".npy", path)
    print("cached dataset to", cache_x)
    return x, y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--train-size", type=int, default=3000)
    ap.add_argument("--val-size", type=int, default=500)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--curriculum-epochs", type=int, default=5,
                    help="first N epochs use small geometry only")
    ap.add_argument("--affine-w", type=float, default=10.0,
                    help="weight of the direct affine (STN) MSE loss")
    ap.add_argument("--warmup-steps", type=int, default=500,
                    help="first N optimizer steps teacher-force rectification "
                         "(loc head trains on MSE only, det head on canonical)")
    ap.add_argument("--detector-only", action="store_true",
                    help="train ONLY the detection subnet on teacher-forced "
                         "rectified images (no STN/localization). Use this "
                         "for the hybrid GridCalibrator + CNN decoder.")
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
    cache_x = args.out + ".canon_x.npy"
    cache_y = args.out + ".canon_y.npy"
    shape_x = (args.train_size, 3, SIZE, SIZE)
    shape_y = (args.train_size, ROWS, COLS)
    train_x, train_y = load_or_build_dataset(
        codec, cache_x, cache_y, shape_x, shape_y, args.train_size, seed=1)
    val_x, val_y, _ = build_dataset(codec, args.val_size, seed=2)

    train_ds = CanonDataset(train_x, train_y)
    val_ds = CanonDataset(val_x, val_y)
    train_loader = torch.utils.data.DataLoader(
        train_ds, batch_size=args.batch_size, shuffle=True, num_workers=0)
    val_loader = torch.utils.data.DataLoader(
        val_ds, batch_size=args.batch_size, shuffle=False, num_workers=0)

    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = ParticleDetector().to(device)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    loss_fn = nn.BCEWithLogitsLoss()
    affine_loss = nn.MSELoss()
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        opt, T_max=args.epochs)
    start_epoch = 0
    best = -1.0
    global_step = 0
    if args.resume and os.path.exists(args.checkpoint):
        ck = torch.load(args.checkpoint, map_location=device)
        model.load_state_dict(ck["model"])
        opt.load_state_dict(ck["opt"])
        start_epoch = ck["epoch"]
        best = ck.get("best", 0.0)
        global_step = ck.get("step", 0)
        print("resumed from epoch", start_epoch, "step", global_step,
              "best", best)

    full = (3.0, (0.85, 1.15), 12.0)
    easy = (1.0, (0.97, 1.03), 4.0)
    print("device:", device, "params:",
          sum(p.numel() for p in model.parameters()))
    if args.detector_only:
        for p in model.loc.parameters():
            p.requires_grad_(False)
        for p in model.fc_loc.parameters():
            p.requires_grad_(False)
    for epoch in range(start_epoch, args.epochs):
        if args.detector_only:
            angle_range, scale_range, shift = full
        else:
            angle_range, scale_range, shift = easy if epoch < args.curriculum_epochs else full
        model.train()
        total_bce = 0.0
        total_aff = 0.0
        for x, y in train_loader:
            y = y.to(device).float().unsqueeze(1)
            if args.detector_only:
                # Hybrid mode: geometry is handled by GridCalibrator at
                # inference, so the detection head only ever sees clean
                # canonical 240 images (no warp -> no artificial black
                # borders eating edge cells).
                photo = F.interpolate(x.to(device).float().div_(255.0),
                                      size=(IMG_SIZE, IMG_SIZE),
                                      mode="bilinear", align_corners=False)
                photo = torch.clamp(photo + torch.randn_like(photo) * 0.01,
                                    0.0, 1.0)
                logits = model.net(photo)
                loss = loss_fn(logits, y)
                bce = loss
                aff = torch.zeros((), device=device)
            else:
                photo, theta_target = warp_batch(x, angle_range, device,
                                                 scale_range, shift)
                params_pred = model.fc_loc(
                    model.loc(photo).view(photo.size(0), -1))
                theta_pred = theta_from_params(params_pred)
                if global_step < args.warmup_steps:
                    # Teacher forcing: rectify with the TRUE affine so the
                    # detection head learns on canonical images, while the
                    # loc head gets its direct MSE signal without BCE
                    # interference.
                    logits = model.net(rectify(photo, theta_target))
                else:
                    logits = model.net(rectify(photo, theta_pred))
                bce = loss_fn(logits, y)
                aff = affine_loss(params_pred,
                                  params_from_theta(theta_target))
                loss = bce + args.affine_w * aff
            opt.zero_grad()
            loss.backward()
            opt.step()
            global_step += 1
            total_bce += bce.item() * x.size(0)
            total_aff += aff.item() * x.size(0)
        scheduler.step()
        acc = bit_accuracy(model, val_loader, device, full[0],
                           args.detector_only)
        n_tr = len(train_ds)
        phase = "det" if args.detector_only else (
            "tf " if global_step < args.warmup_steps else "jit")
        if (epoch + 1) % 5 == 0 or epoch == start_epoch:
            rate, ok, tot = decode_rate(codec, model, val_loader, device,
                                        full[0], detector_only=args.detector_only)
            print("epoch %2d [%s +-%.0f deg] bce %.4f affine %.5f bit-acc %.4f "
                  "decode %.1f%% (%d/%d)" %
                  (epoch + 1, phase, angle_range,
                   total_bce / n_tr, total_aff / n_tr,
                   acc, rate * 100, ok, tot))
        else:
            print("epoch %2d [%s +-%.0f deg] bce %.4f affine %.5f bit-acc %.4f"
                  % (epoch + 1, phase, angle_range,
                     total_bce / n_tr, total_aff / n_tr, acc))
        if acc > best:
            best = acc
            torch.save(model.state_dict(), args.out)
            print("  saved", args.out)
        torch.save({"model": model.state_dict(), "opt": opt.state_dict(),
                    "epoch": epoch + 1, "step": global_step, "best": best},
                   args.checkpoint)

    model.load_state_dict(torch.load(args.out, map_location=device))
    model.eval()
    dummy = torch.randn(1, 3, IMG_SIZE, IMG_SIZE)
    try:
        torch.onnx.export(
            model, dummy, args.onnx, opset_version=20,
            input_names=["img"], output_names=["theta", "logits"],
            dynamo=False)
        print("exported", args.onnx)
    except Exception as e:  # pragma: no cover
        print("onnx export failed:", e)

    print("best bit-acc: %.4f" % best)


if __name__ == "__main__":
    main()
