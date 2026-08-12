# Particle Codec 项目文档

## 项目概述

**Particle Codec** 是一个将二进制数据编码为粒子位置的隐写系统。通过固定种子生成确定性网格排列，将数据嵌入到看似随机的粒子场中。编码不依赖用户名/密钥，任何拿到图的人都能直接解码。

> 注：Flutter 桌面版本（`particle_codec_flutter/`）已于 2026-08-11 删除，仓库只保留 C++ 核心库与 CLI/Viewer。

---

## 项目结构

```
私有粒子/                              ★ 注意：路径含中文！MinGW/CMake 可直接构建，无需 junction
│
├── include/particle_codec/        # 公共头文件
│   ├── codec.h                    # ParticleCodec 主类
│   ├── error.h                    # ErrorCode/ErrorInfo/Result<T> 错误反馈
│   ├── frame_builder.h            # FrameHeader(syncWord=0xAA55AA55, totalSize=12), CRC32, FrameBuilder
│   ├── coordinate_encoder.h       # 坐标编码器
│   ├── mapping_restorer.h         # 解码恢复器
│   ├── grid_mapping.h             # 网格映射
│   ├── hamming.h                  # 汉明码 ECC
│   ├── pseudo_random.h            # 伪随机数（种子派生）
│   ├── perlin_noise.h             # Perlin 噪声（编码时添加 ±0.12 抖动）
│   └── frame_parser.h             # 帧解析器
├── src/                           # C++ 实现
│   ├── codec.cpp                  # 主类实现
│   ├── error.cpp                  # errorName() 实现
│   └── ...其他 .cpp
├── demo/
│   ├── viewer.cpp                 # Win32 粒子查看器（导出：480×480，scale=8，pr=2，gr=4）
│   ├── decode_image.cpp           # CLI 解码器（4 种检测方法：颜色flood-fill/DT/DT直缩/颜色直缩）
│   └── stb_image.h                # stb_image v2.30（PNG/JPG/BMP 加载）
├── tests/
│   ├── codec_test.cpp             # 15+ 单元测试
│   ├── test_error_safety.cpp      # 错误安全与错误反馈测试（非法参数/损坏帧诊断）
│   ├── test_decode_image.cpp      # 端到端编解码测试
│   ├── test_viewer_decode.cpp     # viewer 风格编解码测试（104/104 粒子）
│   └── test_viewer_export.cpp     # export 风格编解码测试（128/128 粒子）
├── CMakeLists.txt                 # CMake 配置（C++17）
├── AGENTS.md                      # ← 你正在看的这个
└── README.md
```

---

## 技术栈

| 组件     | 技术         | 版本                                     |
|----------|--------------|------------------------------------------|
| 核心库   | C++17        | MinGW 15.2（CLion 自带）+ CMake/Ninja    |
| GUI      | Win32 + GDI+ | demo/viewer.cpp（需 MSVC，未接入 CMake） |
| 图像加载 | stb_image.h  | v2.30（单头文件）                        |
| 图像处理 | C++ 手动实现 | flood-fill 聚类 + 距离变换               |

---

## 构建命令

### 前提：工具链（CLion 自带 MinGW）

本机没有 MSVC，使用 CLion 自带的 MinGW + CMake + Ninja：

- g++/gcc：`Z:\Program Files\JetBrains\CLion 2026.2.1\bin\mingw\bin`
- cmake：`Z:\Program Files\JetBrains\CLion 2026.2.1\bin\cmake\win\x64\bin`
- ninja：`Z:\Program Files\JetBrains\CLion 2026.2.1\bin\ninja\win\x64`
  中文路径可用，无需 junction。

### 构建（CMake，仓库根目录执行）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

构建产物为免 DLL 的单文件 exe：MinGW 运行时（libgcc/libstdc++/winpthread）静态链接，仅依赖系统 DLL（KERNEL32/msvcrt）。

### 测试（全部通过）

```bash
build\codec_test.exe          # 15/15 核心测试
build\error_safety_test.exe   # 错误安全/错误反馈测试
build\test_decode_image.exe   # 小 blob 端到端测试
build\test_viewer_decode.exe  # 104/104 粒子 viewer 测试
build\test_viewer_export.exe  # 生成 480×480 export 测试图
build\test_precision.exe      # 精度基准（召回率/解码成功率）
build\decode_image.exe        # CLI 解码器
# 使用：decode_image.exe image.png [grid_cols] [grid_rows]
```

---

## 核心机制

### 编码流程

1. 将数据分割为帧：每帧 = sync (4B 0xAA55AA55) + seq (2B) + len (2B) + payload + CRC32 (4B)
2. 通过固定种子生成确定性网格排列（permutation）
3. 将数据位映射为粒子坐标（gridToBitIndex → perm → 坐标）
4. 添加 Perlin 噪声 ±0.12 产生自然运动

### 解码流程

1. 检测图像中的青色粒子（阈值：R<80, G>100, B>100, B>R+20, G>R+20）
2. flood-fill 8-邻域聚类，计算质心（count ≥ 3 才保留）
3. 网格映射：`col = floor(gx)`，snap 到 `col + 0.5`（`floor` 而非 `round`！）
4. 逆向映射恢复数据位（`centerToGrid → floor → clamp → gridToBitIndex → invPerm`）
5. CRC32 验证帧完整性

### decode_image.cpp 的 4 种检测方法（按序尝试，成功即退出）

1. **颜色 flood-fill** + 质心 → 网格 snap（居中归一化）
2. **距离变换** 找局部极大 + NMS + 网格 snap（居中归一化）
3. **距离变换** + 直接缩放（不做居中，`gx = cx / scaleX`）
4. **颜色 flood-fill** + 直接缩放（同上）

---

## 关键事实（必须知道）

### 渲染参数（viewer 导出）

| 参数          | 值                            |
|---------------|-------------------------------|
| 导出尺寸      | 480×480                       |
| scale         | 8 (480/60)                    |
| 粒子半径 (pr) | `max(int(0.35*scale), 2)` = 2 |
| 辉光半径 (gr) | pr*2 = 4                      |
| 居中          | 无居中，直接 `px*scale`       |
| 抖动          | 无                            |

### decode_image.cpp 默认参数

- grid: 60×60

### 网格对齐

- **使用 `floor(gx)` 而非 `int(gx + 0.5)`** — 粒子在 col+0.5 处，必须 map 到 col，不是 col+1
- 所有 decode 方法（CLI）都用 `floor(gx)`
- 归一化公式（居中）：`gx = ((cx - minX) / rangeX) * (gc - 1) + 0.5`
- 归一化公式（直接）：`gx = pixelX * gridCols / imageWidth`

### 帧结构

```
[sync:4B][seq:2B][len:2B][payload:len][CRC32:4B]
syncWord = 0xAA55AA55
totalSize = 12
maxPayloadPerFrame ≈ (3600 - 微粒映射开销) 字节
```

---

## 常见陷阱与注意事项

### 1. 工具链说明

本机使用 CLion 自带 MinGW（无 MSVC），中文路径可用，无需 junction。 若换用 MSVC 环境，中文路径下仍需 junction 绕过。

### 2. 旧导出图片不可解码

旧版 viewer（pr=1.5 *scale=12）在 8px 网格上粒子核重叠，信息丢失不可逆。必须用新版 viewer（pr=0.35*scale=2）重新导出。

### 3. 测试图片路径

`particle_field.png` 在 `U:\Users\nmn99\Documents\` 是旧版导出（pr=12），不可解码。

---

## 开发规范

### 代码风格

- 优先使用现有模式和本地 API
- 使用结构化 API 而非字符串操作
- 保持模块边界清晰
- 测试覆盖率随风险和影响范围调整

### 沟通方式

- 长任务每 30 秒更新进度
- 最终答案简洁，避免冗长
- 文件引用使用绝对路径 (`file_path:line_number`)

### 如果被问到"做了什么"

请总结回看整个 conversation 中所有实质性工作（文件修改、bug 修复、功能添加），不要只提当前步骤。

---

## 后续待办

1. **移除 decode_image.cpp 的调试输出** — 验证精度稳定后清除 `std::cout`
