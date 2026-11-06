#include "pch.h"
#include "ModCore.h"

#include "Glass.h"
#include "Log.h"
#include "Shared.h"
#include "TreeDump.h"
#include "VisualTreeWatcher.h"
#include "features/BackToTop.h"
#include "features/ReopenTab.h"
#include "features/ScrollBarHover.h"
#include "features/ScrollRangeFix.h"
#include "features/VerticalTabs.h"
#include "features/WheelScroll.h"

extern HMODULE g_module;

namespace npx
{
    namespace
    {
        using PFN_INITIALIZE_XAML_DIAGNOSTICS_EX = HRESULT(WINAPI*)(LPCWSTR endPointName, DWORD pid, LPCWSTR wszDllXamlDiagnostics,
                                                                     LPCWSTR wszTAPDllName, CLSID tapClsid, LPCWSTR wszInitializationData);

        using winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority;

        constexpr std::wstring_view kTabsBarType = L"NotepadXamlUI.TabsBar";
        constexpr std::wstring_view kMainMenuBarType = L"NotepadXamlUI.MainMenuBar";
        // La barra di scorrimento verticale dell'editor è uno ScrollViewer di sistema con questo nome,
        // nella propria isola (lo stesso nome lo hanno le parti interne di elenchi e striscia schede).
        constexpr std::wstring_view kScrollViewerType = L"Microsoft.UI.Xaml.Controls.ScrollViewer";
        constexpr std::wstring_view kScrollViewerName = L"ScrollViewer";
        // Barre di scorrimento di WinUI (dentro ogni ScrollViewer): "subito larghe sotto il mouse" (ScrollBarHover).
        constexpr std::wstring_view kScrollBarType = L"Microsoft.UI.Xaml.Controls.Primitives.ScrollBar";

        std::wstring_view BstrView(BSTR value)
        {
            return value ? std::wstring_view{ value, SysStringLen(value) } : std::wstring_view{};
        }

        // Impedisce che la DLL venga scaricata: il runtime XAML conserva puntatori al nostro codice.
        void PinModule()
        {
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&PinModule), &pinned);
        }

        bool WaitForModule(const wchar_t* name, ULONGLONG timeoutMs)
        {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            while (!GetModuleHandleW(name))
            {
                if (GetTickCount64() > deadline)
                {
                    return false;
                }
                Sleep(100);
            }
            return true;
        }
    }

    ModCore& ModCore::Instance()
    {
        // Mai distrutto: all'uscita del processo il runtime XAML è già stato smontato.
        static ModCore* instance = new ModCore();
        return *instance;
    }

    DWORD WINAPI ModCore::BootstrapThreadProc(void*)
    {
        log::Init();
        PinModule();

        const std::wstring image = ProcessImagePath(GetCurrentProcessId());
        if (!IsStoreNotepadPath(image))
        {
            NPX_LOG(L"Processo non supportato (%s): la mod resta inattiva.", image.c_str());
            return 0;
        }

        // Una sola copia attiva per processo: se il launcher, dopo una ricompilazione, caricasse una
        // seconda copia della DLL (da un'altra cartella), avremmo due pannelli in conflitto.
        const std::wstring mutexName = L"Local\\VTabsNotepadMod.Active." + std::to_wstring(GetCurrentProcessId());
        const HANDLE activeMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (activeMutex && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            NPX_LOG(L"Un'altra copia di VTabsNotepadMod è già attiva in questo processo: questa resta inattiva.");
            CloseHandle(activeMutex);
            return 0;
        }
        // L'handle resta aperto per tutta la vita del processo (è il "segnaposto" della copia attiva).

        // Zona vetro della finestra (riga del titolo nascosta): finché nessuna finestra ha un limite, l'aggancio la
        // lascia com'è.
        glass::Install();

        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        ModCore& core = Instance();
        core.ReloadSettings();
        NPX_LOG(L"VTabsNotepadMod caricata in %s", image.c_str());

        if (!WaitForModule(L"Microsoft.UI.Xaml.dll", 60'000) || !WaitForModule(L"Microsoft.Internal.FrameworkUdk.dll", 10'000))
        {
            NPX_LOG(L"Runtime WinUI non presente nel processo: niente da fare.");
            return 0;
        }

        if (FAILED(core.StartXamlDiagnostics()))
        {
            return 0;
        }

        core.BackgroundLoop();
        return 0;
    }

    HRESULT ModCore::StartXamlDiagnostics()
    {
        const HMODULE udk = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
        const auto initialize = reinterpret_cast<PFN_INITIALIZE_XAML_DIAGNOSTICS_EX>(GetProcAddress(udk, "InitializeXamlDiagnosticsEx"));
        if (!initialize)
        {
            NPX_LOG(L"InitializeXamlDiagnosticsEx non trovata in Microsoft.Internal.FrameworkUdk.dll");
            return E_NOINTERFACE;
        }

        wchar_t dllPath[MAX_PATH]{};
        GetModuleFileNameW(g_module, dllPath, ARRAYSIZE(dllPath));

        // Il runtime XAML carica di nuovo questa stessa DLL come TAP (il modulo è già in memoria,
        // quindi ottiene lo stesso handle) e ci chiama tramite DllGetClassObject + SetSite.
        HRESULT hr = E_FAIL;
        for (int attempt = 1; attempt <= 60; ++attempt)
        {
            hr = initialize(L"WinUIVisualDiagConnection1", GetCurrentProcessId(), L"", dllPath, kTapClsid, nullptr);
            if (SUCCEEDED(hr))
            {
                NPX_LOG(L"InitializeXamlDiagnosticsEx riuscita (tentativo %d).", attempt);
                return hr;
            }
            NPX_VLOG(L"InitializeXamlDiagnosticsEx tentativo %d: 0x%08X", attempt, static_cast<unsigned>(hr));
            Sleep(500);
        }

        NPX_LOG(L"InitializeXamlDiagnosticsEx fallita: 0x%08X", static_cast<unsigned>(hr));
        return hr;
    }

    void ModCore::OnXamlDiagnosticsReady(winrt::com_ptr<IXamlDiagnostics> const& diagnostics)
    {
        std::scoped_lock lock(m_lock);
        if (m_watcher)
        {
            NPX_LOG(L"SetSite ricevuto una seconda volta: ignorato.");
            return;
        }
        m_watcher = winrt::make_self<VisualTreeWatcher>(diagnostics);
        m_watcher->Start();
        NPX_LOG(L"Diagnostica XAML pronta: in ascolto dell'albero visuale.");
    }

    void ModCore::OnElementAdded(VisualTreeWatcher const& watcher, ParentChildRelation const& relation, VisualElement const& element)
    {
        const std::wstring_view type = BstrView(element.Type);
        if (log::IsVerbose() && (relation.Parent == 0 || type.starts_with(L"NotepadXamlUI.")))
        {
            const std::wstring_view name = BstrView(element.Name);
            NPX_LOG(L"+ %.*s #%.*s (handle %llx, parent %llx)", static_cast<int>(type.size()), type.data(),
                    static_cast<int>(name.size()), name.data(), element.Handle, relation.Parent);
        }

        if (type == kMainMenuBarType)
        {
            OnMenuBarAdded(watcher, element);
            return;
        }
        if (type == kScrollViewerType)
        {
            if (BstrView(element.Name) == kScrollViewerName)
            {
                OnScrollViewerAdded(watcher, element);
            }
            return;
        }
        if (type == kScrollBarType)
        {
            OnScrollBarAdded(watcher, element);
            return;
        }
        if (type != kTabsBarType)
        {
            return;
        }

        const auto tabsBar = watcher.FromHandle(element.Handle).try_as<winrt::NotepadXamlUI::TabsBar>();
        if (!tabsBar)
        {
            return;
        }

        std::shared_ptr<WindowEntry> entry;
        size_t count = 0;
        {
            std::scoped_lock lock(m_lock);
            for (auto const& existing : m_windows)
            {
                // Già gestita (notifica ripetuta per lo stesso elemento).
                if (existing->tabsBarHandle == element.Handle)
                {
                    return;
                }
            }
            entry = std::make_shared<WindowEntry>();
            entry->tabsBarHandle = element.Handle;
            entry->tabsBar = winrt::make_weak(tabsBar);
            entry->dispatcher = tabsBar.DispatcherQueue();
            m_windows.push_back(entry);
            count = m_windows.size();
        }
        NPX_LOG(L"Nuova finestra di Notepad (TabsBar %llx), finestre gestite: %zu", element.Handle, count);

        // Lascia finire la costruzione della finestra prima di modificarla.
        entry->dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [this, entry] {
            try
            {
                AttachFeatures(entry);
            }
            catch (...)
            {
                NPX_LOG(L"AttachFeatures: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            }
        });
    }

    void ModCore::OnMenuBarAdded(VisualTreeWatcher const& watcher, VisualElement const& element)
    {
        const auto menuBar = watcher.FromHandle(element.Handle).try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
        if (!menuBar)
        {
            return;
        }
        {
            std::scoped_lock lock(m_lock);
            for (auto const& existing : m_menuBars)
            {
                // Stesso handle sullo stesso thread: è lo stesso elemento (notifica ripetuta). Un handle
                // riusato dopo la distruzione di un elemento si riconosce dal riferimento ormai vuoto.
                if (existing.handle == element.Handle && existing.dispatcher.HasThreadAccess() && existing.element.get() == menuBar)
                {
                    return;
                }
            }
            m_menuBars.push_back({ element.Handle, winrt::make_weak(menuBar), menuBar.DispatcherQueue() });
        }
        // La finestra la trova e la adegua da sé (controllo periodico): l'ordine di creazione di barra
        // dei menu e barra schede non conta.
        NPX_LOG(L"Barra dei menu del Blocco note trovata (MainMenuBar %llx).", element.Handle);
    }

    void ModCore::OnScrollViewerAdded(VisualTreeWatcher const& watcher, VisualElement const& element)
    {
        const auto scrollViewer = watcher.FromHandle(element.Handle).try_as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();
        if (!scrollViewer)
        {
            return;
        }
        std::scoped_lock lock(m_lock);
        for (auto it = m_scrollViewers.begin(); it != m_scrollViewers.end();)
        {
            if (it->dispatcher.HasThreadAccess())
            {
                const auto existing = it->element.get();
                if (!existing)
                {
                    it = m_scrollViewers.erase(it); // distrutto: si fa pulizia
                    continue;
                }
                if (existing == scrollViewer)
                {
                    return; // notifica ripetuta per lo stesso elemento
                }
            }
            ++it;
        }
        m_scrollViewers.push_back({ element.Handle, winrt::make_weak(scrollViewer), scrollViewer.DispatcherQueue() });
    }

    void ModCore::OnScrollBarAdded(VisualTreeWatcher const& watcher, VisualElement const& element)
    {
        const auto scrollBar = watcher.FromHandle(element.Handle).try_as<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar>();
        if (!scrollBar)
        {
            return;
        }
        std::scoped_lock lock(m_lock);
        for (auto it = m_scrollBars.begin(); it != m_scrollBars.end();)
        {
            if (it->dispatcher.HasThreadAccess())
            {
                const auto existing = it->element.get();
                if (!existing)
                {
                    it = m_scrollBars.erase(it); // distrutta (menu chiusi, finestre chiuse): si fa pulizia
                    continue;
                }
                if (existing == scrollBar)
                {
                    return; // notifica ripetuta per lo stesso elemento
                }
            }
            ++it;
        }
        m_scrollBars.push_back({ winrt::make_weak(scrollBar), scrollBar.DispatcherQueue() });
    }

    std::vector<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar> ModCore::ScrollBarsOnCurrentThread()
    {
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar> result;
        std::scoped_lock lock(m_lock);
        for (auto it = m_scrollBars.begin(); it != m_scrollBars.end();)
        {
            if (!it->dispatcher.HasThreadAccess())
            {
                ++it; // di un'altra finestra: si risolve solo sul suo thread
                continue;
            }
            if (auto element = it->element.get())
            {
                result.push_back(std::move(element));
                ++it;
            }
            else
            {
                it = m_scrollBars.erase(it);
            }
        }
        return result;
    }

    std::vector<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> ModCore::ScrollViewersOnCurrentThread()
    {
        std::vector<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> result;
        std::scoped_lock lock(m_lock);
        for (auto it = m_scrollViewers.begin(); it != m_scrollViewers.end();)
        {
            if (!it->dispatcher.HasThreadAccess())
            {
                ++it; // di un'altra finestra: si risolve solo sul suo thread
                continue;
            }
            if (auto element = it->element.get())
            {
                result.push_back(std::move(element));
                ++it;
            }
            else
            {
                it = m_scrollViewers.erase(it);
            }
        }
        return result;
    }

    std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> ModCore::MenuBarsOnCurrentThread()
    {
        std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> result;
        std::scoped_lock lock(m_lock);
        for (auto it = m_menuBars.begin(); it != m_menuBars.end();)
        {
            if (!it->dispatcher.HasThreadAccess())
            {
                ++it; // di un'altra finestra: si risolve solo sul suo thread
                continue;
            }
            if (auto element = it->element.get())
            {
                result.push_back(std::move(element));
                ++it;
            }
            else
            {
                it = m_menuBars.erase(it); // distrutta (finestra chiusa)
            }
        }
        return result;
    }

    void ModCore::OnElementRemoved(VisualTreeWatcher const&, ParentChildRelation const&, VisualElement const& element)
    {
        if (BstrView(element.Type) != kTabsBarType)
        {
            return;
        }

        std::shared_ptr<WindowEntry> entry;
        {
            std::scoped_lock lock(m_lock);
            for (auto const& existing : m_windows)
            {
                if (existing->tabsBarHandle == element.Handle)
                {
                    entry = existing;
                    break;
                }
            }
        }
        if (!entry)
        {
            return;
        }

        // Si verifica a layout assestato che non sia solo un riposizionamento nell'albero.
        entry->dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [this, entry] {
            try
            {
                CheckRemovedWindow(entry);
            }
            catch (...)
            {
                NPX_LOG(L"CheckRemovedWindow: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            }
        });
    }

    void ModCore::CheckRemovedWindow(std::shared_ptr<WindowEntry> const& entry)
    {
        if (const auto tabsBar = entry->tabsBar.get())
        {
            if (tabsBar.XamlRoot() && winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(tabsBar))
            {
                return; // ancora nell'albero: era solo uno spostamento
            }
        }

        NPX_LOG(L"TabsBar %llx rimossa: finestra chiusa.", entry->tabsBarHandle);
        ReleaseFeatures(*entry, /*restore*/ false);

        std::scoped_lock lock(m_lock);
        std::erase(m_windows, entry);
    }

    void ModCore::ReleaseFeatures(WindowEntry& entry, bool restore)
    {
        if (entry.layoutTimer)
        {
            entry.layoutTimer.Stop();
            entry.layoutTimer = nullptr;
        }
        if (entry.backToTop)
        {
            entry.backToTop->Detach();
            entry.backToTop.reset();
        }
        if (entry.reopenTab)
        {
            entry.reopenTab->Detach();
            entry.reopenTab.reset();
        }
        if (entry.scrollBarHover)
        {
            entry.scrollBarHover->Detach(restore);
            entry.scrollBarHover.reset();
        }
        if (entry.wheelScroll)
        {
            entry.wheelScroll->Detach();
            entry.wheelScroll.reset();
        }
        if (entry.scrollRangeFix)
        {
            entry.scrollRangeFix->Detach();
            entry.scrollRangeFix.reset();
        }
        if (entry.verticalTabs)
        {
            entry.verticalTabs->Detach(restore);
            entry.verticalTabs.reset();
        }
    }

    void ModCore::AttachFeatures(std::shared_ptr<WindowEntry> const& entry)
    {
        const auto tabsBar = entry->tabsBar.get();
        if (!tabsBar)
        {
            return;
        }

        const Settings settings = CurrentSettings();
        if (settings.dumpTreeOnAttach)
        {
            TreeDump::DumpWindow(tabsBar, L"attach");
        }

        if (!entry->verticalTabs)
        {
            entry->verticalTabs = std::make_shared<VerticalTabs>(tabsBar);
            entry->verticalTabs->menuBarsProvider = [this] { return MenuBarsOnCurrentThread(); };
            entry->verticalTabs->scrollViewersProvider = [this] { return ScrollViewersOnCurrentThread(); };
            // Chiusura della finestra (WM_NCDESTROY, sul suo thread): si libera tutto subito.
            std::weak_ptr<WindowEntry> weakEntry = entry;
            entry->verticalTabs->onWindowClosed = [this, weakEntry] {
                if (const auto locked = weakEntry.lock())
                {
                    NPX_LOG(L"Finestra di Notepad chiusa (TabsBar %llx): risorse liberate.", locked->tabsBarHandle);
                    ReleaseFeatures(*locked, /*restore*/ false);
                    std::scoped_lock lock(m_lock);
                    std::erase(m_windows, locked);
                }
            };
        }
        entry->verticalTabs->Apply(settings);

        // "Torna su" è indipendente dalle schede verticali: un suo errore non deve fermare il resto.
        try
        {
            if (!entry->backToTop)
            {
                entry->backToTop = std::make_shared<BackToTop>(tabsBar);
                entry->backToTop->scrollViewersProvider = [this] { return ScrollViewersOnCurrentThread(); };
            }
            entry->backToTop->Apply(settings);
        }
        catch (...)
        {
            NPX_LOG(L"BackToTop: attivazione non riuscita (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }

        // Anche "riapri la scheda chiusa" è indipendente dalle altre feature.
        try
        {
            if (!entry->reopenTab)
            {
                entry->reopenTab = std::make_shared<ReopenTab>(tabsBar);
            }
            entry->reopenTab->Apply(settings);
        }
        catch (...)
        {
            NPX_LOG(L"ReopenTab: attivazione non riuscita (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }

        // Barre di scorrimento subito larghe sotto il mouse: indipendente dalle altre feature, sempre attiva.
        try
        {
            if (!entry->scrollBarHover)
            {
                entry->scrollBarHover = std::make_shared<ScrollBarHover>(tabsBar);
                entry->scrollBarHover->scrollBarsProvider = [this] { return ScrollBarsOnCurrentThread(); };
            }
            entry->scrollBarHover->Apply(settings);
        }
        catch (...)
        {
            NPX_LOG(L"ScrollBarHover: attivazione non riuscita (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }

        // Rotella senza scorrimento fluido: indipendente dalle altre feature.
        try
        {
            if (!entry->wheelScroll)
            {
                entry->wheelScroll = std::make_shared<WheelScroll>(tabsBar);
            }
            entry->wheelScroll->Apply(settings);
        }
        catch (...)
        {
            NPX_LOG(L"WheelScroll: attivazione non riuscita (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }

        // Correzione della corsa verticale fasulla dell'editor (difetto del Blocco note): sempre attiva.
        try
        {
            if (!entry->scrollRangeFix)
            {
                entry->scrollRangeFix = std::make_shared<ScrollRangeFix>(tabsBar);
            }
            entry->scrollRangeFix->Apply(settings);
        }
        catch (...)
        {
            NPX_LOG(L"ScrollRangeFix: attivazione non riuscita (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }

        // Controllo periodico di sicurezza del layout (gli eventi WinEvent/WM_SIZE coprono i casi
        // normali; questo recupera quelli persi). Economico e no-op quando le schede verticali sono spente.
        if (!entry->layoutTimer)
        {
            entry->layoutTimer = entry->dispatcher.CreateTimer();
            entry->layoutTimer.Interval(std::chrono::milliseconds(500));
            std::weak_ptr<WindowEntry> weakEntry = entry;
            entry->layoutTimer.Tick([weakEntry](auto&&, auto&&) {
                const auto locked = weakEntry.lock();
                if (!locked)
                {
                    return;
                }
                if (locked->verticalTabs)
                {
                    locked->verticalTabs->OnTimer();
                }
                if (locked->backToTop)
                {
                    try
                    {
                        locked->backToTop->OnTimer();
                    }
                    catch (...)
                    {
                        NPX_LOG(L"BackToTop: errore nel controllo periodico (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                    }
                }
                if (locked->reopenTab)
                {
                    try
                    {
                        locked->reopenTab->OnTimer();
                    }
                    catch (...)
                    {
                        NPX_LOG(L"ReopenTab: errore nel controllo periodico (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                    }
                }
                if (locked->scrollBarHover)
                {
                    try
                    {
                        locked->scrollBarHover->OnTimer();
                    }
                    catch (...)
                    {
                        NPX_LOG(L"ScrollBarHover: errore nel controllo periodico (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                    }
                }
                if (locked->wheelScroll)
                {
                    locked->wheelScroll->OnTimer();
                }
                if (locked->scrollRangeFix)
                {
                    locked->scrollRangeFix->OnTimer();
                }
            });
            entry->layoutTimer.Start();
        }
    }

    Settings ModCore::CurrentSettings() const
    {
        std::scoped_lock lock(m_lock);
        return m_settings;
    }

    bool ModCore::ReloadSettings()
    {
        const Settings fresh = Settings::Load();
        log::SetVerbose(fresh.verboseLog);

        // Anche l'allowlist di sviluppo cambia lo stato effettivo delle finestre.
        std::string devWindows;
        const HANDLE file = CreateFileW(DevWindowsPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            char buffer[1024];
            DWORD read = 0;
            devWindows = "present:";
            while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                devWindows.append(buffer, read);
            }
            CloseHandle(file);
        }

        std::scoped_lock lock(m_lock);
        const bool changed = !(fresh == m_settings) || devWindows != m_devWindows;
        m_settings = fresh;
        m_devWindows = std::move(devWindows);
        return changed;
    }

    std::vector<std::shared_ptr<ModCore::WindowEntry>> ModCore::SnapshotWindows() const
    {
        std::scoped_lock lock(m_lock);
        return m_windows;
    }

    void ModCore::ApplySettingsToAllWindows()
    {
        const Settings settings = CurrentSettings();
        NPX_LOG(L"Impostazioni cambiate: schede verticali %s, larghezza %d, %s; torna su %s; riapri scheda %s; scorrimento fluido %s",
                settings.verticalTabs ? L"ON" : L"OFF", settings.paneWidth, settings.collapsed ? L"compresso" : L"espanso",
                settings.backToTop ? L"ON" : L"OFF", settings.reopenTab ? L"ON" : L"OFF", settings.noSmoothScroll ? L"OFF" : L"ON");

        for (auto const& entry : SnapshotWindows())
        {
            const bool queued = entry->dispatcher.TryEnqueue([entry, settings] {
                try
                {
                    if (entry->verticalTabs)
                    {
                        entry->verticalTabs->Apply(settings);
                    }
                }
                catch (...)
                {
                    NPX_LOG(L"Apply: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                }
                try
                {
                    if (entry->backToTop)
                    {
                        entry->backToTop->Apply(settings);
                    }
                }
                catch (...)
                {
                    NPX_LOG(L"BackToTop Apply: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                }
                try
                {
                    if (entry->reopenTab)
                    {
                        entry->reopenTab->Apply(settings);
                    }
                }
                catch (...)
                {
                    NPX_LOG(L"ReopenTab Apply: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                }
                try
                {
                    if (entry->scrollBarHover)
                    {
                        entry->scrollBarHover->Apply(settings);
                    }
                }
                catch (...)
                {
                    NPX_LOG(L"ScrollBarHover Apply: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                }
                try
                {
                    if (entry->wheelScroll)
                    {
                        entry->wheelScroll->Apply(settings);
                    }
                }
                catch (...)
                {
                    NPX_LOG(L"WheelScroll Apply: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                }
                if (entry->scrollRangeFix)
                {
                    entry->scrollRangeFix->Apply(settings);
                }
            });
            if (!queued)
            {
                ForgetDeadWindow(entry);
            }
        }
    }

    void ModCore::ForgetDeadWindow(std::shared_ptr<WindowEntry> const& entry)
    {
        // Il thread della finestra non esiste più: gli oggetti XAML vanno solo abbandonati (rilasciarli
        // da un altro thread non è sicuro), quindi finiscono in un "cimitero" che non si svuota mai.
        static std::vector<std::shared_ptr<WindowEntry>>* graveyard = new std::vector<std::shared_ptr<WindowEntry>>();
        std::scoped_lock lock(m_lock);
        if (std::find(m_windows.begin(), m_windows.end(), entry) != m_windows.end())
        {
            graveyard->push_back(entry);
            std::erase(m_windows, entry);
            NPX_LOG(L"Finestra non più raggiungibile (TabsBar %llx): rimossa dall'elenco.", entry->tabsBarHandle);
        }
    }

    void ModCore::DumpAllWindows()
    {
        for (auto const& entry : SnapshotWindows())
        {
            const bool queued = entry->dispatcher.TryEnqueue([entry] {
                if (const auto tabsBar = entry->tabsBar.get())
                {
                    TreeDump::DumpWindow(tabsBar, L"request");
                }
                if (entry->verticalTabs)
                {
                    if (const auto panel = entry->verticalTabs->PanelRoot())
                    {
                        TreeDump::DumpWindow(panel, L"pannello schede verticali");
                    }
                }
            });
            if (!queued)
            {
                ForgetDeadWindow(entry);
            }
        }

        // Anche le isole della barra dei menu (l'albero si esporta dalla radice dell'isola)
        std::vector<MenuBarEntry> menuBars;
        {
            std::scoped_lock lock(m_lock);
            menuBars = m_menuBars;
        }
        for (auto const& menuBar : menuBars)
        {
            menuBar.dispatcher.TryEnqueue([weak = menuBar.element] {
                if (const auto element = weak.get())
                {
                    TreeDump::DumpWindow(element, L"barra dei menu");
                }
            });
        }
    }

    void ModCore::ReopenTabInActiveWindow()
    {
        for (auto const& entry : SnapshotWindows())
        {
            // Ogni finestra controlla da sé se è quella attiva (sul suo thread): agisce solo quella.
            const bool queued = entry->dispatcher.TryEnqueue([entry] {
                try
                {
                    if (entry->reopenTab)
                    {
                        entry->reopenTab->ReopenIfActive();
                    }
                }
                catch (...)
                {
                    NPX_LOG(L"ReopenTab: errore sul comando della tray (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                }
            });
            if (!queued)
            {
                ForgetDeadWindow(entry);
            }
        }
    }

    void ModCore::BackgroundLoop()
    {
        const HANDLE dumpEvent = CreateEventW(nullptr, FALSE, FALSE, DumpTreeEventName(GetCurrentProcessId()).c_str());
        const HANDLE reopenEvent = CreateEventW(nullptr, FALSE, FALSE, ReopenTabEventName(GetCurrentProcessId()).c_str());
        const HANDLE settingsChange = FindFirstChangeNotificationW(DataDirectory().c_str(), FALSE,
                                                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);

        HANDLE handles[3]{};
        DWORD count = 0;
        if (dumpEvent)
        {
            handles[count++] = dumpEvent;
        }
        if (reopenEvent)
        {
            handles[count++] = reopenEvent;
        }
        if (settingsChange != INVALID_HANDLE_VALUE)
        {
            handles[count++] = settingsChange;
        }
        if (count == 0)
        {
            return;
        }

        for (;;)
        {
            const DWORD result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
            if (result >= WAIT_OBJECT_0 + count)
            {
                NPX_LOG(L"WaitForMultipleObjects: %lu (errore %lu)", result, GetLastError());
                Sleep(1000);
                continue;
            }

            const HANDLE signaled = handles[result - WAIT_OBJECT_0];
            if (signaled == dumpEvent)
            {
                DumpAllWindows();
            }
            else if (signaled == reopenEvent)
            {
                ReopenTabInActiveWindow();
            }
            else
            {
                Sleep(150); // le scritture INI arrivano a raffiche
                FindNextChangeNotification(settingsChange);
                // La cartella cambia anche per motivi estranei (log, dump, copie della DLL):
                // si riapplica solo se impostazioni o allowlist sono cambiate davvero.
                if (ReloadSettings())
                {
                    ApplySettingsToAllWindows();
                }
            }
        }
    }
}
