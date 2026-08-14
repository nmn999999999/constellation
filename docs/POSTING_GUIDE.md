# 🚀 发布指南：HN + Reddit 帖子（复制粘贴即可）

> 使用方法：复制下方对应的帖子内容，粘贴到目标网站的发布框，点发布。
> 如果某个网站要注册账号，先注册（免费），发布前先阅读该社区规则。

---

## 一、Show HN（Hacker News）

**网址**：https://news.ycombinator.com/submit
（要先注册 HN 账号，或用 GitHub 登录）

### 标题（三选一，建议用 A）

- **A（推荐，最抓眼球）**：
  `Show HN: I hid a message inside a starfield – and anyone can decode it`

- **B（技术向）**：
  `Show HN: Deterministic steganography – encode binary data into a particle field, decode from any image`

- **C（极简）**：
  `Show HN: Constellation – data hidden in a starfield`

### 正文（复制粘贴到正文框）

```
I built a visual data encoding system: binary data becomes the positions of
particles in a starfield-like image. No key, no username – the grid layout is
derived from a fixed seed, so anyone who has the image can decode it.

Demo animation (every frame hides 900 bytes with CRC32 verification):
https://github.com/nmn999999999/constellation/blob/main/docs/particle_field_demo.gif

The image below hides a 900-byte message. Download the Windows decoder (single
.exe, no install) and try it yourself:
https://github.com/nmn999999999/constellation/releases/tag/v0.1.0

How it works:
- Data is split into frames: [sync][seq][len][payload][CRC32]
- Each data bit maps to a grid cell via a seeded permutation
- Cells with bit=1 get a cyan particle (+ glow + Perlin noise drift)
- Decoding: detect particles, inverse-map, verify CRC32

Robustness highlights (measured):
- Survives JPEG q10, blur radius 2, noise sigma 0.10
- GridCalibrator recovers rotation up to 30°, scale 0.5–1.35x, translation
- HomographyCalibrator handles perspective (keystone) distortion
- VP9/AV1 video at extreme compression (~60KB / 3.8s) still decodes fully
- Optional Hamming (7,4) ECC for noisy channels

Tech: C++17, CMake, stb_image. CI green on Linux GCC/Clang, macOS, Windows MSVC.
ML-assisted decoder included (small CNN, hand-written C++ inference, no external
runtime).

Repo: https://github.com/nmn999999999/constellation
```

### ⚠️ HN 发布规则（必看）
1. **标题必须以 `Show HN:` 开头**（A/B/C 都有）
2. 发布后 **前 2 小时最重要**：回复所有评论，HN 算法看互动率
3. 不要短时间重复发（HN 有重复检测）
4. 最佳发布时间：**美东时间 9:00–11:00**（约北京时间 21:00–23:00，或次日凌晨）
5. 账号如果是新建的，可以先在 HN 上回复几个无关帖子攒 karma（新账号发帖门槛低但权重低）

---

## 二、Reddit

**通用流程**：
1. 注册账号：https://www.reddit.com/register/
2. 在对应子版块发帖
3. 发完在评论区回复问题（互动率影响排名）

### 帖子 1：r/steganography（隐写术专业社区，最对口）

**网址**：https://www.reddit.com/r/steganography/submit

**标题**：
`[Project] Encoded a 900-byte message into a starfield – no key, decode from any image or video`

**正文**：
```
I built a steganography system where the carrier IS the message: binary data
becomes particle positions in a starfield. Deterministic seed → anyone can
decode, no key exchange needed.

The image below hides 900 bytes (CRC32 verified). Try decoding it:
[Windows decoder download](https://github.com/nmn999999999/constellation/releases/tag/v0.1.0)
```
decode_image_win64.exe sample_hidden_message.png 60 60
```
[Demo GIF](https://github.com/nmn999999999/constellation/blob/main/docs/particle_field_demo.gif)

Robustness measured: JPEG q10, blur 2px, noise 0.10 sigma, rotation 30° via
calibration, perspective via homography, VP9/AV1 extreme compression.

C++17, single-file no-DLL Windows build, CI on 4 platforms.
https://github.com/nmn999999999/constellation
```

### 帖子 2：r/programming（大众编程社区，流量大）

**网址**：https://www.reddit.com/r/programming/submit

**标题**：
`I encoded binary data into a starfield – deterministic steganography in C++`

**正文**：
```
Interesting little project I've been working on: data → particle positions in
a starfield image. The permutation is fixed-seed, so decoding requires no key.

Try it: this image hides a 900-byte message.
[GitHub repo](https://github.com/nmn999999999/constellation)
[Releases (Windows exe)](https://github.com/nmn999999999/constellation/releases/tag/v0.1.0)
[Demo animation](https://github.com/nmn999999999/constellation/blob/main/docs/particle_field_demo.gif)

What makes it different from QR codes: the carrier is a natural-looking
starfield, not a machine-readable pattern. Robustness is surprisingly good:
- JPEG down to q10, blur radius 2, noise σ0.10
- Rotation up to 30° (affine calibration), perspective (homography)
- Video: VP9/AV1 at ~60KB for 3.8s still decodes 100%

Tech: C++17, CMake, stb_image, no external deps. Tests green on GCC/Clang/MSVC.
```

### 帖子 3：r/coolgithubprojects（专门展示 GitHub 项目）

**网址**：https://www.reddit.com/r/coolgithubprojects/submit

**标题**：
`Constellation – encode data into a starfield, decode from any image (no key)`

**正文**：
```
[GitHub](https://github.com/nmn999999999/constellation) | [Windows demo exe](https://github.com/nmn999999999/constellation/releases/tag/v0.1.0)

A C++ library that turns binary data into a moving starfield. Deterministic
seed mapping means anyone can decode – the image itself carries the message.

Includes: image/video decoding, rotation/scale/perspective calibration,
ML-assisted detection, and a 4-platform CI.
```

---

## 三、发布顺序建议

| 顺序 | 平台 | 目的 |
|------|------|------|
| 1 | r/steganography | 对口社区，先拿专业反馈 |
| 2 | Show HN | 最大流量来源（若上了首页） |
| 3 | r/programming | 补充流量 |
| 4 | r/coolgithubprojects | 长尾引流 |

**注意**：HN 和 Reddit 不要在同一分钟内发，间隔 1-2 小时，避免看起来像 spam。

---

## 四、发完之后 2 小时内要做的

1. **每 10-20 分钟刷新一次**，回复所有评论（哪怕只是"谢谢"）
2. 评论区有人提问就回答，这是排名算法最看重的
3. 如果被问"和 QR 码有什么区别"→ 答：QR 是显式机器码，这个是视觉载体本身
4. 如果被问"安全性"→ 诚实说：这是趣味/隐蔽传输实验，不是加密（数据本身建议先加密再编码）
5. 别删帖、别改标题（会被降权）

---

## 五、如果被删帖/被喷怎么办

- **被删**：通常是因为子版块规则（比如 r/programming 不允许纯项目展示），换 r/coolgithubprojects 或 r/golang/r/rust 等语言社区再发
- **被喷**：常见质疑是"这算什么创新"。回应思路：
  - "QR 码是显式可读的，这个对肉眼是随机星场"
  - "隐写 ≠ 加密，这个项目定位是隐蔽载体 + 鲁棒解码"
  - "鲁棒性数据是真的，欢迎跑测试"

---

## 六、注册/登录链接汇总

| 平台 | 注册/登录 |
|------|-----------|
| Hacker News | https://news.ycombinator.com/login （注册即用，新号即可发帖） |
| Reddit | https://www.reddit.com/register/ |
| r/steganography | https://www.reddit.com/r/steganography/ |
| r/programming | https://www.reddit.com/r/programming/ |
| r/coolgithubprojects | https://www.reddit.com/r/coolgithubprojects/ |
