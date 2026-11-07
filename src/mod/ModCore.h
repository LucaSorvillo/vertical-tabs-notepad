#pragma once

#include "Discovery.h"
#include "Settings.h"

namespace npx
{
    class VerticalTabs;
    class BackToTop;
    class ReopenTab;
    class ScrollBarHover;
    class WheelScroll;
    class ScrollRangeFix;

    // Cuore della mod: trova le finestre del Blocco note (una per ogni TabsBar) e le loro parti con gli agganci di
    // Discovery, applica le feature sul thread UI di ciascuna e, con "Esci" della tray, toglie tutto e fa uscire la DLL
    // dal processo (TryUnload).
    class ModCore
    {
    public:
        static ModCore& Instance();

        // Thread avviato da DllMain: attende il runtime WinUI, installa gli agganci e poi resta in ascolto di comandi
        // (dump dell'albero, riapri scheda) e modifiche alle impostazioni. Con "Esci" della tray scarica la DLL.
        static DWORD WINAPI BootstrapThreadProc(void* parameter);

        Settings CurrentSettings() const;

        // Barre dei menu del Blocco note (NotepadXamlUI.MainMenuBar, ognuna nella propria isola XAML)
        // che appartengono al thread chiamante: ogni finestra cerca tra queste la sua.
        std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> MenuBarsOnCurrentThread();
        // ScrollViewer "ScrollViewer" delle barre di scorrimento dell'editor (NotepadXamlUI.ScrollBar) del thread
        // chiamante: ogni finestra cerca tra questi i suoi (ognuno nella propria isola XAML).
        std::vector<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> ScrollViewersOnCurrentThread();
        // ScrollBar di WinUI del thread chiamante: negli alberi delle isole note (barra delle schede, barra dei menu, barre
        // di scorrimento dell'editor, il nostro pannello) e nei loro popup aperti (menu, elenchi a discesa). full=false:
        // solo popup e barre dell'editor (il controllo frequente, economico).
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar> ScrollBarsOnCurrentThread(bool full);

    private:
        struct WindowEntry
        {
            uint64_t id = 0;     // identità della TabsBar (per il log)
            HWND main = nullptr; // finestra principale del Blocco note
            winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> tabsBar;
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
            std::shared_ptr<VerticalTabs> verticalTabs;             // usati solo dal thread del dispatcher
            std::shared_ptr<BackToTop> backToTop;
            std::shared_ptr<ReopenTab> reopenTab;
            std::shared_ptr<ScrollBarHover> scrollBarHover;
            std::shared_ptr<WheelScroll> wheelScroll;
            std::shared_ptr<ScrollRangeFix> scrollRangeFix;
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer layoutTimer{ nullptr };
            bool closing = false; // si sta staccando (scaricamento): i lavori nostri già in coda non la toccano
        };

        struct MenuBarEntry
        {
            uint64_t id = 0; // identità della MainMenuBar
            winrt::weak_ref<winrt::Microsoft::UI::Xaml::FrameworkElement> element; // si risolve solo sul suo thread
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
        };

        struct ScrollViewerEntry
        {
            uint64_t id = 0; // identità della NotepadXamlUI.ScrollBar che lo contiene
            winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> element; // si risolve solo sul suo thread
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
        };

        // Ricerca UI Automation di una finestra non ancora trovata (dal thread di controllo).
        struct ProbeState
        {
            ULONGLONG first = 0;
            ULONGLONG last = 0;
            int attempts = 0;
        };

        ModCore() = default;

        // Ricerca delle parti delle finestre (vedi Discovery.h).
        static void OnElementFound(discovery::Kind kind, winrt::Microsoft::UI::Xaml::FrameworkElement const& element) noexcept;
        bool IsKnownScrollBar(winrt::Microsoft::UI::Xaml::FrameworkElement const& bar);
        // false se lo stesso elemento si è già presentato negli ultimi 3 s (un lavoro di registrazione è già in coda).
        bool FirstNoticeOf(winrt::Microsoft::UI::Xaml::FrameworkElement const& element);
        void Register(discovery::Kind kind, winrt::Microsoft::UI::Xaml::FrameworkElement const& element);
        void RegisterWindow(winrt::NotepadXamlUI::TabsBar const& tabsBar);
        void RegisterMenuBar(winrt::Microsoft::UI::Xaml::FrameworkElement const& mainMenuBar);
        void RegisterScrollBar(winrt::Microsoft::UI::Xaml::FrameworkElement const& bar);
        void StartDiscovery();
        // Finestre visibili che nessun aggancio ha presentato (aperte prima della mod) e isole appena comparse: ricerca UI
        // Automation. now: subito, senza aspettare che la finestra si assesti.
        void ProbeUnmanaged(bool now);

        // Chiude le feature di una finestra (sul suo thread).
        static void ReleaseFeatures(WindowEntry& entry, bool restore);
        // true quando la DLL deve uscire (tray chiusa con "Esci" e TryUnload riuscito).
        bool BackgroundLoop();
        bool ReloadSettings();
        void ApplySettingsToAllWindows();
        void DumpAllWindows();
        // Riapre la scheda chiusa nella finestra attiva (comando della tray): ogni finestra si
        // riconosce da sé, solo quella in primo piano agisce.
        void ReopenTabInActiveWindow();
        void AttachFeatures(std::shared_ptr<WindowEntry> const& entry);
        void ForgetDeadWindow(std::shared_ptr<WindowEntry> const& entry);
        std::vector<std::shared_ptr<WindowEntry>> SnapshotWindows() const;

        // Scaricamento ("Esci" della tray). Sul thread di ogni finestra, mai annidato nel nostro codice: feature staccate
        // (tutto come prima della mod) e finestre, barre dei menu e barre di scorrimento di quel thread dimenticate. Niente
        // se nel frattempo la tray è ripartita (epoch diverso).
        void TeardownThread(int epoch);
        // Dal thread di controllo: toglie tutto; true quando la DLL può uscire. Si riprova finché non riesce.
        bool TryUnload();
        // La tray è ripartita prima che la DLL uscisse: si riparte da capo.
        void Resume();

        mutable std::mutex m_lock;
        Settings m_settings;
        std::string m_devWindows; // contenuto di dev-windows.txt (per accorgersi delle modifiche)
        std::vector<std::shared_ptr<WindowEntry>> m_windows;
        std::vector<MenuBarEntry> m_menuBars;
        std::vector<ScrollViewerEntry> m_scrollViewers;
        std::unordered_map<uint64_t, ULONGLONG> m_recentNotices; // elemento presentato -> quando (vedi FirstNoticeOf)

        // Solo dal thread di controllo.
        std::unordered_map<HWND, ProbeState> m_probes;
        std::unordered_map<HWND, bool> m_islands; // isole delle finestre gestite: visibile all'ultimo controllo
        std::vector<winrt::Microsoft::UI::Dispatching::DispatcherQueue> m_seenDispatchers;
        bool m_tornDown = false; // finestre staccate e agganci tolti, la DLL aspetta di poter uscire
        // Distacco in corso su ogni thread (finished dal thread della finestra).
        struct TeardownState
        {
            int epoch = 0;
            ULONGLONG created = 0;
            std::atomic<bool> finished{ false };
        };
        std::vector<std::pair<winrt::Microsoft::UI::Dispatching::DispatcherQueue, std::shared_ptr<TeardownState>>> m_teardowns;
        std::atomic<int> m_unloadEpoch{ 0 }; // cresce a ogni Resume
    };
}
