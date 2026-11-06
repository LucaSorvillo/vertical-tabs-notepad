#pragma once

#include "Settings.h"

namespace npx
{
    class VisualTreeWatcher;
    class VerticalTabs;
    class BackToTop;
    class ReopenTab;
    class ScrollBarHover;
    class WheelScroll;
    class ScrollRangeFix;

    // Cuore della mod: avvia la diagnostica XAML, tiene traccia delle finestre di Notepad
    // (una per ogni TabsBar) e applica le feature sul thread UI di ciascuna.
    class ModCore
    {
    public:
        static ModCore& Instance();

        // Thread avviato da DllMain: attende il runtime WinUI, attiva la diagnostica XAML
        // e poi resta in ascolto di comandi (dump dell'albero) e modifiche alle impostazioni.
        static DWORD WINAPI BootstrapThreadProc(void* parameter);

        // Chiamato dal TAP quando il runtime XAML ci consegna IXamlDiagnostics.
        void OnXamlDiagnosticsReady(winrt::com_ptr<IXamlDiagnostics> const& diagnostics);

        // Chiamati dal VisualTreeWatcher sul thread UI che possiede l'elemento.
        void OnElementAdded(VisualTreeWatcher const& watcher, ParentChildRelation const& relation, VisualElement const& element);
        void OnElementRemoved(VisualTreeWatcher const& watcher, ParentChildRelation const& relation, VisualElement const& element);

        Settings CurrentSettings() const;

        // Barre dei menu del Blocco note (NotepadXamlUI.MainMenuBar, ognuna nella propria isola XAML)
        // che appartengono al thread chiamante: ogni finestra cerca tra queste la sua.
        std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> MenuBarsOnCurrentThread();
        // ScrollViewer chiamati "ScrollViewer" del thread chiamante: tra questi c'è la barra di scorrimento
        // verticale dell'editor di ogni finestra (nella propria isola XAML).
        std::vector<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> ScrollViewersOnCurrentThread();
        // Tutte le ScrollBar del thread chiamante (editor, menu, elenchi, il nostro pannello).
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar> ScrollBarsOnCurrentThread();

    private:
        struct WindowEntry
        {
            InstanceHandle tabsBarHandle{};
            winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> tabsBar;
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
            std::shared_ptr<VerticalTabs> verticalTabs;             // usati solo dal thread del dispatcher
            std::shared_ptr<BackToTop> backToTop;
            std::shared_ptr<ReopenTab> reopenTab;
            std::shared_ptr<ScrollBarHover> scrollBarHover;
            std::shared_ptr<WheelScroll> wheelScroll;
            std::shared_ptr<ScrollRangeFix> scrollRangeFix;
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer layoutTimer{ nullptr };
        };

        struct MenuBarEntry
        {
            InstanceHandle handle{};
            winrt::weak_ref<winrt::Microsoft::UI::Xaml::FrameworkElement> element; // si risolve solo sul suo thread
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
        };

        struct ScrollViewerEntry
        {
            InstanceHandle handle{};
            winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> element; // si risolve solo sul suo thread
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
        };

        struct ScrollBarEntry
        {
            winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar> element; // si risolve solo sul suo thread
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
        };

        ModCore() = default;

        HRESULT StartXamlDiagnostics();
        void OnMenuBarAdded(VisualTreeWatcher const& watcher, VisualElement const& element);
        void OnScrollViewerAdded(VisualTreeWatcher const& watcher, VisualElement const& element);
        void OnScrollBarAdded(VisualTreeWatcher const& watcher, VisualElement const& element);
        // Chiude le feature di una finestra (sul suo thread).
        static void ReleaseFeatures(WindowEntry& entry, bool restore);
        void BackgroundLoop();
        bool ReloadSettings();
        void ApplySettingsToAllWindows();
        void DumpAllWindows();
        // Riapre la scheda chiusa nella finestra attiva (comando della tray): ogni finestra si
        // riconosce da sé, solo quella in primo piano agisce.
        void ReopenTabInActiveWindow();
        void AttachFeatures(std::shared_ptr<WindowEntry> const& entry);
        void CheckRemovedWindow(std::shared_ptr<WindowEntry> const& entry);
        void ForgetDeadWindow(std::shared_ptr<WindowEntry> const& entry);
        std::vector<std::shared_ptr<WindowEntry>> SnapshotWindows() const;

        mutable std::mutex m_lock;
        Settings m_settings;
        std::string m_devWindows; // contenuto di dev-windows.txt (per accorgersi delle modifiche)
        winrt::com_ptr<VisualTreeWatcher> m_watcher;
        std::vector<std::shared_ptr<WindowEntry>> m_windows;
        std::vector<MenuBarEntry> m_menuBars;
        std::vector<ScrollViewerEntry> m_scrollViewers;
        std::vector<ScrollBarEntry> m_scrollBars;
    };
}
