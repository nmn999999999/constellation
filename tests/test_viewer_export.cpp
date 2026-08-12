#include <particle_codec/codec.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>

#define NOMINMAX
#include <windows.h>

using namespace particle_codec;

int main() {
    const int gridCols = 60, gridRows = 60;
    const int exportW = gridCols * 8;
    const int exportH = gridRows * 8;
    const double scale = static_cast<double>(exportW) / gridCols;

    std::string message = "Hello from viewer export!";

    std::cout << "=== Viewer Export Decode Test ===" << std::endl;
    std::cout << "Message: " << message << std::endl;
    std::cout << "Export size: " << exportW << "x" << exportH << ", scale=" << scale << std::endl;

    ParticleCodec codec("particle_codec", gridCols, gridRows);
    auto frames = codec.encode(std::vector<uint8_t>(message.begin(), message.end()));
    if (frames.empty()) {
        std::cerr << "FAIL: no frames" << std::endl;
        return 1;
    }

    auto &frame = frames[0];
    std::cout << "Particles: " << frame.particleCount << std::endl;

    // Create bitmap matching viewer export format exactly
    HDC hdc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, exportW, exportH);
    SelectObject(memDc, bmp);

    // Dark background
    HBRUSH bg = CreateSolidBrush(RGB(10, 10, 26));
    RECT fr = {0, 0, exportW, exportH};
    FillRect(memDc, &fr, bg);
    DeleteObject(bg);

    auto oldBrush = SelectObject(memDc, GetStockObject(DC_BRUSH));
    auto oldPen = SelectObject(memDc, GetStockObject(NULL_PEN));

    int pr = std::max(static_cast<int>(0.35 * scale), 2);
    int gr = pr * 2;
    std::cout << "Core radius: " << pr << ", Glow radius: " << gr << std::endl;

    for (int i = 0; i < frame.particleCount; i++) {
        float fx = frame.particles[i * 2];
        float fy = frame.particles[i * 2 + 1];
        double sx = fx * scale;
        double sy = fy * scale;

        SetDCBrushColor(memDc, RGB(0, 100, 130));
        Ellipse(memDc, (int) (sx - gr), (int) (sy - gr), (int) (sx + gr), (int) (sy + gr));

        SetDCBrushColor(memDc, RGB(0, 188, 212));
        Ellipse(memDc, (int) (sx - pr), (int) (sy - pr), (int) (sx + pr), (int) (sy + pr));
    }

    SelectObject(memDc, oldBrush);
    SelectObject(memDc, oldPen);

    // Save as BMP
    const char *testPath = "viewer_export_test.bmp";
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = exportW;
    bi.bmiHeader.biHeight = -exportH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD size = ((exportW * 3 + 3) / 4) * 4 * exportH;
    std::vector<uint8_t> pixels(size);
    GetDIBits(memDc, bmp, 0, exportH, pixels.data(), &bi, DIB_RGB_COLORS);

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42;
    bfh.bfSize = 54 + size;
    bfh.bfOffBits = 54;

    FILE *f = fopen(testPath, "wb");
    if (f) {
        fwrite(&bfh, 1, sizeof(bfh), f);
        fwrite(&bi.bmiHeader, 1, sizeof(bi.bmiHeader), f);
        fwrite(pixels.data(), 1, size, f);
        fclose(f);
        std::cout << "Saved: " << testPath << " (" << exportW << "x" << exportH << ")" << std::endl;
    }

    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, hdc);

    std::cout << std::endl;
    std::cout << "Run: decode_image.exe " << testPath << " " << gridCols << " " << gridRows <<
            std::endl;
    return 0;
}
