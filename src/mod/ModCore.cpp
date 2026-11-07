#include "pch.h"
#include "ModCore.h"

#include "CaptionButtons.h"
#include "EditorWidth.h"
#include "Glass.h"
#include "Interop.h"
#include "Lifetime.h"
#include "Log.h"
#include "PositionPin.h"
#include "Shared.h"
#include "TreeDump.h"
#include "features/BackToTop.h"
#include "features/ReopenTab.h"
#include "features/ScrollBarHover.h"
#include "features/ScrollRangeFix.h"
#include "features/VerticalTabs.h"
#include "features/WheelScroll.h"

extern HMODULE g_module;

namespace npx
{
    namespace lifetime
    {
        namespace
        {
            std::atomic<bool> g_unloading{ false };
        }

        std::atomic<int>& Calls()
        {
            static std::atomic<int> calls{ 0 };
            return calls;
        }

        bool Unloading()
        {
            return g_unloading.load();
        }
    }

    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        namespace muxp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;
        using winrt::Microsoft::UI::Dispatching::DispatcherQueue;
        using winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority;

        constexpr wchar_t kIslandClass[] = L"Microsoft.UI.Content.DesktopChildSiteBridge";
        // La barra di scorrimento dell'editor (NotepadXamlUI.ScrollBar) contiene uno ScrollViewer di sistema con questo nome.
        constexpr std::wstring_view kScrollViewerName = L"ScrollViewer";

        // Il segnaposto della copia attiva della mod in questo processo (chiuso allo scaricamento).
        HANDLE g_activeMutex = nullptr;

        bool WaitUntil(std::function<bool()> const& condition, ULONGLONG timeoutMs)
        {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            while (!condition())
            {
                if (GetTickCount64() > deadline)
                {
                    return false;
                }
                Sleep(25);
            }
            return true;
        }

        // Esegue work sul thread di ogni dispatcher e aspetta che tutti abbiano finito (false allo scadere). Lo stato condiviso
        // sta in un shared_ptr: un lavoro arrivato dopo lo scadere non tocca memoria già liberata.
        template <typename Work>
        bool RunOnEachThread(std::vector<DispatcherQueue> const& dispatchers, ULONGLONG timeoutMs, Work work)
        {
            struct Pending
            {
                std::atomic<size_t> remaining{ 0 };
                HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                ~Pending()
                {
                    CloseHandle(done);
                }
            };
            if (dispatchers.empty())
            {
                return true;
            }
            const auto pending = std::make_shared<Pending>();
            pending->remaining = dispatchers.size();
            for (auto const& dispatcher : dispatchers)
            {
                bool queued = false;
                try
                {
                    queued = work(dispatcher, [pending] {
                        if (--pending->remaining == 0)
                        {
                            SetEvent(pending->done);
                        }
                    });
                }
                catch (...)
                {
                }
                if (!queued && --pending->remaining == 0)
                {
                    SetEvent(pending->done); // thread già finito: niente da aspettare
                }
            }
            return WaitForSingleObject(pending->done, static_cast<DWORD>(timeoutMs)) == WAIT_OBJECT_0;
        }

        // Tray chiusa con "Esci" (o dati della app eliminati): lo stesso criterio di ReloadSettings, letto dai file.
        bool TrayExitedNow()
        {
            return GetFileAttributesW(TrayExitedPath().c_str()) != INVALID_FILE_ATTRIBUTES ||
                   GetFileAttributesW(SettingsPath().c_str()) == INVALID_FILE_ATTRIBUTES;
        }

        // La DLL esce dal processo: ultimo atto del thread di controllo (TryUnload ha già tolto tutto).
        [[noreturn]] void UnloadNow(bool apartment)
        {
            if (g_activeMutex)
            {
                CloseHandle(g_activeMutex);
                g_activeMutex = nullptr;
            }
            NPX_LOG(L"Mod tolta dal Blocco note (processo %lu): DLL scaricata.", GetCurrentProcessId());
            if (apartment)
            {
                winrt::clear_factory_cache();
                winrt::uninit_apartment();
            }
            FreeLibraryAndExitThread(g_module, 0);
        }

        // Finestre principali del Blocco note di questo processo.
        std::vector<HWND> NotepadWindows(bool visibleOnly)
        {
            std::vector<HWND> windows;
            for (HWND window = FindWindowExW(nullptr, nullptr, L"Notepad", nullptr); window; window = FindWindowExW(nullptr, window, L"Notepad", nullptr))
            {
                DWORD process = 0;
                GetWindowThreadProcessId(window, &process);
                if (process == GetCurrentProcessId() && (!visibleOnly || IsWindowVisible(window)))
                {
                    windows.push_back(window);
                }
            }
            return windows;
        }

        // Isole XAML di una finestra (figlie dirette DesktopChildSiteBridge), tranne le nostre sovrapposte all'editor.
        std::vector<HWND> Islands(HWND main)
        {
            std::vector<HWND> islands;
            for (HWND child = GetWindow(main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                if (interop::ClassOf(child) == kIslandClass && !interop::IsOverlay(child))
                {
                    islands.push_back(child);
                }
            }
            return islands;
        }

        // Identità COM di un oggetto (il suo IUnknown): per riconoscerlo senza tenerlo.
        uint64_t IdentityOf(winrt::Windows::Foundation::IInspectable const& object)
        {
            return reinterpret_cast<uint64_t>(winrt::get_abi(object.as<winrt::Windows::Foundation::IUnknown>()));
        }

        // Primo antenato visuale (o l'elemento stesso) del tipo Target.
        template <typename Target>
        Target Ancestor(mux::DependencyObject node, int maxDepth)
        {
            for (int depth = 0; node && depth < maxDepth; ++depth)
            {
                if (auto target = node.try_as<Target>())
                {
                    return target;
                }
                node = muxm::VisualTreeHelper::GetParent(node);
            }
            return nullptr;
        }

        // Lo ScrollViewer "ScrollViewer" dentro una barra di scorrimento del Blocco note.
        muxc::ScrollViewer InnerScrollViewer(mux::DependencyObject const& bar)
        {
            std::vector<std::pair<mux::DependencyObject, int>> pending{ { bar, 0 } };
            while (!pending.empty())
            {
                auto [node, depth] = std::move(pending.back());
                pending.pop_back();
                if (const auto viewer = node.try_as<muxc::ScrollViewer>(); viewer && viewer.Name() == kScrollViewerName)
                {
                    return viewer;
                }
                if (depth >= 4)
                {
                    continue;
                }
                const int32_t count = muxm::VisualTreeHelper::GetChildrenCount(node);
                for (int32_t i = 0; i < count; ++i)
                {
                    pending.emplace_back(muxm::VisualTreeHelper::GetChild(node, i), depth + 1);
                }
            }
            return nullptr;
        }

        // Tutte le ScrollBar del sottoalbero visuale di start (senza entrare nelle barre).
        void CollectScrollBars(mux::DependencyObject const& start, std::vector<muxp::ScrollBar>& result)
        {
            std::vector<mux::DependencyObject> pending{ start };
            size_t visited = 0;
            while (!pending.empty() && visited < 20'000)
            {
                const mux::DependencyObject node = std::move(pending.back());
                pending.pop_back();
                ++visited;
                if (auto bar = node.try_as<muxp::ScrollBar>())
                {
                    result.push_back(std::move(bar));
                    continue;
                }
                const int32_t count = muxm::VisualTreeHelper::GetChildrenCount(node);
                for (int32_t i = 0; i < count; ++i)
                {
                    pending.push_back(muxm::VisualTreeHelper::GetChild(node, i));
                }
            }
        }

        // L'indirizzo sta nella nostra DLL?
        bool InOurModule(const void* address)
        {
            const auto* base = reinterpret_cast<const BYTE*>(g_module);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            const auto* at = static_cast<const BYTE*>(address);
            return at >= base && at < base + nt->OptionalHeader.SizeOfImage;
        }

        // Il nostro codice è sullo stack di questo thread SOTTO la chiamata attuale (il lavoro del dispatcher che sta girando)?
        // Succede quando il dispatcher ci chiama dentro un'attesa del nostro stesso codice: una feature che sposta una finestra
        // di un altro thread (SetWindowPos aspetta, e intanto il thread riceve i messaggi mandati e con essi i lavori del
        // dispatcher), una nostra procedura di finestra durante un ciclo modale (finestra trascinata)... Lì staccare o
        // cambiare le feature vorrebbe dire distruggere o modificare oggetti che quel codice sta ancora usando (crash visto
        // provando lo scaricamento: la colonna distrutta mentre RestoreChildren aspettava lo spostamento dell'editor).
        bool OurCodeBelowOnStack()
        {
            void* frames[400];
            const USHORT count = RtlCaptureStackBackTrace(0, ARRAYSIZE(frames), frames, nullptr);
            USHORT i = 0;
            while (i < count && InOurModule(frames[i]))
            {
                ++i; // la chiamata attuale: questa funzione, chi la usa, il lavoro e il suo delegato
            }
            for (; i < count; ++i)
            {
                if (InOurModule(frames[i]))
                {
                    return true;
                }
            }
            return false;
        }

        // Lavoro sul thread di un dispatcher, mai annidato nel nostro stesso codice (vedi OurCodeBelowOnStack): se il dispatcher
        // ci chiama lì, si riprova fra 100 ms, dal ciclo dei messaggi. Il timer del nuovo tentativo tiene vivo il lavoro (e
        // il lavoro il timer) finché non parte.
        struct ThreadWork : std::enable_shared_from_this<ThreadWork>
        {
            DispatcherQueue dispatcher{ nullptr };
            std::function<void()> work;
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{ nullptr }; // solo dal thread del dispatcher
            int deferrals = 0;

            static bool Post(DispatcherQueue const& dispatcher, DispatcherQueuePriority priority, std::function<void()> work)
            {
                if (!dispatcher)
                {
                    return false;
                }
                auto job = std::make_shared<ThreadWork>();
                job->dispatcher = dispatcher;
                job->work = std::move(work);
                return dispatcher.TryEnqueue(priority, [job] { job->Run(); });
            }

            void Run()
            {
                const auto keep = shared_from_this();
                if (timer)
                {
                    timer.Stop();
                    timer = nullptr; // (rompe il giro lavoro <-> timer)
                }
                if (!OurCodeBelowOnStack())
                {
                    const std::function<void()> run = std::move(work);
                    work = nullptr;
                    try
                    {
                        run();
                    }
                    catch (...)
                    {
                        NPX_LOG(L"Lavoro su una finestra: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                    }
                    return;
                }
                // Nel registro al più una riga ogni 5 s (durante un ciclo modale lungo, per esempio la finestra trascinata, i
                // lavori rimandati possono essere tanti).
                static std::atomic<ULONGLONG> lastLogged{ 0 };
                const ULONGLONG now = GetTickCount64();
                if (++deferrals == 1 && now - lastLogged.load() >= 5000)
                {
                    lastLogged = now;
                    NPX_LOG(L"Lavoro su una finestra arrivato dentro il nostro stesso codice (thread %lu): rimandato di poco.", GetCurrentThreadId());
                }
                try
                {
                    timer = dispatcher.CreateTimer();
                    timer.IsRepeating(false);
                    timer.Interval(std::chrono::milliseconds(100));
                    timer.Tick([keep](auto&&, auto&&) { keep->Run(); });
                    timer.Start();
                }
                catch (...)
                {
                    timer = nullptr;
                    NPX_LOG(L"Lavoro su una finestra perso: timer non creato (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                }
            }
        };
    }

    ModCore& ModCore::Instance()
    {
        // Mai distrutto: allo scaricamento della DLL i distruttori statici girano sotto il loader lock.
        static ModCore* instance = new ModCore();
        return *instance;
    }

    DWORD WINAPI ModCore::BootstrapThreadProc(void*)
    {
        log::Init();

        const std::wstring image = ProcessImagePath(GetCurrentProcessId());
        if (!IsStoreNotepadPath(image))
        {
            NPX_LOG(L"Processo non supportato (%s): la mod si scarica.", image.c_str());
            FreeLibraryAndExitThread(g_module, 0);
        }

        // Una sola copia attiva per processo: se il launcher, dopo una ricompilazione, caricasse una
        // seconda copia della DLL (da un'altra cartella), avremmo due pannelli in conflitto.
        const std::wstring mutexName = L"Local\\VTabsNotepadMod.Active." + std::to_wstring(GetCurrentProcessId());
        const HANDLE activeMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (activeMutex && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            NPX_LOG(L"Un'altra copia di VTabsNotepadMod è già attiva in questo processo: questa si scarica.");
            CloseHandle(activeMutex);
            FreeLibraryAndExitThread(g_module, 0);
        }
        // L'handle resta aperto finché la mod è nel processo (è il "segnaposto" della copia attiva).
        g_activeMutex = activeMutex;

        // Zona vetro della finestra (riga del titolo nascosta): finché nessuna finestra ha un limite, l'aggancio la
        // lascia com'è. Larghezza del testo per le tabelle Markdown: finché nessuna finestra ha la colonna, invariata.
        glass::Install();
        editorwidth::Install();

        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        ModCore& core = Instance();
        core.ReloadSettings();
        NPX_LOG(L"VTabsNotepadMod caricata in %s", image.c_str());

        // Il runtime WinUI arriva con la prima finestra: si aspetta senza limite (dopo il primo minuto un controllo al
        // secondo). Tray chiusa con "Esci" prima di qualunque finestra: c'è solo l'aggancio della zona vetro da togliere.
        const ULONGLONG waitStart = GetTickCount64();
        while (!GetModuleHandleW(L"Microsoft.UI.Xaml.dll") || !GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll"))
        {
            if (TrayExitedNow() && core.TryUnload())
            {
                UnloadNow(true);
            }
            Sleep(GetTickCount64() - waitStart < 60'000 ? 100 : 1000);
        }

        core.StartDiscovery();
        if (core.BackgroundLoop())
        {
            UnloadNow(true);
        }
        return 0;
    }

    void ModCore::StartDiscovery()
    {
        // Gli agganci (vedi Discovery.h) vanno installati sul thread di una finestra con XAML già pronto: si aspetta la
        // prima finestra con le sue isole.
        const ULONGLONG started = GetTickCount64();
        int failures = 0;
        while (!discovery::Installed())
        {
            if (lifetime::Unloading() || TrayExitedNow())
            {
                return; // tray chiusa con "Esci": ci pensa BackgroundLoop (TryUnload)
            }
            bool tried = false;
            for (HWND window : NotepadWindows(/*visibleOnly*/ false))
            {
                if (Islands(window).empty())
                {
                    continue;
                }
                tried = true;
                if (discovery::Install(window, &ModCore::OnElementFound))
                {
                    break;
                }
            }
            if (discovery::Installed())
            {
                break;
            }
            if (discovery::Blocked())
            {
                NPX_LOG(L"Ricerca delle finestre impossibile in questo processo: la mod resta inattiva.");
                return;
            }
            // Nessuna finestra pronta: si riguarda presto. Tentativo fallito: si riprova con calma (e dopo 5 volte di
            // rado), così un impedimento che non passa non diventa una raffica di tentativi e di righe nel log.
            if (tried && ++failures == 5)
            {
                NPX_LOG(L"Ricerca delle finestre: agganci non installati dopo 5 tentativi, si riprova ogni minuto.");
            }
            Sleep(!tried ? (GetTickCount64() - started < 60'000 ? 250 : 1000) : (failures < 5 ? 5'000 : 60'000));
        }
        NPX_LOG(L"Ricerca delle finestre pronta in %llu ms.", GetTickCount64() - started);
        // Le finestre già aperte non applicheranno più i loro modelli: le si cerca subito.
        ProbeUnmanaged(/*now*/ true);
    }

    void ModCore::OnElementFound(discovery::Kind kind, mux::FrameworkElement const& element) noexcept
    {
        // Siamo dentro il layout XAML o dentro una richiesta UI Automation: qui solo un riferimento debole, il resto al
        // prossimo giro del thread della finestra.
        if (lifetime::Unloading())
        {
            return;
        }
        try
        {
            // MeasureOverride arriva a ogni misura della barra: una barra già nota si riconosce subito.
            if (kind == discovery::Kind::ScrollBar && Instance().IsKnownScrollBar(element))
            {
                return;
            }
            // Una ricerca presenta la stessa barra delle schede da ognuna delle sue schede (e la barra dei menu da ogni voce):
            // un solo lavoro ogni 3 s per lo stesso elemento.
            if (kind != discovery::Kind::ScrollBar && !Instance().FirstNoticeOf(element))
            {
                return;
            }
            const auto dispatcher = element.DispatcherQueue();
            if (!dispatcher)
            {
                return;
            }
            ThreadWork::Post(dispatcher, DispatcherQueuePriority::Low, [kind, weak = winrt::make_weak(element)] {
                if (const auto found = weak.get())
                {
                    Instance().Register(kind, found);
                }
            });
        }
        catch (...)
        {
        }
    }

    bool ModCore::FirstNoticeOf(mux::FrameworkElement const& element)
    {
        const uint64_t id = IdentityOf(element);
        const ULONGLONG now = GetTickCount64();
        std::scoped_lock lock(m_lock);
        std::erase_if(m_recentNotices, [now](auto const& entry) { return now - entry.second >= 3000; });
        return m_recentNotices.try_emplace(id, now).second;
    }

    bool ModCore::IsKnownScrollBar(mux::FrameworkElement const& bar)
    {
        const uint64_t id = IdentityOf(bar);
        std::scoped_lock lock(m_lock);
        for (auto const& entry : m_scrollViewers)
        {
            // Stesso oggetto e ancora vivo (un indirizzo riusato da una barra nuova ha lo ScrollViewer di prima ormai morto).
            if (entry.id == id && entry.dispatcher.HasThreadAccess() && entry.element.get())
            {
                return true;
            }
        }
        return false;
    }

    void ModCore::Register(discovery::Kind kind, mux::FrameworkElement const& element)
    {
        if (lifetime::Unloading())
        {
            return; // la mod si sta togliendo
        }
        switch (kind)
        {
        case discovery::Kind::TabView:
            if (const auto tabsBar = Ancestor<winrt::NotepadXamlUI::TabsBar>(element, 8))
            {
                RegisterWindow(tabsBar);
            }
            else
            {
                NPX_VLOG(L"TabView ignorato (non è in una barra delle schede del Blocco note).");
            }
            break;
        case discovery::Kind::MenuBar:
            if (const auto mainMenuBar = Ancestor<winrt::NotepadXamlUI::MainMenuBar>(element, 8))
            {
                RegisterMenuBar(mainMenuBar);
            }
            break;
        case discovery::Kind::ScrollBar:
            RegisterScrollBar(element);
            break;
        }
    }

    void ModCore::RegisterWindow(winrt::NotepadXamlUI::TabsBar const& tabsBar)
    {
        HWND main = nullptr;
        if (const auto xamlRoot = tabsBar.XamlRoot())
        {
            if (const auto environment = xamlRoot.ContentIslandEnvironment())
            {
                main = GetAncestor(interop::HwndFromWindowId(environment.AppWindowId()), GA_ROOT);
            }
        }
        std::shared_ptr<WindowEntry> entry;
        std::vector<std::shared_ptr<WindowEntry>> replaced;
        size_t count = 0;
        {
            std::scoped_lock lock(m_lock);
            for (auto const& existing : m_windows)
            {
                if (existing->dispatcher.HasThreadAccess() && existing->tabsBar.get() == tabsBar)
                {
                    if (!existing->main)
                    {
                        existing->main = main;
                    }
                    return; // già nota (modello riapplicato o ricerca ripetuta)
                }
            }
            // Barra delle schede ricreata per una finestra già gestita (stesso thread): si stacca la vecchia, così la
            // finestra non ha mai due pannelli né due subclass.
            for (auto const& existing : m_windows)
            {
                if (main && existing->main == main && existing->dispatcher.HasThreadAccess())
                {
                    replaced.push_back(existing);
                }
            }
            entry = std::make_shared<WindowEntry>();
            entry->id = IdentityOf(tabsBar);
            entry->main = main;
            entry->tabsBar = winrt::make_weak(tabsBar);
            entry->dispatcher = tabsBar.DispatcherQueue();
            m_windows.push_back(entry);
            count = m_windows.size();
        }
        for (auto const& old : replaced)
        {
            NPX_LOG(L"Barra delle schede ricreata per la finestra %llx: si riparte dalla nuova.", reinterpret_cast<uintptr_t>(main));
            if (old->verticalTabs)
            {
                old->verticalTabs->onWindowClosed = nullptr;
            }
            ReleaseFeatures(*old, /*restore*/ true);
            std::scoped_lock lock(m_lock);
            std::erase(m_windows, old);
        }
        NPX_LOG(L"Nuova finestra di Notepad (TabsBar %llx), finestre gestite: %zu", entry->id, count);
        AttachFeatures(entry);
    }

    void ModCore::RegisterMenuBar(mux::FrameworkElement const& mainMenuBar)
    {
        const uint64_t id = IdentityOf(mainMenuBar);
        {
            std::scoped_lock lock(m_lock);
            for (auto it = m_menuBars.begin(); it != m_menuBars.end();)
            {
                if (it->dispatcher.HasThreadAccess())
                {
                    const auto existing = it->element.get();
                    if (!existing)
                    {
                        it = m_menuBars.erase(it); // distrutta (finestra chiusa): si fa pulizia
                        continue;
                    }
                    if (existing == mainMenuBar)
                    {
                        return; // già nota
                    }
                }
                ++it;
            }
            m_menuBars.push_back({ id, winrt::make_weak(mainMenuBar), mainMenuBar.DispatcherQueue() });
        }
        // La finestra la trova e la adegua da sé (controllo periodico): l'ordine di arrivo di barra dei menu e barra delle
        // schede non conta.
        NPX_LOG(L"Barra dei menu del Blocco note trovata (MainMenuBar %llx).", id);
    }

    void ModCore::RegisterScrollBar(mux::FrameworkElement const& bar)
    {
        const uint64_t id = IdentityOf(bar);
        const auto viewer = InnerScrollViewer(bar);
        if (!viewer)
        {
            NPX_VLOG(L"Barra di scorrimento del Blocco note %llx senza il suo ScrollViewer (ancora in costruzione?).", id);
            return;
        }
        size_t count = 0;
        {
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
                    if (existing == viewer)
                    {
                        it->id = id;
                        return; // già noto
                    }
                }
                ++it;
            }
            m_scrollViewers.push_back({ id, winrt::make_weak(viewer), viewer.DispatcherQueue() });
            count = m_scrollViewers.size();
        }
        NPX_LOG(L"Barra di scorrimento dell'editor trovata (%llx); barre note: %zu.", id, count);
    }

    void ModCore::ProbeUnmanaged(bool now)
    {
        if (!discovery::Installed() || lifetime::Unloading())
        {
            return;
        }
        std::vector<HWND> managed;
        {
            std::scoped_lock lock(m_lock);
            for (auto const& entry : m_windows)
            {
                managed.push_back(entry->main);
            }
        }
        const ULONGLONG tick = GetTickCount64();
        const std::vector<HWND> windows = NotepadWindows(/*visibleOnly*/ true);
        std::erase_if(m_probes, [&windows](auto const& known) { return std::find(windows.begin(), windows.end(), known.first) == windows.end(); });
        std::erase_if(m_islands, [](auto const& known) { return !IsWindow(known.first); });
        for (HWND window : windows)
        {
            const std::vector<HWND> islands = Islands(window);
            if (islands.empty())
            {
                continue; // XAML non ancora pronto
            }
            if (std::find(managed.begin(), managed.end(), window) != managed.end())
            {
                // Gestita: un'isola appena comparsa o tornata visibile (la barra di scorrimento verticale quando il testo non
                // sta più nella finestra, la barra di ricerca...) si guarda una volta, per le parti che contiene.
                for (HWND island : islands)
                {
                    const bool visible = IsWindowVisible(island) != FALSE;
                    const auto [it, inserted] = m_islands.try_emplace(island, false);
                    if (visible && !it->second)
                    {
                        discovery::Probe(island);
                    }
                    it->second = visible;
                }
                continue;
            }
            // Una finestra appena comparsa la presentano da sé gli agganci ai modelli: la ricerca serve a quelle già aperte
            // quando è arrivata la mod (e come rete di sicurezza), al più ogni 2 s e 10 volte per finestra.
            ProbeState& state = m_probes[window];
            if (!state.first)
            {
                state.first = tick;
            }
            if ((!now && tick - state.first < 1500) || (state.last && tick - state.last < 2000) || state.attempts >= 10)
            {
                continue;
            }
            state.last = tick;
            ++state.attempts;
            int probed = 0;
            for (HWND island : islands)
            {
                const bool visible = IsWindowVisible(island) != FALSE;
                m_islands[island] = visible;
                if (visible && discovery::Probe(island))
                {
                    ++probed;
                }
            }
            NPX_LOG(L"Ricerca delle parti della finestra %llx (tentativo %d): %d isole su %zu.", reinterpret_cast<uintptr_t>(window), state.attempts, probed,
                    islands.size());
        }
    }

    std::vector<muxp::ScrollBar> ModCore::ScrollBarsOnCurrentThread(bool full)
    {
        // Elementi noti di questo thread: da ognuno l'isola che lo contiene (XamlRoot).
        std::vector<mux::UIElement> anchors;
        std::vector<mux::UIElement> editors;
        for (auto const& entry : SnapshotWindows())
        {
            if (!entry->dispatcher || !entry->dispatcher.HasThreadAccess())
            {
                continue;
            }
            if (const auto tabsBar = entry->tabsBar.get())
            {
                anchors.push_back(tabsBar);
            }
            if (entry->verticalTabs)
            {
                if (const auto panel = entry->verticalTabs->PanelRoot())
                {
                    anchors.push_back(panel);
                }
            }
        }
        for (auto const& menuBar : MenuBarsOnCurrentThread())
        {
            anchors.push_back(menuBar);
        }
        for (auto const& viewer : ScrollViewersOnCurrentThread())
        {
            anchors.push_back(viewer);
            editors.push_back(viewer);
        }

        std::vector<mux::XamlRoot> roots;
        for (auto const& anchor : anchors)
        {
            try
            {
                const auto root = anchor.XamlRoot();
                if (root && std::find(roots.begin(), roots.end(), root) == roots.end())
                {
                    roots.push_back(root);
                }
            }
            catch (...)
            {
            }
        }

        std::vector<muxp::ScrollBar> result;
        try
        {
            if (full)
            {
                for (auto const& root : roots)
                {
                    if (const auto content = root.Content())
                    {
                        CollectScrollBars(content, result);
                    }
                }
            }
            else
            {
                for (auto const& editor : editors)
                {
                    CollectScrollBars(editor, result);
                }
            }
            for (auto const& root : roots)
            {
                for (auto const& popup : muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(root))
                {
                    if (const auto child = popup.Child())
                    {
                        CollectScrollBars(child, result);
                    }
                }
            }
        }
        catch (...)
        {
            // Albero che cambia sotto i piedi (finestra in chiusura): si riprova al prossimo controllo.
        }
        return result;
    }

    std::vector<muxc::ScrollViewer> ModCore::ScrollViewersOnCurrentThread()
    {
        std::vector<muxc::ScrollViewer> result;
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

    std::vector<mux::FrameworkElement> ModCore::MenuBarsOnCurrentThread()
    {
        std::vector<mux::FrameworkElement> result;
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
                    NPX_LOG(L"Finestra di Notepad chiusa (TabsBar %llx): risorse liberate.", locked->id);
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
                // Il colore della scheda riaperta passa dalla colonna (aggiorna subito la riga); senza colonna solo nel file.
                std::weak_ptr<WindowEntry> weakEntry = entry;
                entry->reopenTab->restoreColor = [weakEntry](winrt::guid const& tabId, tabcolors::TabColor color) {
                    const auto locked = weakEntry.lock();
                    if (locked && locked->verticalTabs)
                    {
                        locked->verticalTabs->RestoreTabColor(tabId, color);
                    }
                    else
                    {
                        tabcolors::Set(tabId, color);
                    }
                };
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
                entry->scrollBarHover->scrollBarsProvider = [this](bool full) { return ScrollBarsOnCurrentThread(full); };
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
                if (!locked || locked->closing)
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
        Settings fresh = Settings::Load();
        log::SetVerbose(fresh.verboseLog);
        // Tray chiusa con "Esci": tutte le funzioni spente (anche quelle sempre attive, che guardano trayExited) finché non
        // riparte. Solo qui, in memoria: settings.ini resta com'è. Senza settings.ini (dati eliminati disinstallando l'app:
        // il launcher lo crea sempre prima di iniettare) vale lo stesso, invece dei valori predefiniti (accesi).
        const bool exitedByTray = GetFileAttributesW(TrayExitedPath().c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool noSettings = GetFileAttributesW(SettingsPath().c_str()) == INVALID_FILE_ATTRIBUTES;
        if (exitedByTray || noSettings)
        {
            fresh.trayExited = true;
            fresh.verticalTabs = false;
            fresh.backToTop = false;
            fresh.reopenTab = false;
            fresh.noSmoothScroll = false;
        }

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
        if (fresh.trayExited != m_settings.trayExited)
        {
            NPX_LOG(!fresh.trayExited ? L"Tray di nuovo avviata: la mod si riaccende con le impostazioni salvate."
                    : exitedByTray    ? L"Tray chiusa con \"Esci\": la mod si spegne in tutte le finestre (come con le funzioni spente)."
                                      : L"Impostazioni non trovate (app disinstallata?): la mod si spegne in tutte le finestre.");
        }
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
        NPX_LOG(L"Impostazioni cambiate: schede verticali %s, larghezza %d, %s; torna su %s; riapri scheda %s; scorrimento fluido %s%s",
                settings.verticalTabs ? L"ON" : L"OFF", settings.paneWidth, settings.collapsed ? L"compresso" : L"espanso",
                settings.backToTop ? L"ON" : L"OFF", settings.reopenTab ? L"ON" : L"OFF", settings.noSmoothScroll ? L"OFF" : L"ON",
                settings.trayExited ? L"; tray chiusa: tutto spento" : L"");

        for (auto const& entry : SnapshotWindows())
        {
            const bool queued = ThreadWork::Post(entry->dispatcher, DispatcherQueuePriority::Normal, [entry, settings] {
                if (entry->closing)
                {
                    return; // la finestra si sta staccando (scaricamento)
                }
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
            NPX_LOG(L"Finestra non più raggiungibile (TabsBar %llx): rimossa dall'elenco.", entry->id);
        }
    }

    void ModCore::DumpAllWindows()
    {
        for (auto const& entry : SnapshotWindows())
        {
            const bool queued = ThreadWork::Post(entry->dispatcher, DispatcherQueuePriority::Normal, [entry] {
                if (entry->closing)
                {
                    return;
                }
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
            const bool queued = ThreadWork::Post(entry->dispatcher, DispatcherQueuePriority::Normal, [entry] {
                if (entry->closing)
                {
                    return;
                }
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

    void ModCore::TeardownThread(int epoch)
    {
        if (!lifetime::Unloading() || epoch != m_unloadEpoch.load())
        {
            return; // la tray è ripartita prima: niente da staccare
        }
        const auto entries = SnapshotWindows();
        for (auto const& entry : entries)
        {
            if (entry->dispatcher && entry->dispatcher.HasThreadAccess())
            {
                entry->closing = true; // un lavoro nostro già in coda non tocca più questa finestra
            }
        }
        for (auto const& entry : entries)
        {
            if (!entry->dispatcher || !entry->dispatcher.HasThreadAccess())
            {
                continue;
            }
            if (entry->verticalTabs)
            {
                entry->verticalTabs->onWindowClosed = nullptr;
            }
            try
            {
                ReleaseFeatures(*entry, /*restore*/ true); // colonna, barra dei menu, editor, pulsanti, agganci della finestra
            }
            catch (...)
            {
                NPX_LOG(L"Scaricamento: errore staccando la finestra %llx (0x%08X).", entry->id, static_cast<unsigned>(winrt::to_hresult()));
            }
            std::scoped_lock lock(m_lock);
            std::erase(m_windows, entry);
        }
        // Riferimenti deboli agli elementi di questo thread: rilasciati qui (sul thread che li possiede).
        std::scoped_lock lock(m_lock);
        std::erase_if(m_menuBars, [](MenuBarEntry const& entry) { return entry.dispatcher.HasThreadAccess(); });
        std::erase_if(m_scrollViewers, [](ScrollViewerEntry const& entry) { return entry.dispatcher.HasThreadAccess(); });
    }

    bool ModCore::TryUnload()
    {
        if (!lifetime::g_unloading.exchange(true))
        {
            NPX_LOG(L"Tray chiusa con \"Esci\": la mod si toglie dal Blocco note.");
        }
        if (!m_tornDown)
        {
            // 1. Ogni thread delle finestre: tutto come prima della mod, finestre e parti dimenticate.
            std::vector<DispatcherQueue> dispatchers;
            {
                std::scoped_lock lock(m_lock);
                const auto add = [&dispatchers](DispatcherQueue const& dispatcher) {
                    if (dispatcher && std::find(dispatchers.begin(), dispatchers.end(), dispatcher) == dispatchers.end())
                    {
                        dispatchers.push_back(dispatcher);
                    }
                };
                for (auto const& entry : m_windows)
                {
                    add(entry->dispatcher);
                }
                for (auto const& entry : m_menuBars)
                {
                    add(entry.dispatcher);
                }
                for (auto const& entry : m_scrollViewers)
                {
                    add(entry.dispatcher);
                }
            }
            for (auto const& dispatcher : dispatchers)
            {
                if (std::find(m_seenDispatchers.begin(), m_seenDispatchers.end(), dispatcher) == m_seenDispatchers.end())
                {
                    m_seenDispatchers.push_back(dispatcher);
                }
            }
            const size_t windows = SnapshotWindows().size();
            // Un lavoro di distacco per thread (ThreadWork: mai annidato nel nostro codice). Resta in corso anche se qui si
            // smette di aspettare: al prossimo tentativo si aspetta lo stesso (uno nuovo solo se quello è perso).
            const int epoch = m_unloadEpoch.load();
            const ULONGLONG now = GetTickCount64();
            for (auto const& dispatcher : dispatchers)
            {
                auto it = std::find_if(m_teardowns.begin(), m_teardowns.end(), [&dispatcher](auto const& entry) { return entry.first == dispatcher; });
                if (it != m_teardowns.end() && it->second->epoch == epoch && (it->second->finished.load() || now - it->second->created < 20'000))
                {
                    continue; // già in corso (o finito) per questo scaricamento
                }
                auto state = std::make_shared<TeardownState>();
                state->epoch = epoch;
                state->created = now;
                const bool queued = ThreadWork::Post(dispatcher, DispatcherQueuePriority::Normal, [this, state] {
                    TeardownThread(state->epoch);
                    state->finished = true;
                });
                if (!queued)
                {
                    state->finished = true; // thread già finito: niente da staccare lì
                }
                if (it != m_teardowns.end())
                {
                    it->second = state;
                }
                else
                {
                    m_teardowns.emplace_back(dispatcher, state);
                }
            }
            const bool detached = WaitUntil(
                [this, epoch] {
                    return std::all_of(m_teardowns.begin(), m_teardowns.end(),
                                       [epoch](auto const& entry) { return entry.second->epoch != epoch || entry.second->finished.load(); });
                },
                8000);
            if (!detached)
            {
                NPX_LOG(L"Scaricamento rimandato: non tutte le finestre si sono staccate in tempo.");
                return false;
            }
            {
                std::scoped_lock lock(m_lock);
                if (!m_windows.empty() || !m_menuBars.empty() || !m_scrollViewers.empty())
                {
                    // Il thread di una finestra non esiste più: i suoi oggetti XAML si possono solo abbandonare (e i nostri
                    // delegati che tengono fanno restare la DLL: vedi il contatore più sotto).
                    NPX_LOG(L"Scaricamento: %zu finestre senza thread, abbandonate.", m_windows.size());
                    static auto* graveyard = new std::vector<std::shared_ptr<WindowEntry>>();
                    graveyard->insert(graveyard->end(), m_windows.begin(), m_windows.end());
                    m_windows.clear();
                    static auto* lostMenuBars = new std::vector<MenuBarEntry>();
                    lostMenuBars->insert(lostMenuBars->end(), m_menuBars.begin(), m_menuBars.end());
                    m_menuBars.clear();
                    static auto* lostScrollViewers = new std::vector<ScrollViewerEntry>();
                    lostScrollViewers->insert(lostScrollViewers->end(), m_scrollViewers.begin(), m_scrollViewers.end());
                    m_scrollViewers.clear();
                }
            }
            // 2. Agganci: di nuovo le funzioni originali del Blocco note e di WinUI, e niente subclass negli editor.
            if (!discovery::Uninstall() || !glass::Uninstall() || !editorwidth::Uninstall() || !pin::Uninstall())
            {
                NPX_LOG(L"Scaricamento rimandato: un aggancio non si può (ancora) togliere (la mod resta, spenta).");
                return false;
            }
            m_tornDown = true;
            NPX_LOG(L"Scaricamento: %zu finestre staccate, agganci tolti.", windows);
        }
        // 3. Nessuno dentro il nostro codice: il contatore, poi un giro del ciclo di ogni thread delle finestre (chi era
        //    entrato prima del ripristino ne è uscito quando il suo thread torna al ciclo dei messaggi), poi un attimo di
        //    margine e di nuovo il contatore.
        if (!WaitUntil([] { return lifetime::Calls().load() == 0; }, 3000))
        {
            NPX_LOG(L"Scaricamento rimandato: %d chiamate ancora dentro il nostro codice (per esempio la finestra si sta spostando).",
                    lifetime::Calls().load());
            return false;
        }
        if (!RunOnEachThread(m_seenDispatchers, 5000, [](DispatcherQueue const& dispatcher, auto done) {
                return dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [done] { done(); });
            }))
        {
            NPX_LOG(L"Scaricamento rimandato: un thread delle finestre non risponde.");
            return false;
        }
        Sleep(300);
        if (lifetime::Calls().load() != 0)
        {
            return false;
        }
        // 4. Oggetti nostri ancora vivi (delegati degli eventi XAML, lavori in coda nei dispatcher): solo a zero la DLL può
        //    uscire, altrimenti il Blocco note chiamerebbe codice che non c'è più.
        if (!WaitUntil([] { return static_cast<uint32_t>(winrt::get_module_lock()) == 0; }, 5000))
        {
            NPX_LOG(L"Scaricamento rimandato: %u oggetti della mod ancora vivi nel Blocco note.", static_cast<uint32_t>(winrt::get_module_lock()));
            return false;
        }
        // 5. Classe di finestra dei pulsanti (fallisce se ne esiste ancora una finestra: allora la DLL resta).
        if (!CaptionButtons::UnregisterWindowClass())
        {
            NPX_LOG(L"Scaricamento rimandato: una finestra della mod esiste ancora (%lu).", GetLastError());
            return false;
        }
        m_seenDispatchers.clear();
        m_teardowns.clear();
        return true;
    }

    void ModCore::Resume()
    {
        NPX_LOG(L"Tray di nuovo avviata prima dello scaricamento: la mod riparte.");
        m_tornDown = false;
        m_seenDispatchers.clear();
        m_probes.clear();
        m_islands.clear();
        ++m_unloadEpoch; // un lavoro di distacco ancora in coda non stacca più niente
        lifetime::g_unloading = false;
        pin::Resume();
        glass::Install();
        editorwidth::Install();
        StartDiscovery();
    }

    bool ModCore::BackgroundLoop()
    {
        const HANDLE dumpEvent = CreateEventW(nullptr, FALSE, FALSE, DumpTreeEventName(GetCurrentProcessId()).c_str());
        const HANDLE reopenEvent = CreateEventW(nullptr, FALSE, FALSE, ReopenTabEventName(GetCurrentProcessId()).c_str());
        const HANDLE settingsChange = FindFirstChangeNotificationW(DataDirectory().c_str(), FALSE,
                                                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
        const auto closeHandles = [&] {
            if (dumpEvent)
            {
                CloseHandle(dumpEvent);
            }
            if (reopenEvent)
            {
                CloseHandle(reopenEvent);
            }
            if (settingsChange != INVALID_HANDLE_VALUE)
            {
                FindCloseChangeNotification(settingsChange);
            }
        };

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

        // Ci si sveglia per un comando o un cambiamento nella cartella dei dati; ogni 2 s comunque, per le finestre e le
        // isole nuove da cercare e per la tray chiusa (anche se le notifiche della cartella mancassero).
        bool reload = true; // subito: la tray può essere già stata chiusa mentre si cercavano le finestre
        for (;;)
        {
            if (!reload)
            {
                const DWORD result = count ? WaitForMultipleObjects(count, handles, FALSE, 2000) : (Sleep(2000), static_cast<DWORD>(WAIT_TIMEOUT));
                if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
                {
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
                        reload = true;
                    }
                }
                else if (result != WAIT_TIMEOUT)
                {
                    NPX_LOG(L"WaitForMultipleObjects: %lu (errore %lu)", result, GetLastError());
                    Sleep(1000);
                }
                else if (TrayExitedNow() != CurrentSettings().trayExited)
                {
                    reload = true; // notifica della cartella persa
                }
            }
            if (reload)
            {
                reload = false;
                // La cartella cambia anche per motivi estranei (log, dump, copie della DLL): si riapplica solo se
                // impostazioni o allowlist sono cambiate davvero.
                if (ReloadSettings())
                {
                    ApplySettingsToAllWindows();
                }
            }
            if (CurrentSettings().trayExited)
            {
                // Le finestre sono già tornate normali (impostazioni spente); ora via tutto e la DLL esce. Se non si può
                // ancora (la finestra si sta spostando, un oggetto ancora vivo), si riprova al prossimo giro (al più 2 s).
                if (TryUnload())
                {
                    closeHandles();
                    return true;
                }
                continue;
            }
            if (m_tornDown || lifetime::Unloading())
            {
                Resume(); // la tray è ripartita prima che la DLL uscisse
            }
            // Finestre aperte prima della mod (rete di sicurezza: anche quelle che nessun aggancio ha presentato) e isole
            // nuove: ricerca UI Automation.
            ProbeUnmanaged(/*now*/ false);
        }
    }
}
