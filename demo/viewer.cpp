#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <ole2.h>
#include <gdiplus.h>
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER (WM_USER + 1)
#endif
#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <queue>
#include "particle_codec/codec.h"

using namespace particle_codec;
#pragma comment(lib, "gdiplus.lib")

namespace {
    // === Constants ===
    constexpr int kTimerId = 1;
    constexpr int kTimerInterval = 16;
    constexpr double kMotionAmp = 0.12;
    constexpr int kToolbarH = 36;
    constexpr int kStatusH = 26;
    constexpr int kMenuH = 0; // we draw menu ourselves

    constexpr COLORREF kBgColor = RGB(10, 10, 26);
    constexpr COLORREF kParticleColor = RGB(0, 188, 212);
    constexpr COLORREF kGlowColor = RGB(0, 100, 130);
    constexpr COLORREF kDetectedColor = RGB(255, 80, 80);
    constexpr COLORREF kToolbarBg = RGB(18, 18, 36);
    constexpr COLORREF kStatusBg = RGB(0, 0, 0);
    constexpr COLORREF kTextColor = RGB(200, 200, 200);
    constexpr COLORREF kAccentColor = RGB(0, 188, 212);

    enum class AppMode { Encode, Scan };

    struct Contour {
        int label;
        double cx, cy;
        int area;
        int minX, maxX, minY, maxY;
    };

    // === Application State ===
    struct AppState {
        HWND hwnd = nullptr;
        HDC backDc = nullptr;
        HBITMAP backBmp = nullptr;
        int width = 1000;
        int height = 720;
        unsigned long long lastTick = 0;
        double time = 0.0;
        int frameCount = 0;
        bool animating = true;

        // Codec
        ParticleCodec *codec = nullptr;
        std::vector<std::pair<double, double> > particles;
        std::vector<std::vector<std::pair<double, double> > > frameParticles;
        int gridCols = 60;
        int gridRows = 60;
        int currentFrame = 0;
        int totalFrames = 0;
        int totalParticles = 0;
        std::string inputText;
        std::string decodedText;
        bool useEcc = false;
        bool roundtripSuccess = false;

        // Mode & UI
        AppMode mode = AppMode::Encode;
        char textInput[256] = "Hello Particle Codec!";
        char statusText[512] = "";
        bool showGrid = false;
        bool showScanOverlay = false;

        // Scan mode
        Gdiplus::Bitmap *loadedImage = nullptr;
        std::vector<std::pair<double, double> > detectedCentroids;
        std::vector<Contour> detectedContours;
        std::string scanResult;
        int scanImageWidth = 0;
        int scanImageHeight = 0;
        bool scanDecoded = false;
        char scanStatus[256] = "";

        // Menu
        HMENU menuBar = nullptr;
        int menuHeight = 0;

        // Windows controls
        HWND editHwnd = nullptr;
        HWND btnEncodeMode = nullptr;
        HWND btnScanMode = nullptr;
        HWND btnEncode = nullptr;
        HWND btnEcc = nullptr;
        HWND btnGrid = nullptr;
        HWND btnSave = nullptr;
        HWND btnPlay = nullptr;
        HWND btnOpenImage = nullptr;
        HWND btnScanNow = nullptr;

        // Control visibility
        bool showingEncodeControls = true;

        // Static labels
        HWND labelText = nullptr;

        // Brush for dark control backgrounds
        HBRUSH hbrDark = nullptr;
    };

    AppState g_s;

    // === GDI+ Helpers ===
    static ULONG_PTR g_gdiplusToken = 0;

    static void InitGdiplus() {
        Gdiplus::GdiplusStartupInput inp;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &inp, nullptr);
    }

    static void ShutdownGdiplus() {
        if (g_gdiplusToken)
            Gdiplus::GdiplusShutdown(g_gdiplusToken);
    }

    static Gdiplus::Bitmap *LoadImageFile(const wchar_t *path) {
        return Gdiplus::Bitmap::FromFile(path);
    }

    // === Contour Detection ===
    // Finds connected components in `gray` (foreground = value < threshold).
    // Visited pixels are marked in place (gray = 255), so no separate
    // label buffer is needed. `gray` is modified.
    static std::vector<Contour> FindContours(std::vector<uint8_t> &gray, int w, int h, int threshold = 80) {
        int size = w * h;
        std::vector<Contour> contours;
        int nextLabel = 1;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int idx = y * w + x;
                if (gray[idx] >= threshold) continue;

                // Flood fill (stack-based, no per-pixel queue allocation)
                std::vector<int> stack;
                stack.push_back(idx);
                gray[idx] = 255;
                int minX = x, maxX = x, minY = y, maxY = y;
                double sumX = 0, sumY = 0;
                int count = 0;

                while (!stack.empty()) {
                    int cur = stack.back();
                    stack.pop_back();
                    int cx = cur % w;
                    int cy = cur / w;
                    sumX += cx;
                    sumY += cy;
                    count++;
                    minX = std::min(minX, cx);
                    maxX = std::max(maxX, cx);
                    minY = std::min(minY, cy);
                    maxY = std::max(maxY, cy);

                    // 4-connected neighbors
                    int dirs[] = {-1, 0, 1, 0, 0, -1, 0, 1};
                    for (int d = 0; d < 4; d++) {
                        int nx = cx + dirs[d * 2];
                        int ny = cy + dirs[d * 2 + 1];
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                        int nidx = ny * w + nx;
                        if (gray[nidx] < threshold) {
                            gray[nidx] = 255;
                            stack.push_back(nidx);
                        }
                    }
                }

                if (count >= 3 && count <= 800) {
                    contours.push_back({nextLabel, sumX / count, sumY / count, count, minX, maxX, minY, maxY});
                }
                nextLabel++;
            }
        }
        return contours;
    }

    static void DetectParticlesFromImage(Gdiplus::Bitmap *bmp) {
        if (!bmp) return;
        int w = bmp->GetWidth();
        int h = bmp->GetHeight();
        if (w <= 0 || h <= 0) return;

        // Lock bits
        Gdiplus::Rect rect(0, 0, w, h);
        Gdiplus::BitmapData data;
        bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data);

        std::vector<uint8_t> gray(w * h);
        for (int y = 0; y < h; y++) {
            auto *row = static_cast<uint8_t *>(data.Scan0) + y * data.Stride;
            for (int x = 0; x < w; x++) {
                uint8_t b = row[x * 4];
                uint8_t g = row[x * 4 + 1];
                uint8_t r = row[x * 4 + 2];
                gray[y * w + x] = static_cast<uint8_t>(0.2126 * r + 0.7152 * g + 0.0722 * b);
            }
        }
        bmp->UnlockBits(&data);

        // Otsu threshold
        int hist[256] = {};
        for (int v: gray) hist[v]++;
        int total = w * h;
        double sum = 0;
        for (int i = 0; i < 256; i++) sum += i * hist[i];
        double sumB = 0, wB = 0, wF = 0;
        double maxV = 0;
        int threshold = 80;
        for (int i = 0; i < 256; i++) {
            wB += hist[i];
            if (wB == 0) continue;
            wF = total - wB;
            if (wF == 0) break;
            sumB += i * hist[i];
            double mB = sumB / wB;
            double mF = (sum - sumB) / wF;
            double between = wB * wF * (mB - mF) * (mB - mF);
            if (between > maxV) {
                maxV = between;
                threshold = i;
            }
        }

        auto contours = FindContours(gray, w, h, threshold);

        // Grid mapping: use same scale/offset as DrawCanvas
        g_s.detectedContours = std::move(contours);
        g_s.detectedCentroids.clear();
        g_s.scanImageWidth = w;
        g_s.scanImageHeight = h;

        {
            double scaleX = static_cast<double>(w) / g_s.gridCols;
            double scaleY = static_cast<double>(h) / g_s.gridRows;
            double scale = std::min(scaleX, scaleY);
            double ox = (w - g_s.gridCols * scale) * 0.5;
            double oy = (h - g_s.gridRows * scale) * 0.5;

            for (auto &c: g_s.detectedContours) {
                double gx = (c.cx - ox) / scale;
                double gy = (c.cy - oy) / scale;
                int col = static_cast<int>(gx + 0.5);
                int row = static_cast<int>(gy + 0.5);
                col = std::clamp(col, 0, g_s.gridCols - 1);
                row = std::clamp(row, 0, g_s.gridRows - 1);
                g_s.detectedCentroids.emplace_back(col + 0.5, row + 0.5);
            }
        }

        // Decode
        if (g_s.codec && !g_s.detectedCentroids.empty()) {
            auto result = g_s.codec->decodeCentroidsPayload(g_s.detectedCentroids);
            if (result) {
                g_s.scanResult = std::string(result->begin(), result->end());
                g_s.scanDecoded = true;
                // Show detected particles in canvas
                g_s.particles = g_s.detectedCentroids;
                g_s.totalParticles = (int) g_s.detectedCentroids.size();
                g_s.totalFrames = 1;
                g_s.currentFrame = 0;
                snprintf(g_s.statusText, sizeof(g_s.statusText),
                         "Scan OK: decoded \"%s\" (%zu bytes, %zu particles)",
                         g_s.scanResult.c_str(), result->size(), g_s.detectedCentroids.size());
                // Popup result
                std::string msg = "Decoded: \"" + g_s.scanResult + "\"\n\n"
                                  + std::to_string(g_s.detectedCentroids.size()) + " particles detected";
                MessageBoxA(g_s.hwnd, msg.c_str(), "Scan Result", MB_OK | MB_ICONINFORMATION);
            } else {
                g_s.scanResult.clear();
                g_s.scanDecoded = false;
                snprintf(g_s.statusText, sizeof(g_s.statusText),
                         "Scan FAILED: %zu contours found, but decode failed (wrong user/grid/ECC?)",
                         g_s.detectedCentroids.size());
                MessageBoxA(g_s.hwnd, g_s.statusText, "Scan Failed", MB_OK | MB_ICONWARNING);
            }
        }
    }

    // === Back Buffer ===
    static void CreateBackBuffer(HWND hwnd, int w, int h) {
        if (g_s.backDc) {
            SelectObject(g_s.backDc, g_s.backBmp);
            DeleteDC(g_s.backDc);
            DeleteObject(g_s.backBmp);
        }
        HDC dc = GetDC(hwnd);
        g_s.backDc = CreateCompatibleDC(dc);
        g_s.backBmp = CreateCompatibleBitmap(dc, w, h);
        SelectObject(g_s.backDc, g_s.backBmp);
        ReleaseDC(hwnd, dc);
        g_s.width = w;
        g_s.height = h;
    }

    // === Drawing ===
    static void DrawRoundedRect(HDC dc, int x, int y, int w, int h, int r, COLORREF fill, COLORREF border) {
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        auto oldBrush = SelectObject(dc, brush);
        auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, x, y, x + w, y + h, r * 2, r * 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    static HFONT CreateDefaultFont(int size = 13, bool bold = false) {
        return CreateFont(size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    }

    static void DrawToolbar() {
        HDC dc = g_s.backDc;
        int w = g_s.width;
        int y = 0;
        int th = kToolbarH;

        RECT bar = {0, y, w, y + th};
        HBRUSH bg = CreateSolidBrush(kToolbarBg);
        FillRect(dc, &bar, bg);
        DeleteObject(bg);

        HPEN line = CreatePen(PS_SOLID, 1, RGB(30, 40, 50));
        SelectObject(dc, line);
        MoveToEx(dc, 0, y + th, nullptr);
        LineTo(dc, w, y + th);
        DeleteObject(line);
    }

    static void DrawStatusBar() {
        HDC dc = g_s.backDc;
        int w = g_s.width, h = g_s.height;

        RECT bar = {0, h - kStatusH, w, h};
        HBRUSH bg = CreateSolidBrush(kStatusBg);
        FillRect(dc, &bar, bg);
        DeleteObject(bg);

        HPEN line = CreatePen(PS_SOLID, 1, RGB(30, 30, 50));
        SelectObject(dc, line);
        MoveToEx(dc, 0, h - kStatusH, nullptr);
        LineTo(dc, w, h - kStatusH);
        DeleteObject(line);

        auto oldFont = SelectObject(dc, CreateDefaultFont(12));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(140, 140, 160));

        RECT tr = {8, h - kStatusH, w - 8, h};
        DrawTextA(dc, g_s.statusText, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(dc, oldFont);
        DeleteObject(oldFont);
    }

    static void DrawCanvas() {
        HDC dc = g_s.backDc;
        int w = g_s.width, h = g_s.height;
        int topY = g_s.menuHeight + kToolbarH;
        int canvasH = h - topY - kStatusH;
        if (canvasH <= 0) return;

        // Background
        HBRUSH bgBrush = CreateSolidBrush(kBgColor);
        RECT rect = {0, topY, w, topY + canvasH};
        FillRect(dc, &rect, bgBrush);
        DeleteObject(bgBrush);

        // No particles? Show placeholder
        if (g_s.particles.empty()) {
            auto oldFont = SelectObject(dc, CreateDefaultFont(14));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(60, 60, 80));
            RECT tr = {0, topY, w, topY + canvasH};
            const char *msg = g_s.mode == AppMode::Encode
                                  ? "Enter text and click Encode"
                                  : "Open an image and scan for particles";
            DrawTextA(dc, msg, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            DeleteObject(oldFont);
            return;
        }

        double scaleX = static_cast<double>(w) / g_s.gridCols;
        double scaleY = static_cast<double>(canvasH) / g_s.gridRows;
        double scale = std::min(scaleX, scaleY);
        double ox = (w - g_s.gridCols * scale) * 0.5;
        double oy = topY + (canvasH - g_s.gridRows * scale) * 0.5;
        if (scale <= 0) return;

        // Grid lines
        if (g_s.showGrid) {
            HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(20, 30, 40));
            SelectObject(dc, gridPen);
            for (int x = 0; x <= g_s.gridCols; x += 10) {
                int sx = static_cast<int>(ox + x * scale);
                MoveToEx(dc, sx, static_cast<int>(oy), nullptr);
                LineTo(dc, sx, static_cast<int>(oy + g_s.gridRows * scale));
            }
            for (int y = 0; y <= g_s.gridRows; y += 10) {
                int sy = static_cast<int>(oy + y * scale);
                MoveToEx(dc, static_cast<int>(ox), sy, nullptr);
                LineTo(dc, static_cast<int>(ox + g_s.gridCols * scale), sy);
            }
            DeleteObject(gridPen);
        }

        if (g_s.mode == AppMode::Scan && g_s.showScanOverlay && !g_s.detectedContours.empty()) {
            // Draw detected contours as red boxes
            auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            HPEN detPen = CreatePen(PS_SOLID, 2, RGB(255, 80, 80));
            SelectObject(dc, detPen);
            double cw = static_cast<double>(g_s.scanImageWidth) / g_s.gridCols;
            double ch = static_cast<double>(g_s.scanImageHeight) / g_s.gridRows;
            for (auto &c: g_s.detectedContours) {
                int col = static_cast<int>(c.cx / cw);
                int row = static_cast<int>(c.cy / ch);
                col = std::clamp(col, 0, g_s.gridCols - 1);
                row = std::clamp(row, 0, g_s.gridRows - 1);
                double sx = ox + (col + 0.1) * scale;
                double sy = oy + (row + 0.1) * scale;
                double sw = scale * 0.8;
                Rectangle(dc, static_cast<int>(sx), static_cast<int>(sy),
                          static_cast<int>(sx + sw), static_cast<int>(sy + sw));
            }
            SelectObject(dc, oldBrush);
            DeleteObject(detPen);
        }

        // Particles
        auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));

        double time = g_s.time;
        for (const auto &[px, py]: g_s.particles) {
            double dx = kMotionAmp * sin(px * 0.17 + py * 0.31 + time * 1.8);
            double dy = kMotionAmp * cos(px * 0.29 + py * 0.13 + time * 2.1);

            double sx = ox + (px + dx) * scale;
            double sy = oy + (py + dy) * scale;

            int gr = std::max(static_cast<int>(2.5 * scale), 2);
            SetDCBrushColor(dc, kGlowColor);
            Ellipse(dc, sx - gr, sy - gr, sx + gr, sy + gr);

            int pr = std::max(static_cast<int>(1.2 * scale), 1);
            SetDCBrushColor(dc, kParticleColor);
            Ellipse(dc, sx - pr, sy - pr, sx + pr, sy + pr);
        }

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);

        // Scan result banner
        if (g_s.mode == AppMode::Scan && !g_s.scanResult.empty()) {
            int bannerH = 52;
            RECT banner = {0, topY, w, topY + bannerH};
            HBRUSH bbg = CreateSolidBrush(RGB(0, 40, 50));
            FillRect(dc, &banner, bbg);
            DeleteObject(bbg);

            auto oldFont = SelectObject(dc, CreateDefaultFont(16, true));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0, 220, 240));
            std::string display = " Decoded: \"" + g_s.scanResult + "\"";
            RECT tr2 = {8, topY, w - 8, topY + bannerH};
            DrawTextA(dc, display.c_str(), -1, &tr2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, oldFont);
            DeleteObject(oldFont);
        }

        // Frame navigation overlay
        if (g_s.totalFrames > 1) {
            auto oldFont = SelectObject(dc, CreateDefaultFont(12));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(200, 200, 200));
            char buf[64];
            snprintf(buf, sizeof(buf), "Frame %d / %d", g_s.currentFrame + 1, g_s.totalFrames);
            RECT fr = {12, topY + canvasH - 30, 200, topY + canvasH};
            DrawTextA(dc, buf, -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            DeleteObject(oldFont);
        }
    }

    static void RenderFrame() {
        DrawCanvas();
        DrawToolbar();
        DrawStatusBar();
    }

    // === Encode ===
    static void DoEncode() {
        if (!g_s.codec) return;
        std::string input(g_s.textInput);
        if (input.empty()) {
            snprintf(g_s.statusText, sizeof(g_s.statusText), "Input is empty");
            return;
        }

        std::vector<uint8_t> data(input.begin(), input.end());
        auto frames = g_s.useEcc ? g_s.codec->encodeWithEcc(data) : g_s.codec->encode(data);
        if (frames.empty()) {
            snprintf(g_s.statusText, sizeof(g_s.statusText), "Encoding failed");
            return;
        }

        g_s.frameParticles.clear();
        int total = 0;
        for (auto &frame: frames) {
            std::vector<std::pair<double, double> > pts;
            for (int i = 0; i < frame.particleCount; i++) {
                pts.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);
            }
            g_s.frameParticles.push_back(std::move(pts));
            total += frame.particleCount;
        }

        g_s.currentFrame = 0;
        g_s.totalFrames = static_cast<int>(frames.size());
        g_s.particles = g_s.frameParticles[0];
        g_s.totalParticles = static_cast<int>(g_s.particles.size());

        // Verify roundtrip
        g_s.roundtripSuccess = true;
        std::string decoded;
        for (auto &frame: frames) {
            std::vector<std::pair<double, double> > centroids;
            for (int i = 0; i < frame.particleCount; i++)
                centroids.emplace_back(frame.particles[i * 2], frame.particles[i * 2 + 1]);
            auto result = g_s.codec->decodeCentroidsPayload(centroids);
            if (!result) {
                g_s.roundtripSuccess = false;
                break;
            }
            decoded += std::string(result->begin(), result->end());
        }
        g_s.decodedText = decoded;

        if (g_s.totalFrames > 1) {
            snprintf(g_s.statusText, sizeof(g_s.statusText),
                     "Encoded %d frames, %d particles | Roundtrip: %s | Decoded: \"%s\"",
                     g_s.totalFrames, g_s.totalParticles,
                     g_s.roundtripSuccess ? "PASS" : "FAIL",
                     g_s.decodedText.c_str());
        } else {
            snprintf(g_s.statusText, sizeof(g_s.statusText),
                     "%d particles | Roundtrip: %s | Decoded: \"%s\"",
                     g_s.totalParticles,
                     g_s.roundtripSuccess ? "PASS" : "FAIL",
                     g_s.decodedText.c_str());
        }
    }

    // === Edit box subclass (forward keyboard shortcuts to parent) ===
    static WNDPROC g_oldEditProc = nullptr;

    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN) {
            HWND parent = GetParent(hwnd);
            if (wParam == VK_ESCAPE) {
                DestroyWindow(parent);
                return 0;
            }
            if (wParam == VK_SPACE) {
                PostMessageA(parent, WM_KEYDOWN, VK_SPACE, lParam);
                return 0;
            }
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                PostMessageA(parent, WM_KEYDOWN, wParam, lParam);
                return 0;
            }
            if (wParam == VK_RETURN) {
                PostMessageA(parent, WM_COMMAND, 1005, 0);
                return 0;
            }
            if (wParam == 'S' && GetKeyState(VK_CONTROL) < 0) {
                PostMessageA(parent, WM_COMMAND, 1002, 0);
                return 0;
            }
        }
        return CallWindowProc(g_oldEditProc, hwnd, msg, wParam, lParam);
    }

    // === UI Helpers ===
    static void UpdateControlVisibility() {
        bool encode = (g_s.mode == AppMode::Encode);
        if (encode == g_s.showingEncodeControls) return;

        auto show = [](HWND h, bool v) {
            if (h) ShowWindow(h, v ? SW_SHOW : SW_HIDE);
        };
        show(g_s.btnEncode, encode);
        show(g_s.btnEcc, encode);
        show(g_s.btnGrid, encode);
        show(g_s.editHwnd, encode);
        show(g_s.labelText, encode);

        show(g_s.btnOpenImage, !encode);
        show(g_s.btnScanNow, !encode);

        g_s.showingEncodeControls = encode;
        InvalidateRect(g_s.hwnd, nullptr, TRUE);
    }

    // === Window Procedure ===
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE: {
                g_s.hwnd = hwnd;
                CreateBackBuffer(hwnd, g_s.width, g_s.height);
                g_s.lastTick = GetTickCount64();
                SetTimer(hwnd, kTimerId, kTimerInterval, nullptr);

                // Create Windows controls
                auto cs = (LPCREATESTRUCTA) lParam;
                HINSTANCE hInst = cs->hInstance;
                HFONT font = CreateDefaultFont(13);
                auto makeBtn = [&](const char *text, int x, int w, int id, DWORD style = BS_PUSHBUTTON) {
                    return CreateWindowExA(0, "BUTTON", text,
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                           x, 5, w, 26, hwnd, (HMENU) id, hInst, nullptr);
                };

                // Mode buttons
                g_s.btnEncodeMode = makeBtn("Encode", 8, 68, 4001, BS_AUTORADIOBUTTON | WS_GROUP);
                g_s.btnScanMode = makeBtn("Scan", 82, 68, 4002, BS_AUTORADIOBUTTON);
                SendMessageA(g_s.btnEncodeMode, WM_SETFONT, (WPARAM) font, TRUE);
                SendMessageA(g_s.btnScanMode, WM_SETFONT, (WPARAM) font, TRUE);

                // Separator is just visual space at x=156..174

                // Encode controls
                g_s.labelText = CreateWindowExA(0, "STATIC", "Text:",
                                                WS_CHILD | WS_VISIBLE, 176, 8, 36, 22, hwnd, nullptr, hInst, nullptr);
                SendMessageA(g_s.labelText, WM_SETFONT, (WPARAM) font, TRUE);

                g_s.editHwnd = CreateWindowExA(0, "EDIT", g_s.textInput,
                                               WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                               214, 6, 220, 24, hwnd, nullptr, hInst, nullptr);
                SendMessageA(g_s.editHwnd, WM_SETFONT, (WPARAM) font, TRUE);
                SendMessageA(g_s.editHwnd, EM_SETCUEBANNER, TRUE, (LPARAM) L"Enter text...");
                g_oldEditProc = (WNDPROC) SetWindowLongPtrA(g_s.editHwnd, GWLP_WNDPROC, (LONG_PTR) EditSubclassProc);

                g_s.btnEncode = makeBtn("Encode", 440, 64, 4003);
                SendMessageA(g_s.btnEncode, WM_SETFONT, (WPARAM) font, TRUE);

                g_s.btnEcc = makeBtn("ECC", 510, 50, 4004, BS_AUTOCHECKBOX);
                SendMessageA(g_s.btnEcc, WM_SETFONT, (WPARAM) font, TRUE);

                g_s.btnGrid = makeBtn("Grid", 566, 50, 4005, BS_AUTOCHECKBOX);
                SendMessageA(g_s.btnGrid, WM_SETFONT, (WPARAM) font, TRUE);

                // Scan controls
                g_s.btnOpenImage = makeBtn("Open Image...", 176, 96, 4008);
                SendMessageA(g_s.btnOpenImage, WM_SETFONT, (WPARAM) font, TRUE);

                g_s.btnScanNow = makeBtn("Scan Now", 278, 74, 4009);
                SendMessageA(g_s.btnScanNow, WM_SETFONT, (WPARAM) font, TRUE);

                // Right side buttons
                g_s.btnSave = makeBtn("&Save", g_s.width - 160, 68, 4006);
                SendMessageA(g_s.btnSave, WM_SETFONT, (WPARAM) font, TRUE);

                g_s.btnPlay = makeBtn("Pause", g_s.width - 86, 78, 4007);
                SendMessageA(g_s.btnPlay, WM_SETFONT, (WPARAM) font, TRUE);

                DeleteObject(font);

                // Initial visibility
                ShowWindow(g_s.btnEncodeMode, SW_SHOW);
                ShowWindow(g_s.btnScanMode, SW_SHOW);
                g_s.showingEncodeControls = true;
                UpdateControlVisibility();

                g_s.codec = new ParticleCodec("particle_codec", g_s.gridCols, g_s.gridRows);
                snprintf(g_s.statusText, sizeof(g_s.statusText),
                         "Ready | Grid: %dx%d | Max payload: %d bytes/frame",
                         g_s.gridCols, g_s.gridRows, g_s.codec->maxPayloadBytes());
                return 0;
            }

            case WM_SIZE: {
                int w = LOWORD(lParam);
                int h = HIWORD(lParam);
                if (w > 0 && h > 0) {
                    CreateBackBuffer(hwnd, w, h);
                    // Reposition edit boxes
                    if (g_s.editHwnd) {
                        SetWindowPos(g_s.editHwnd, nullptr, 224, 6, 270, 24, SWP_NOZORDER);
                    }
                }
                return 0;
            }

            case WM_TIMER: {
                if (!g_s.animating) return 0;
                unsigned long long now = GetTickCount64();
                double dt = (now - g_s.lastTick) / 1000.0;
                g_s.lastTick = now;
                g_s.time += dt * 2.0;
                g_s.frameCount++;

                HDC dc = GetDC(hwnd);
                RECT client;
                GetClientRect(hwnd, &client);
                RenderFrame();
                BitBlt(dc, 0, 0, client.right, client.bottom, g_s.backDc, 0, 0, SRCCOPY);
                ReleaseDC(hwnd, dc);
                return 0;
            }

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                RECT client;
                GetClientRect(hwnd, &client);
                RenderFrame();
                BitBlt(dc, 0, 0, client.right, client.bottom, g_s.backDc, 0, 0, SRCCOPY);
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_CTLCOLORBTN:
            case WM_CTLCOLORSTATIC: {
                HDC hdc = (HDC) wParam;
                SetBkColor(hdc, RGB(30, 30, 30));
                SetTextColor(hdc, RGB(200, 200, 200));
                if (!g_s.hbrDark)
                    g_s.hbrDark = CreateSolidBrush(RGB(30, 30, 30));
                return (LRESULT) g_s.hbrDark;
            }

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE)
                    DestroyWindow(hwnd);
                else if (wParam == VK_SPACE) {
                    g_s.animating = !g_s.animating;
                    if (g_s.animating) g_s.lastTick = GetTickCount64();
                } else if (wParam == 'O' && GetKeyState(VK_CONTROL) < 0) {
                    PostMessageA(hwnd, WM_COMMAND, 1001, 0);
                } else if (wParam == 'S' && GetKeyState(VK_CONTROL) < 0) {
                    PostMessageA(hwnd, WM_COMMAND, 1002, 0);
                } else if (wParam == VK_LEFT && g_s.currentFrame > 0) {
                    g_s.currentFrame--;
                    g_s.particles = g_s.frameParticles[g_s.currentFrame];
                    g_s.totalParticles = static_cast<int>(g_s.particles.size());
                } else if (wParam == VK_RIGHT && g_s.currentFrame < g_s.totalFrames - 1) {
                    g_s.currentFrame++;
                    g_s.particles = g_s.frameParticles[g_s.currentFrame];
                    g_s.totalParticles = static_cast<int>(g_s.particles.size());
                }
                return 0;

            case WM_LBUTTONDOWN: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
                int topY = kToolbarH;

                // Click on canvas - frame nav arrows (bottom-left of canvas)
                if (my > topY && g_s.totalFrames > 1) {
                    int canvasH = g_s.height - topY - kStatusH;
                    if (mx >= 12 && mx < 50 && my >= topY + canvasH - 40 && my < topY + canvasH - 10 && g_s.currentFrame
                        > 0) {
                        g_s.currentFrame--;
                        g_s.particles = g_s.frameParticles[g_s.currentFrame];
                        g_s.totalParticles = static_cast<int>(g_s.particles.size());
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (mx >= 80 && mx < 118 && my >= topY + canvasH - 40 && my < topY + canvasH - 10 && g_s.
                        currentFrame < g_s.totalFrames - 1) {
                        g_s.currentFrame++;
                        g_s.particles = g_s.frameParticles[g_s.currentFrame];
                        g_s.totalParticles = static_cast<int>(g_s.particles.size());
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
                return 0;
            }

            case WM_COMMAND: {
                int id = LOWORD(wParam);
                if (id == 1001) {
                    // Open Image
                    OPENFILENAMEA ofn = {};
                    char path[MAX_PATH] = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = "Images\0*.bmp;*.png;*.jpg;*.jpeg;*.gif;*.tiff\0All\0*.*\0";
                    ofn.lpstrFile = path;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                    if (!GetOpenFileNameA(&ofn)) return 0;

                    // Load image
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
                    std::wstring wpath(wlen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);

                    delete g_s.loadedImage;
                    g_s.loadedImage = LoadImageFile(wpath.c_str());
                    if (!g_s.loadedImage || g_s.loadedImage->GetLastStatus() != Gdiplus::Ok) {
                        delete g_s.loadedImage;
                        g_s.loadedImage = nullptr;
                        snprintf(g_s.scanStatus, sizeof(g_s.scanStatus), "Failed to load: %s", path);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    g_s.mode = AppMode::Scan;
                    g_s.showScanOverlay = false;
                    g_s.scanResult.clear();

                    // Get image dimensions for display
                    g_s.scanImageWidth = g_s.loadedImage->GetWidth();
                    g_s.scanImageHeight = g_s.loadedImage->GetHeight();
                    snprintf(g_s.scanStatus, sizeof(g_s.scanStatus),
                             "Loaded: %s (%dx%d)", path, g_s.scanImageWidth, g_s.scanImageHeight);

                    // Auto-detect if image is within reasonable size
                    if (g_s.scanImageWidth > 0 && g_s.scanImageHeight > 0) {
                        // Auto run detection
                        DetectParticlesFromImage(g_s.loadedImage);
                        g_s.showScanOverlay = true;
                    }

                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 1002) {
                    // Export canvas as clean image (no UI, no motion, full-fill)
                    if (g_s.particles.empty()) {
                        MessageBoxA(hwnd, "No particles to export.", "Export", MB_OK);
                        return 0;
                    }
                    OPENFILENAMEA ofn = {};
                    char path[MAX_PATH] = "particle_field.png";
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = "PNG\0*.png\0Bitmap\0*.bmp\0";
                    ofn.lpstrFile = path;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                    if (!GetSaveFileNameA(&ofn)) return 0;

                    int exportW = g_s.gridCols * 8;
                    int exportH = g_s.gridRows * 8;

                    // Create bitmap
                    HDC dc = GetDC(hwnd);
                    HDC memDc = CreateCompatibleDC(dc);
                    HBITMAP bmp = CreateCompatibleBitmap(dc, exportW, exportH);
                    SelectObject(memDc, bmp);

                    // Dark background
                    HBRUSH bg = CreateSolidBrush(kBgColor);
                    RECT fr = {0, 0, exportW, exportH};
                    FillRect(memDc, &fr, bg);
                    DeleteObject(bg);

                    // Draw particles at full scale, no centering, no motion
                    double scale = static_cast<double>(exportW) / g_s.gridCols;
                    auto oldBrush = SelectObject(memDc, GetStockObject(DC_BRUSH));
                    auto oldPen = SelectObject(memDc, GetStockObject(NULL_PEN));

                    int pr = std::max(static_cast<int>(0.35 * scale), 2);
                    int gr = pr * 2;

                    for (const auto &[px, py]: g_s.particles) {
                        double sx = px * scale;
                        double sy = py * scale;

                        SetDCBrushColor(memDc, kGlowColor);
                        Ellipse(memDc, sx - gr, sy - gr, sx + gr, sy + gr);

                        SetDCBrushColor(memDc, kParticleColor);
                        Ellipse(memDc, sx - pr, sy - pr, sx + pr, sy + pr);
                    }

                    SelectObject(memDc, oldBrush);
                    SelectObject(memDc, oldPen);

                    // Save as PNG or BMP
                    if (ofn.nFilterIndex == 1 || strstr(path, ".png")) {
                        // Use GDI+ to save as PNG
                        Gdiplus::Bitmap gpBmp(bmp, nullptr);
                        CLSID clsid;
                        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &clsid);
                        int wlen = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
                        std::wstring wpath(wlen, L'\0');
                        MultiByteToWideChar(CP_ACP, 0, path, -1, &wpath[0], wlen);
                        gpBmp.Save(wpath.c_str(), &clsid);
                    } else {
                        // Save as BMP manually
                        BITMAPFILEHEADER bf = {};
                        BITMAPINFOHEADER bi = {};
                        bi.biSize = sizeof(bi);
                        bi.biWidth = exportW;
                        bi.biHeight = exportH;
                        bi.biPlanes = 1;
                        bi.biBitCount = 24;
                        bi.biCompression = BI_RGB;

                        DWORD size = ((exportW * 3 + 3) / 4) * 4 * exportH;
                        std::vector<uint8_t> pixels(size);
                        GetDIBits(memDc, bmp, 0, exportH, pixels.data(),
                                  (BITMAPINFO *) &bi, DIB_RGB_COLORS);

                        HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hFile != INVALID_HANDLE_VALUE) {
                            bf.bfType = 0x4D42;
                            bf.bfSize = sizeof(bf) + sizeof(bi) + size;
                            bf.bfOffBits = sizeof(bf) + sizeof(bi);
                            DWORD written;
                            WriteFile(hFile, &bf, sizeof(bf), &written, nullptr);
                            WriteFile(hFile, &bi, sizeof(bi), &written, nullptr);
                            WriteFile(hFile, pixels.data(), size, &written, nullptr);
                            CloseHandle(hFile);
                        }
                    }

                    DeleteObject(bmp);
                    DeleteDC(memDc);
                    ReleaseDC(hwnd, dc);

                    snprintf(g_s.statusText, sizeof(g_s.statusText),
                             "Exported: %s (%dx%d, %d particles)",
                             path, exportW, exportH, (int) g_s.particles.size());
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 1003) {
                    // Save image with GDI+
                    if (!g_s.loadedImage) return 0;
                    OPENFILENAMEA ofn = {};
                    char path[MAX_PATH] = "scanned_particles.png";
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = "PNG\0*.png\0JPEG\0*.jpg\0Bitmap\0*.bmp\0";
                    ofn.lpstrFile = path;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                    if (!GetSaveFileNameA(&ofn)) return 0;

                    CLSID clsid;
                    if (ofn.nFilterIndex == 1) {
                        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &clsid);
                    } else {
                        CLSIDFromString(L"{557CF401-1A04-11D3-9A73-0000F81EF32E}", &clsid);
                    }
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
                    std::wstring wpath(wlen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);
                    g_s.loadedImage->Save(wpath.c_str(), &clsid);
                } else if (id == 1004) {
                    DestroyWindow(hwnd);
                } else if (id == 2001 || id == 4001) {
                    g_s.mode = AppMode::Encode;
                    CheckRadioButton(hwnd, 4001, 4002, 4001);
                    UpdateControlVisibility();
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 2002 || id == 4002) {
                    g_s.mode = AppMode::Scan;
                    CheckRadioButton(hwnd, 4001, 4002, 4002);
                    UpdateControlVisibility();
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 4003 || id == 1005) {
                    // Enter pressed in edit box → trigger Encode
                    GetWindowTextA(g_s.editHwnd, g_s.textInput, sizeof(g_s.textInput));
                    delete g_s.codec;
                    g_s.codec = new ParticleCodec("particle_codec", g_s.gridCols, g_s.gridRows);
                    DoEncode();
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 4004) {
                    g_s.useEcc = (SendMessageA(g_s.btnEcc, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 4005) {
                    g_s.showGrid = (SendMessageA(g_s.btnGrid, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 4006) {
                    PostMessageA(hwnd, WM_COMMAND, 1002, 0);
                } else if (id == 4007) {
                    g_s.animating = !g_s.animating;
                    SetWindowTextA(g_s.btnPlay, g_s.animating ? "Pause" : "Play");
                    if (g_s.animating) g_s.lastTick = GetTickCount64();
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (id == 4008) {
                    PostMessageA(hwnd, WM_COMMAND, 1001, 0);
                } else if (id == 4009) {
                    if (g_s.loadedImage) {
                        DetectParticlesFromImage(g_s.loadedImage);
                        g_s.showScanOverlay = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else if (id == 3001) {
                    MessageBoxA(hwnd,
                                "Particle Codec Viewer v1.0\n\n"
                                "Encode data as particle positions on a shuffled grid.\n\n"
                                "Encode Mode: Type text and click Encode.\n"
                                "Scan Mode: Open an image of a particle field to decode.\n\n"
                                "Controls:\n"
                                "  Space - Play/Pause animation\n"
                                "  Ctrl+O - Open image\n"
                                "  Left/Right - Navigate frames\n"
                                "  ESC - Exit",
                                "About Particle Codec",
                                MB_OK | MB_ICONINFORMATION);
                }
                return 0;
            }

            case WM_DESTROY:
                KillTimer(hwnd, kTimerId);
                if (g_s.backDc) {
                    SelectObject(g_s.backDc, g_s.backBmp);
                    DeleteDC(g_s.backDc);
                    DeleteObject(g_s.backBmp);
                }
                if (g_s.hbrDark) DeleteObject(g_s.hbrDark);
                delete g_s.codec;
                delete g_s.loadedImage;
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    // === Menu Creation ===
    static HMENU CreateAppMenu() {
        HMENU menu = CreateMenu();

        HMENU fileMenu = CreatePopupMenu();
        AppendMenuA(fileMenu, MF_STRING, 1001, "&Open Image...\tCtrl+O");
        AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(fileMenu, MF_STRING, 1002, "&Save Screenshot...");
        if (true) {
            // Always show
            AppendMenuA(fileMenu, MF_STRING, 1003, "Save Scanned Image...");
        }
        AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(fileMenu, MF_STRING, 1004, "E&xit\tESC");
        AppendMenuA(menu, MF_POPUP, (UINT_PTR) fileMenu, "&File");

        HMENU modeMenu = CreatePopupMenu();
        AppendMenuA(modeMenu, MF_STRING, 2001, "&Encode");
        AppendMenuA(modeMenu, MF_STRING, 2002, "&Scan");
        AppendMenuA(menu, MF_POPUP, (UINT_PTR) modeMenu, "&Mode");

        HMENU helpMenu = CreatePopupMenu();
        AppendMenuA(helpMenu, MF_STRING, 3001, "&About");
        AppendMenuA(menu, MF_POPUP, (UINT_PTR) helpMenu, "&Help");

        return menu;
    }
}

// === Main ===
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int showCmd) {
    InitGdiplus();

    const char *className = "ParticleCodecViewer";

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = className;
    wc.lpszMenuName = nullptr;
    RegisterClassExA(&wc);

    // Parse command line
    std::string input = "Hello Particle Codec!";
    int gridSize = 60;
    bool useEcc = false;

    if (__argc > 1) input = __argv[1];
    if (__argc > 2) gridSize = std::stoi(__argv[2]);
    if (__argc > 3) useEcc = (std::string(__argv[3]) == "ecc");

    strncpy_s(g_s.textInput, input.c_str(), sizeof(g_s.textInput) - 1);
    g_s.gridCols = gridSize;
    g_s.gridRows = gridSize;
    g_s.useEcc = useEcc;

    int windowW = 1000;
    int windowH = 760;

    RECT wr = {0, 0, windowW, windowH};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, TRUE);
    windowW = wr.right - wr.left;
    windowH = wr.bottom - wr.top;

    HWND hwnd = CreateWindowExA(0, className, "Particle Codec Viewer",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                windowW, windowH, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // Set menu
    g_s.menuBar = CreateAppMenu();
    SetMenu(hwnd, g_s.menuBar);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ShutdownGdiplus();
    return 0;
}
