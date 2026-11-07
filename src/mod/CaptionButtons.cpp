#include "pch.h"
#include "CaptionButtons.h"

#include "Lifetime.h"
#include "Log.h"
#include "Shared.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace npx
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"VTabsNotepad.CaptionButtons";
        // WTA_NONCLIENT: pulsanti di DWM nascosti (flag non documentato del Windows App SDK, vedi l'header).
        constexpr DWORD kHideSystemButtons = 0x10000;
        constexpr UINT_PTR kTipTimer = 1;
        constexpr UINT_PTR kThemeTimer = 2;
        constexpr UINT kTipDelayMs = 600;
        constexpr UINT kThemeDelayMs = 250;
        constexpr UINT kPopupSystemMenu = 0x0313; // WM_POPUPSYSTEMMENU: menu di sistema in un punto dello schermo (come la barra delle applicazioni)
        // Glifi di Segoe Fluent Icons (gli stessi in Segoe MDL2 Assets): riduci, ingrandisci, ripristina, chiudi.
        constexpr wchar_t kMinimizeGlyph = L'\xE921';
        constexpr wchar_t kMaximizeGlyph = L'\xE922';
        constexpr wchar_t kRestoreGlyph = L'\xE923';
        constexpr wchar_t kCloseGlyph = L'\xE8BB';
        constexpr float kGlyphSizeDip = 10.0f;

        // Colori dei pulsanti della barra del titolo di Windows 11 (risorse CaptionButton* di WinUI; glifo della finestra
        // non attiva misurato su quelli di DWM nel tema scuro: #797979 su #202020, cioè bianco al 40%), ARGB.
        struct Palette
        {
            UINT32 glyph, glyphInactive, hoverBackground, hoverGlyph, pressedBackground, pressedGlyph;
        };
        constexpr Palette kDark{ 0xFFFFFFFF, 0x66FFFFFF, 0x0FFFFFFF, 0xFFFFFFFF, 0x0AFFFFFF, 0xC5FFFFFF };
        constexpr Palette kLight{ 0xE4000000, 0x5C000000, 0x09000000, 0xE4000000, 0x06000000, 0x9E000000 };
        constexpr UINT32 kCloseHoverBackground = 0xFFC42B1C;
        constexpr UINT32 kCloseHoverGlyph = 0xFFFFFFFF;
        constexpr UINT32 kClosePressedBackground = 0xE6C42B1C;
        constexpr UINT32 kClosePressedGlyph = 0xB3FFFFFF;

        D2D1_COLOR_F ToColor(UINT32 argb)
        {
            return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f, static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                                static_cast<float>(argb & 0xFF) / 255.0f, static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
        }

        HINSTANCE ThisModule()
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&ToColor), &module);
            return module;
        }

        bool HighContrast()
        {
            HIGHCONTRASTW contrast{ sizeof(contrast) };
            return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
        }

        // Tema delle app di Windows: true = scuro (solo se la finestra non dà il suo a DWM).
        bool AppsUseDarkTheme()
        {
            DWORD value = 1;
            DWORD size = sizeof(value);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                             RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            {
                return false;
            }
            return value == 0;
        }

        bool SameRect(RECT const& a, RECT const& b)
        {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }

        int ButtonFromHit(WPARAM hit)
        {
            switch (hit)
            {
            case HTMINBUTTON:
                return 0;
            case HTMAXBUTTON:
                return 1;
            case HTCLOSE:
                return 2;
            default:
                return -1;
            }
        }
    }

    CaptionButtons::~CaptionButtons()
    {
        Remove(m_main && IsWindow(m_main));
    }

    int CaptionButtons::ButtonPx() const
    {
        const UINT dpi = m_main ? GetDpiForWindow(m_main) : 96;
        return std::max(1, static_cast<int>(std::lround(kButtonWidthDip * (dpi ? dpi : 96) / 96.0)));
    }

    int CaptionButtons::VisibleTop() const
    {
        // Ingrandita, la finestra sborda dallo schermo della larghezza del bordo (la parte sopra non si vede): i pulsanti
        // cominciano dal bordo dello schermo, come quelli di DWM.
        if (!IsZoomed(m_main))
        {
            return 0;
        }
        POINT origin{};
        ClientToScreen(m_main, &origin);
        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(MonitorFromWindow(m_main, MONITOR_DEFAULTTONEAREST), &info))
        {
            return 0;
        }
        return std::max(0, static_cast<int>(info.rcWork.top - origin.y));
    }

    bool CaptionButtons::ReadDark() const
    {
        // Il tema che la finestra dà a DWM (barra del titolo scura) se si legge; altrimenti quello delle app di Windows.
        BOOL dark = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(m_main, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        {
            return dark != FALSE;
        }
        return AppsUseDarkTheme();
    }

    void CaptionButtons::HideSystemButtons(bool hide)
    {
        if (!m_main || hide == m_systemHidden)
        {
            return;
        }
        WTA_OPTIONS options{};
        options.dwMask = kHideSystemButtons;
        options.dwFlags = hide ? kHideSystemButtons : 0;
        const HRESULT hr = SetWindowThemeAttribute(m_main, WTA_NONCLIENT, &options, sizeof(options));
        if (SUCCEEDED(hr))
        {
            m_systemHidden = hide;
        }
        NPX_LOG(L"CaptionButtons: pulsanti di DWM %s (0x%08X).", hide ? L"nascosti" : L"di nuovo visibili", static_cast<unsigned>(hr));
    }

    void CaptionButtons::Update(HWND main, RECT const& row)
    {
        if (m_removing)
        {
            return;
        }
        if (m_main && m_main != main)
        {
            Remove();
        }
        m_main = main;
        if (!main || !IsWindow(main))
        {
            return;
        }
        const int button = ButtonPx();
        // Dalla cima della finestra (come i pulsanti di DWM, sopra anche il bordo superiore) al fondo della riga.
        RECT rect{ row.right - 3 * button, VisibleTop(), row.right, row.bottom };
        // Glifi al centro della riga (allineati a menu e pulsanti della barra), anche da ingrandita quando la sua parte in
        // alto è fuori dallo schermo; a mezzo pixel come quelli di DWM (stessa resa dei bordi, confrontata pixel per pixel).
        m_glyphCenter = std::floor(static_cast<float>(row.top + row.bottom) / 2.0f - static_cast<float>(rect.top)) + 0.5f;
        // Solo con una riga vera, abbastanza larga, con il menu di sistema (pulsanti) e senza contrasto elevato (lì restano
        // quelli di DWM, con i colori del tema).
        const bool wanted = !IsRectEmpty(&row) && rect.left >= row.left && rect.bottom - rect.top >= button / 2 &&
                            (GetWindowLongW(main, GWL_STYLE) & (WS_CAPTION | WS_SYSMENU)) == (WS_CAPTION | WS_SYSMENU) && !HighContrast();
        if (!wanted)
        {
            if (m_hwnd || m_systemHidden)
            {
                Remove();
            }
            return;
        }
        if (!m_hwnd)
        {
            m_active = GetForegroundWindow() == main;
            m_dark = ReadDark();
            if (!Create(rect))
            {
                return; // restano quelli di DWM
            }
            HideSystemButtons(true);
            return;
        }
        HideSystemButtons(true);
        if (!SameRect(rect, m_rect))
        {
            m_rect = rect;
            SetWindowPos(m_hwnd, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE);
        }
        else if (GetWindow(main, GW_CHILD) != m_hwnd)
        {
            // Il Blocco note rimette in cima le sue finestre: i pulsanti tornano sopra tutto.
            SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        Paint(); // (solo se è cambiato qualcosa: ingrandita, dimensioni, scala)
    }

    bool CaptionButtons::Create(RECT const& rect)
    {
        // La registrazione si controlla ogni volta: allo scaricamento della mod la classe si toglie (UnregisterWindowClass)
        // e, se la mod riparte senza essere uscita, va rifatta.
        const bool registered = [] {
            WNDCLASSEXW info{ sizeof(info) };
            if (GetClassInfoExW(ThisModule(), kClassName, &info))
            {
                return true;
            }
            WNDCLASSEXW cls{ sizeof(cls) };
            cls.lpfnWndProc = WndProc;
            cls.hInstance = ThisModule();
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.lpszClassName = kClassName;
            return RegisterClassExW(&cls) != 0;
        }();
        if (!registered)
        {
            NPX_LOG(L"CaptionButtons: classe di finestra non registrata (%lu).", GetLastError());
            return false;
        }
        m_rect = rect;
        // WS_EX_NOPARENTNOTIFY: niente WM_PARENTNOTIFY alla finestra principale per una finestra che non è sua.
        m_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY, kClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, rect.left, rect.top,
                                 rect.right - rect.left, rect.bottom - rect.top, m_main, nullptr, ThisModule(), this);
        if (!m_hwnd)
        {
            NPX_LOG(L"CaptionButtons: finestra dei pulsanti non creata (%lu): restano quelli di DWM.", GetLastError());
            return false;
        }
        m_haveDrawn = false;
        Paint();
        SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        NPX_LOG(L"CaptionButtons: pulsanti della finestra alti %ld px (%d px l'uno), tema %s.", rect.bottom - rect.top, ButtonPx(),
                 m_dark ? L"scuro" : L"chiaro");
        return true;
    }

    void CaptionButtons::Remove(bool restoreSystem)
    {
        if (m_removing)
        {
            return;
        }
        // Prima lo stato, poi le finestre: DestroyWindow fa arrivare messaggi alla finestra principale e il layout può
        // richiamare Update mentre siamo qui (lo ignora finché m_removing è acceso).
        m_removing = true;
        HideTip();
        const HWND hwnd = std::exchange(m_hwnd, nullptr);
        const HWND tooltip = std::exchange(m_tooltip, nullptr);
        m_hot = -1;
        m_pressed = -1;
        m_tracking = false;
        m_haveDrawn = false;
        m_rect = {};
        if (m_systemHidden)
        {
            // Prima tornano quelli di DWM, poi spariscono i nostri: nessun momento senza pulsanti.
            if (restoreSystem && m_main && IsWindow(m_main))
            {
                HideSystemButtons(false);
            }
            m_systemHidden = false;
        }
        if (tooltip && IsWindow(tooltip))
        {
            DestroyWindow(tooltip);
        }
        if (hwnd && IsWindow(hwnd))
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }
        m_removing = false;
    }

    void CaptionButtons::SetActive(bool active)
    {
        if (active != m_active)
        {
            m_active = active;
            Paint();
        }
    }

    void CaptionButtons::ThemeChanged()
    {
        if (m_hwnd)
        {
            SetTimer(m_hwnd, kThemeTimer, kThemeDelayMs, nullptr);
        }
    }

    void CaptionButtons::Verify()
    {
        if (!m_hwnd || !m_main || !IsWindow(m_main))
        {
            return;
        }
        // Al massimo due volte al secondo (si chiama dai controlli del layout, anche molto frequenti).
        const ULONGLONG now = GetTickCount64();
        if (now - m_lastVerify < 500)
        {
            return;
        }
        m_lastVerify = now;
        RECT bounds{};
        if (SUCCEEDED(DwmGetWindowAttribute(m_main, DWMWA_CAPTION_BUTTON_BOUNDS, &bounds, sizeof(bounds))) && bounds.right > bounds.left)
        {
            // Qualcuno ha rimesso le opzioni del tema della finestra: di nuovo nascosti.
            NPX_LOG(L"CaptionButtons: i pulsanti di DWM erano di nuovo visibili.");
            m_systemHidden = false;
            HideSystemButtons(true);
        }
        if (const bool dark = ReadDark(); dark != m_dark)
        {
            m_dark = dark;
            Paint();
        }
    }

    void CaptionButtons::DevSetState(int hot, int pressed)
    {
        m_hot = hot;
        m_pressed = pressed;
        Paint();
    }

    int CaptionButtons::ButtonAt(POINT point) const
    {
        const int width = m_rect.right - m_rect.left;
        const int height = m_rect.bottom - m_rect.top;
        if (point.x < 0 || point.y < 0 || point.x >= width || point.y >= height)
        {
            return -1;
        }
        const int fromRight = (width - 1 - point.x) / ButtonPx(); // 0 = chiudi (in fondo a destra)
        return fromRight > 2 ? -1 : 2 - fromRight;
    }

    void CaptionButtons::Press(int button)
    {
        HideTip();
        m_pressed = button;
        m_hot = button;
        SetCapture(m_hwnd);
        Paint();
    }

    void CaptionButtons::Invoke(int button)
    {
        if (!m_main)
        {
            return;
        }
        const bool zoomed = IsZoomed(m_main) != FALSE;
        const WPARAM command = button == 0 ? SC_MINIMIZE : button == 1 ? (zoomed ? SC_RESTORE : SC_MAXIMIZE) : SC_CLOSE;
        NPX_LOG(L"CaptionButtons: %s.", button == 0 ? L"riduci a icona" : button == 1 ? (zoomed ? L"ripristina" : L"ingrandisci") : L"chiudi");
        PostMessageW(m_main, WM_SYSCOMMAND, command, 0);
    }

    std::wstring CaptionButtons::TipText(int button) const
    {
        // Come i suggerimenti dei pulsanti di DWM: il nome della voce del menu di sistema (senza & e senza la scorciatoia).
        const bool zoomed = IsZoomed(m_main) != FALSE;
        const UINT command = button == 0 ? SC_MINIMIZE : button == 1 ? (zoomed ? SC_RESTORE : SC_MAXIMIZE) : SC_CLOSE;
        wchar_t text[128]{};
        if (const HMENU menu = GetSystemMenu(m_main, FALSE); menu && GetMenuStringW(menu, command, text, ARRAYSIZE(text), MF_BYCOMMAND) > 0)
        {
            std::wstring result;
            for (const wchar_t* p = text; *p && *p != L'\t'; ++p)
            {
                if (*p != L'&')
                {
                    result += *p;
                }
                else if (p[1] == L'&')
                {
                    result += L'&';
                    ++p;
                }
            }
            if (!result.empty())
            {
                return result;
            }
        }
        return button == 0 ? Tr(L"Minimize", L"Riduci a icona")
               : button == 1 ? (zoomed ? Tr(L"Restore", L"Ripristina") : Tr(L"Maximize", L"Ingrandisci"))
                             : Tr(L"Close", L"Chiudi");
    }

    void CaptionButtons::ShowTip()
    {
        if (!m_hwnd || m_hot < 0 || m_pressed >= 0)
        {
            return;
        }
        if (!m_tooltip)
        {
            m_tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd, nullptr, ThisModule(), nullptr);
            if (!m_tooltip)
            {
                return;
            }
            TTTOOLINFOW tool{ sizeof(tool) };
            tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            tool.hwnd = m_hwnd;
            tool.uId = 1;
            tool.lpszText = const_cast<LPWSTR>(L"");
            SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        std::wstring text = TipText(m_hot);
        TTTOOLINFOW tool{ sizeof(tool) };
        tool.hwnd = m_hwnd;
        tool.uId = 1;
        tool.lpszText = text.data();
        SendMessageW(m_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        POINT cursor{};
        GetCursorPos(&cursor);
        const int below = static_cast<int>(std::lround(20.0 * GetDpiForWindow(m_hwnd) / 96.0));
        SendMessageW(m_tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(cursor.x, cursor.y + below));
        SendMessageW(m_tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
    }

    void CaptionButtons::HideTip()
    {
        if (m_hwnd)
        {
            KillTimer(m_hwnd, kTipTimer);
        }
        if (m_tooltip)
        {
            TTTOOLINFOW tool{ sizeof(tool) };
            tool.hwnd = m_hwnd;
            tool.uId = 1;
            SendMessageW(m_tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
        }
    }

    bool CaptionButtons::EnsureGraphics()
    {
        try
        {
            if (!m_d2d)
            {
                winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.put()));
            }
            if (!m_dwrite)
            {
                winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwrite.put())));
                // Segoe Fluent Icons (Windows 11), altrimenti Segoe MDL2 Assets (stessi codici).
                m_fontFamily = L"Segoe MDL2 Assets";
                winrt::com_ptr<IDWriteFontCollection> fonts;
                if (SUCCEEDED(m_dwrite->GetSystemFontCollection(fonts.put(), FALSE)))
                {
                    UINT32 index = 0;
                    BOOL exists = FALSE;
                    if (SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) && exists)
                    {
                        m_fontFamily = L"Segoe Fluent Icons";
                    }
                }
            }
            if (!m_target)
            {
                // DPI 96: le unità di Direct2D sono pixel.
                const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
                winrt::check_hresult(m_d2d->CreateDCRenderTarget(&properties, m_target.put()));
                m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // (ClearType non va con la trasparenza)
                // Antialias lineare (gamma 1, senza contrasto aumentato: le diagonali di chiudi altrimenti risultano più spesse di
                // quelle di DWM). Confrontati pixel per pixel con quelli di DWM: ingrandisci, ripristina e chiudi come DWM senza
                // hinting; la linea di riduci a icona con l'hinting "GDI classico", su una riga sola invece che sfumata su due.
                m_smoothParams = nullptr;
                m_snappedParams = nullptr;
                m_dwrite->CreateCustomRenderingParams(1.0f, 0.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, m_smoothParams.put());
                m_dwrite->CreateCustomRenderingParams(1.0f, 0.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_GDI_CLASSIC, m_snappedParams.put());
                m_brush = nullptr;
            }
            if (!m_brush)
            {
                winrt::check_hresult(m_target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), m_brush.put()));
            }
            // Dimensione intera come DWM (12 px al 125%: glifi di 12 px con tratti di 1 px, 12,5 li sfumerebbe a 2).
            const float size = std::floor(kGlyphSizeDip * static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f);
            if (!m_format || size != m_formatSize)
            {
                m_format = nullptr;
                winrt::check_hresult(m_dwrite->CreateTextFormat(m_fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                                DWRITE_FONT_STRETCH_NORMAL, size, L"", m_format.put()));
                m_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                m_formatSize = size;
            }
            return true;
        }
        catch (...)
        {
            NPX_LOG(L"CaptionButtons: Direct2D non disponibile (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            return false;
        }
    }

    void CaptionButtons::Paint()
    {
        if (!m_hwnd || !m_main)
        {
            return;
        }
        const int width = m_rect.right - m_rect.left;
        const int height = m_rect.bottom - m_rect.top;
        if (width <= 0 || height <= 0)
        {
            return;
        }
        const bool maximized = IsZoomed(m_main) != FALSE;
        const int button = ButtonPx();
        const Drawn state{ width, height, button, m_hot, m_pressed, m_glyphCenter, maximized, m_active, m_dark };
        if (m_haveDrawn && state == m_drawn)
        {
            return;
        }
        if (!EnsureGraphics())
        {
            return;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // dall'alto
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HDC screen = GetDC(nullptr);
        const HDC memory = CreateCompatibleDC(screen);
        const HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool painted = false;
        if (memory && dib)
        {
            const HGDIOBJ previous = SelectObject(memory, dib);
            const RECT bounds{ 0, 0, width, height };
            if (SUCCEEDED(m_target->BindDC(memory, &bounds)))
            {
                const Palette& palette = m_dark ? kDark : kLight;
                m_target->BeginDraw();
                // Alfa 1/255 ovunque, invisibile: i pixel di una finestra layered del tutto trasparenti non ricevono il mouse
                // (passerebbe alla finestra sotto), e i pulsanti risponderebbero solo sopra i glifi.
                m_target->Clear(D2D1::ColorF(0, 0, 0, 1.0f / 255.0f));
                for (int index = 0; index < 3; ++index)
                {
                    // Da destra: chiudi in fondo, ingrandisci, riduci (il primo prende gli eventuali pixel in più).
                    const int right = width - (2 - index) * button;
                    const int left = index == 0 ? 0 : right - button;
                    const D2D1_RECT_F rect{ static_cast<float>(left), 0.0f, static_cast<float>(right), static_cast<float>(height) };
                    const bool pressed = m_pressed == index && m_hot == index;
                    const bool hot = !pressed && m_pressed < 0 && m_hot == index;
                    UINT32 background = 0;
                    UINT32 glyph = m_active ? palette.glyph : palette.glyphInactive;
                    if (index == 2 && (hot || pressed))
                    {
                        background = pressed ? kClosePressedBackground : kCloseHoverBackground;
                        glyph = pressed ? kClosePressedGlyph : kCloseHoverGlyph;
                    }
                    else if (hot)
                    {
                        background = palette.hoverBackground;
                        glyph = palette.hoverGlyph;
                    }
                    else if (pressed)
                    {
                        background = palette.pressedBackground;
                        glyph = palette.pressedGlyph;
                    }
                    if (background >> 24)
                    {
                        m_brush->SetColor(ToColor(background));
                        m_target->FillRectangle(rect, m_brush.get());
                    }
                    const wchar_t symbol = index == 0 ? kMinimizeGlyph : index == 1 ? (maximized ? kRestoreGlyph : kMaximizeGlyph) : kCloseGlyph;
                    m_brush->SetColor(ToColor(glyph));
                    // Riquadro del glifo centrato sulla riga (DirectWrite lo centra nel riquadro).
                    const D2D1_RECT_F glyphRect{ rect.left, m_glyphCenter - 32.0f, rect.right, m_glyphCenter + 32.0f };
                    const bool snapped = index == 0 && m_snappedParams;
                    if (IDWriteRenderingParams* params = snapped ? m_snappedParams.get() : m_smoothParams.get())
                    {
                        m_target->SetTextRenderingParams(params);
                    }
                    m_target->DrawText(&symbol, 1, m_format.get(), glyphRect, m_brush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                       snapped ? DWRITE_MEASURING_MODE_GDI_CLASSIC : DWRITE_MEASURING_MODE_NATURAL);
                }
                const HRESULT hr = m_target->EndDraw();
                if (hr == D2DERR_RECREATE_TARGET)
                {
                    m_target = nullptr;
                }
                painted = SUCCEEDED(hr);
            }
            if (painted)
            {
                POINT source{};
                SIZE size{ width, height };
                BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                painted = UpdateLayeredWindow(m_hwnd, screen, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA) != FALSE;
            }
            SelectObject(memory, previous);
        }
        if (dib)
        {
            DeleteObject(dib);
        }
        if (memory)
        {
            DeleteDC(memory);
        }
        ReleaseDC(nullptr, screen);
        if (painted)
        {
            m_drawn = state;
            m_haveDrawn = true;
        }
    }

    LRESULT CALLBACK CaptionButtons::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        const lifetime::CallScope call;
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW const*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<CaptionButtons*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || (self->m_hwnd && self->m_hwnd != hwnd))
        {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        if (!self->m_hwnd)
        {
            self->m_hwnd = hwnd; // durante CreateWindowEx
        }
        return self->OnMessage(message, wParam, lParam);
    }

    LRESULT CaptionButtons::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        const HWND hwnd = m_hwnd;
        switch (message)
        {
        case WM_NCHITTEST:
        {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &point);
            // Riga in cima con la finestra non ingrandita: il bordo superiore (ridimensiona), come sopra i pulsanti di DWM.
            if (!IsZoomed(m_main) && point.y + m_rect.top < 1)
            {
                return HTTRANSPARENT;
            }
            switch (ButtonAt(point))
            {
            case 0:
                return HTMINBUTTON;
            case 1:
                return HTMAXBUTTON;
            case 2:
                return HTCLOSE;
            default:
                return HTTRANSPARENT;
            }
        }
        case WM_NCMOUSEMOVE:
        {
            if (!m_tracking)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
                m_tracking = TrackMouseEvent(&track) != FALSE;
            }
            const int button = ButtonFromHit(wParam);
            if (button != m_hot && m_pressed < 0)
            {
                m_hot = button;
                HideTip();
                if (button >= 0)
                {
                    SetTimer(hwnd, kTipTimer, kTipDelayMs, nullptr);
                }
                Paint();
            }
            return 0;
        }
        case WM_NCMOUSELEAVE:
            m_tracking = false;
            if (m_pressed < 0 && m_hot >= 0)
            {
                m_hot = -1;
                HideTip();
                Paint();
            }
            return 0;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (const int button = ButtonFromHit(wParam); button >= 0)
            {
                Press(button);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (m_pressed >= 0)
            {
                // Premuto e trascinato fuori: il pulsante non è più evidenziato (e al rilascio non fa niente).
                const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                const int hot = ButtonAt(point) == m_pressed ? m_pressed : -1;
                if (hot != m_hot)
                {
                    m_hot = hot;
                    Paint();
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (m_pressed >= 0)
            {
                const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                const int pressed = m_pressed;
                m_pressed = -1;
                m_hot = -1;
                ReleaseCapture();
                Paint();
                if (ButtonAt(point) == pressed)
                {
                    Invoke(pressed);
                }
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (m_pressed >= 0 && reinterpret_cast<HWND>(lParam) != hwnd)
            {
                m_pressed = -1;
                m_hot = -1;
                Paint();
            }
            return 0;
        case WM_NCRBUTTONDOWN:
            return 0;
        case WM_NCRBUTTONUP:
            // Menu di sistema dove si è cliccato (lo apre la finestra principale, con le voci giuste per il suo stato).
            HideTip();
            PostMessageW(m_main, kPopupSystemMenu, 0, lParam);
            return 0;
        case WM_TIMER:
            if (wParam == kTipTimer)
            {
                KillTimer(hwnd, kTipTimer);
                ShowTip();
                return 0;
            }
            if (wParam == kThemeTimer)
            {
                KillTimer(hwnd, kThemeTimer);
                m_dark = ReadDark();
                m_haveDrawn = false;
                Paint();
                return 0;
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (m_hwnd == hwnd)
            {
                m_hwnd = nullptr; // distrutta con la finestra principale
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool CaptionButtons::UnregisterWindowClass()
    {
        WNDCLASSEXW info{ sizeof(info) };
        if (!GetClassInfoExW(ThisModule(), kClassName, &info))
        {
            return true; // mai registrata (o già tolta)
        }
        return UnregisterClassW(kClassName, ThisModule()) != FALSE;
    }
}
