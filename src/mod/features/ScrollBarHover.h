#pragma once

#include "Settings.h"

namespace npx
{
    // Barre di scorrimento del Blocco note che si allargano subito sotto il mouse, come quelle di Esplora file.
    //
    // Le ScrollBar di WinUI 3 (modello di sistema) hanno due aspetti: sottile (indicatore) e largo, con frecce e binario,
    // quando il mouse ci sta sopra (stato "Expanded" del gruppo "ConsciousStates", scelto subito da ScrollBar::
    // ChangeVisualState). Le animazioni di quello stato però partono dopo ScrollBarExpandBeginTime = 0,4 s (BeginTime nel
    // modello): per questo le barre del Blocco note si allargano in ritardo, mentre quelle di Esplora file (DirectUI)
    // subito. Qui, in ogni barra di questa finestra che ha il suo modello, il BeginTime delle animazioni di "Expanded" ed
    // "ExpandedWithoutAnimation" passa a 0. Durata dell'animazione e ritardo prima di stringersi (uscendo col mouse)
    // restano quelli di sistema.
    //
    // Sempre attiva: l'opzione nella tray è stata tolta su richiesta dell'utente. I valori originali si ricordano e tornano
    // solo se la finestra esce dalla mod (gate della modalità sviluppo, dev-windows.txt) o la mod si stacca.
    //
    // Le barre del thread della finestra che la mod conosce (ModCore::ScrollBarsOnCurrentThread): editor, barra delle schede,
    // barra dei menu, il nostro pannello e i popup aperti di queste isole (menu, elenchi). Vive e viene usata interamente
    // sul thread UI della finestra.
    class ScrollBarHover
    {
    public:
        explicit ScrollBarHover(winrt::NotepadXamlUI::TabsBar const& tabsBar) : m_tabsBar(winrt::make_weak(tabsBar))
        {
        }
        ScrollBarHover(ScrollBarHover const&) = delete;
        ScrollBarHover& operator=(ScrollBarHover const&) = delete;

        // ScrollBar vive sul thread della finestra.
        // full=false: solo le barre che cambiano spesso (popup, editor); full=true anche il resto degli alberi.
        std::function<std::vector<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar>(bool full)> scrollBarsProvider;

        void Apply(Settings const& settings);
        // Controllo periodico (500 ms): barre nuove o che hanno appena ricevuto il modello.
        void OnTimer();
        // restore: rimette i ritardi di sistema; altrimenti (finestra in chiusura) niente XAML.
        void Detach(bool restore);

    private:
        // Animazione adattata, ritrovata dallo stato e dalla posizione nel suo storyboard: un weak_ref all'oggetto Timeline
        // non basta, perché WinUI ne rilascia l'involucro (peer) quando nessuno lo tiene e il riferimento scade (il
        // ripristino non trovava più niente).
        struct Patched
        {
            std::wstring state;
            uint32_t index = 0;
            winrt::Windows::Foundation::TimeSpan original{};
        };
        struct Bar
        {
            winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar> bar;
            std::vector<Patched> timelines;
            bool done = false; // modello visto e adattato (o senza niente da adattare)
        };

        HWND MainWindow();
        void PatchAll(bool full);
        int RestoreAll(); // animazioni rimesse
        // false se il modello non c'è ancora o un'animazione non si è lasciata cambiare (si riprova).
        bool Patch(winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar const& bar, Bar& entry);

        winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> m_tabsBar;
        HWND m_main = nullptr;
        bool m_enabled = false;
        std::vector<Bar> m_bars;
        bool m_loggedFirst = false;
        unsigned m_ticks = 0; // controlli periodici: uno su quattro (2 s) guarda gli alberi interi
    };
}
