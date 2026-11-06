#include "TrayIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <objidl.h>
#include <gdiplus.h>

namespace npx
{
    namespace
    {
        void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r)
        {
            const float d = 2 * r;
            path.AddArc(x, y, d, d, 180, 90);
            path.AddArc(x + w - d, y, d, d, 270, 90);
            path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            path.AddArc(x, y + h - d, d, d, 90, 90);
            path.CloseFigure();
        }

        // Griglia di 16 unità agganciata ai pixel della misura vera (con PixelOffsetModeHalf il pixel i va da i
        // a i+1): linee di spessore intero sul centro dei pixel, nitide anche a 20 px (schermo al 125%).
        struct PixelGrid
        {
            float s;     // pixel per unità
            float width; // spessore delle linee: 1 px fino a 20 px, poi circa quanto la scala
            explicit PixelGrid(int size) : s(static_cast<float>(size) / 16.0f), width(s < 1.4f ? 1.0f : (s < 2.6f ? 2.0f : std::round(s))) {}
            // Posizione di una linea: centro di un pixel (spessore dispari) o bordo fra due pixel (pari)
            float Line(float u) const { return static_cast<int>(width) % 2 == 1 ? std::floor(u * s) + 0.5f : std::round(u * s); }
        };
    }

    HICON CreateNotepadOutlineIcon(int size, bool lightTaskbar)
    {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        {
            return nullptr;
        }
        HICON icon = nullptr;
        {
            const PixelGrid grid(size);
            const float s = grid.s;
            const BYTE tone = lightTaskbar ? 0 : 255;
            const Gdiplus::Color color(255, tone, tone, tone);
            Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&bitmap);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            Gdiplus::Pen pen(color, grid.width);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            pen.SetStartCap(Gdiplus::LineCapSquare); // estremità sul bordo dei pixel
            pen.SetEndCap(Gdiplus::LineCapSquare);

            // Pagina
            const float left = grid.Line(2.5f), right = grid.Line(13.5f), top = grid.Line(3.5f), bottom = grid.Line(14.5f);
            Gdiplus::GraphicsPath page;
            AddRoundRect(page, left, top, right - left, bottom - top, std::max(1.0f, 1.6f * s));
            graphics.DrawPath(&pen, &page);
            // Tre anelli che passano il bordo in alto, a passo intero intorno al centro (sempre equidistanti)
            const float middle = grid.Line(8.0f);
            const float ringStep = std::max(2.0f, std::round(2.5f * s));
            const float ringTop = grid.Line(1.5f), ringBottom = grid.Line(5.0f);
            for (int k = -1; k <= 1; ++k)
            {
                const float x = middle + k * ringStep;
                graphics.DrawLine(&pen, x, ringTop, x, ringBottom);
            }
            // Tre righe a passo intero, l'ultima più corta
            const float lineLeft = grid.Line(5.5f), lineRight = grid.Line(10.5f), shortRight = grid.Line(8.0f);
            const float firstRow = grid.Line(7.5f), rowStep = std::max(2.0f, std::round(2.0f * s));
            graphics.DrawLine(&pen, lineLeft, firstRow, lineRight, firstRow);
            graphics.DrawLine(&pen, lineLeft, firstRow + rowStep, lineRight, firstRow + rowStep);
            graphics.DrawLine(&pen, lineLeft, firstRow + 2 * rowStep, shortRight, firstRow + 2 * rowStep);

            bitmap.GetHICON(&icon);
        }
        Gdiplus::GdiplusShutdown(token);
        return icon;
    }

    bool IsTaskbarLight()
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme",
                         RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        {
            return false;
        }
        return value != 0;
    }

    HICON CreateGlyphIcon(wchar_t glyph, int size, bool lightTaskbar)
    {
        // GDI non scrive il canale alfa: si disegna il glifo bianco su nero e si usa
        // la luminosità come alfa, ottenendo bordi antialias corretti.
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP color = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!dc || !color || !bits)
        {
            if (color)
            {
                DeleteObject(color);
            }
            if (dc)
            {
                DeleteDC(dc);
            }
            return LoadIconW(nullptr, IDI_APPLICATION);
        }

        HGDIOBJ oldBitmap = SelectObject(dc, color);
        RECT rect{ 0, 0, size, size };
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe Fluent Icons");
        HGDIOBJ oldFont = SelectObject(dc, font);
        wchar_t face[LF_FACESIZE]{};
        GetTextFaceW(dc, LF_FACESIZE, face);
        if (wcscmp(face, L"Segoe Fluent Icons") != 0)
        {
            // Windows 10: stesso set di glifi in "Segoe MDL2 Assets".
            SelectObject(dc, oldFont);
            DeleteObject(font);
            font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
            oldFont = SelectObject(dc, font);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, &glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();

        const uint8_t tone = lightTaskbar ? 0 : 255;
        auto* pixels = static_cast<uint32_t*>(bits);
        for (int i = 0; i < size * size; ++i)
        {
            const uint32_t p = pixels[i];
            const uint8_t r = static_cast<uint8_t>((p >> 16) & 0xFF);
            const uint8_t g = static_cast<uint8_t>((p >> 8) & 0xFF);
            const uint8_t b = static_cast<uint8_t>(p & 0xFF);
            const uint8_t alpha = static_cast<uint8_t>((r * 30 + g * 59 + b * 11) / 100);
            pixels[i] = (static_cast<uint32_t>(alpha) << 24) | (static_cast<uint32_t>(tone) << 16) | (static_cast<uint32_t>(tone) << 8) | tone;
        }

        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);

        std::vector<uint8_t> maskBits(static_cast<size_t>(((size + 15) / 16) * 2 * size), 0);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmMask = mask;
        info.hbmColor = color;
        HICON icon = CreateIconIndirect(&info);

        DeleteObject(mask);
        DeleteObject(color);
        return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    }
}
