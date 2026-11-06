#pragma once

#include <d2d1.h>
#include <dwrite.h>

namespace npx
{
    // Pulsanti della finestra (riduci a icona, ingrandisci/ripristina, chiudi) alti quanto la prima riga, come in Chrome e nel
    // Terminale di Windows. Con la riga del titolo nascosta (vedi WindowLayout) la prima riga è la barra dei menu (44 px al
    // 125%, dalla cima della finestra), mentre i pulsanti di DWM restano alti 30 DIP. Stesso componente di VTabsExplorer.
    //
    // - I pulsanti di DWM si nascondono con le opzioni del tema della zona non client: SetWindowThemeAttribute(WTA_NONCLIENT)
    //   con il flag non documentato 0x10000, lo stesso che usa il Windows App SDK per la sua barra del titolo (dal disassemblato
    //   di Microsoft.UI.Windowing.Core.dll: flag 0x10007 con maschera 0x1000F, tolto con flag 0). Tolto il flag tornano come
    //   prima; stili, Mica e menu di sistema non cambiano.
    // - Al loro posto c'è una finestra figlia layered (alfa per pixel: sotto si vede la Mica della barra) disegnata con
    //   Direct2D/DirectWrite: glifi di Segoe Fluent Icons e colori dei pulsanti di Windows 11 (tema chiaro/scuro, finestra
    //   attiva o no, chiudi rosso).
    // - All'hit test risponde HTMINBUTTON/HTMAXBUTTON/HTCLOSE come i pulsanti veri, così Windows 11 mostra i layout di
    //   ancoraggio passando su ingrandisci. Il clic lo segue la finestra stessa (cattura del mouse): al rilascio sullo stesso
    //   pulsante manda WM_SYSCOMMAND alla finestra principale. Il clic destro apre il menu di sistema. Con la finestra non
    //   ingrandita la riga di 1 px in cima è trasparente all'hit test: lì il bordo superiore ridimensiona come prima.
    //   Suggerimenti con i nomi delle voci del menu di sistema (nella lingua di Windows).
    // Va usata sul thread della finestra principale.
    class CaptionButtons
    {
    public:
        CaptionButtons() = default;
        ~CaptionButtons();
        CaptionButtons(CaptionButtons const&) = delete;
        CaptionButtons& operator=(CaptionButtons const&) = delete;

        // Pulsanti a destra della riga 'row' (coordinate client di 'main': la riga del contenuto, es. da sotto il bordo
        // superiore al fondo della barra), dal suo bordo destro: dalla cima della finestra al fondo della riga (da ingrandita
        // dalla cima dello schermo), con i glifi centrati sulla riga. Rettangolo vuoto: via i nostri, tornano quelli di DWM.
        // Idempotente ed economica: si chiama a ogni layout.
        void Update(HWND main, RECT const& row);
        // Toglie i nostri; restoreSystem=false se la finestra si sta chiudendo (non si tocca più).
        void Remove(bool restoreSystem = true);
        // Finestra attiva o no (WM_NCACTIVATE della finestra principale).
        void SetActive(bool active);
        // Tema o colori di sistema cambiati: rilegge il tema e ridisegna, poco dopo (quando la finestra si è aggiornata).
        void ThemeChanged();
        // Ricontrolla che i pulsanti di DWM siano ancora nascosti e il tema della finestra (controllo periodico; al
        // massimo due volte al secondo).
        void Verify();
        HWND Window() const { return m_hwnd; }
        // Solo per le prove (comando "caption"): pulsante evidenziato/premuto come col mouse (-1 = nessuno), azione di un pulsante.
        void DevSetState(int hot, int pressed);
        void DevInvoke(int button) { Invoke(button); }

        static constexpr double kButtonWidthDip = 46.0; // come i pulsanti di Windows 11

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT OnMessage(UINT message, WPARAM wParam, LPARAM lParam);

        bool Create(RECT const& rect);
        void HideSystemButtons(bool hide);
        int ButtonPx() const;
        int VisibleTop() const;
        // Pulsante sotto un punto (coordinate client della nostra finestra): 0 riduci, 1 ingrandisci/ripristina, 2 chiudi; -1 nessuno.
        int ButtonAt(POINT point) const;
        bool ReadDark() const;
        void Press(int button);
        void Invoke(int button);
        void ShowTip();
        void HideTip();
        std::wstring TipText(int button) const;
        bool EnsureGraphics();
        void Paint();

        HWND m_main = nullptr;
        HWND m_hwnd = nullptr;
        HWND m_tooltip = nullptr;
        RECT m_rect{};          // nella finestra principale
        float m_glyphCenter = 0; // centro verticale dei glifi (coordinate della nostra finestra): il centro della riga
        bool m_systemHidden = false;
        bool m_active = true;
        bool m_dark = true;
        bool m_tracking = false; // TrackMouseEvent in corso
        int m_hot = -1;
        int m_pressed = -1;
        bool m_removing = false; // Remove in corso (vedi lì)
        ULONGLONG m_lastVerify = 0;

        // Ultimo stato disegnato (si ridisegna solo se cambia)
        struct Drawn
        {
            int width = 0, height = 0, button = 0, hot = -1, pressed = -1;
            float glyphCenter = 0;
            bool maximized = false, active = false, dark = false;
            bool operator==(Drawn const&) const = default;
        };
        Drawn m_drawn{};
        bool m_haveDrawn = false;

        winrt::com_ptr<ID2D1Factory> m_d2d;
        winrt::com_ptr<IDWriteFactory> m_dwrite;
        winrt::com_ptr<ID2D1DCRenderTarget> m_target;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brush;
        winrt::com_ptr<IDWriteRenderingParams> m_smoothParams;  // glifi senza hinting
        winrt::com_ptr<IDWriteRenderingParams> m_snappedParams; // linea di riduci a icona agganciata ai pixel
        winrt::com_ptr<IDWriteTextFormat> m_format;
        float m_formatSize = 0;
        std::wstring m_fontFamily;
    };
}
