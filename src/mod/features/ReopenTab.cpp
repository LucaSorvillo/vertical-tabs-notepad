#include "pch.h"
#include "ReopenTab.h"

#include "Interop.h"
#include "Log.h"
#include "Shared.h"

#include <shellapi.h>

#include <thread>

namespace npx
{
    namespace
    {
        constexpr int kHotkeyId = 0x4E52;            // id della scorciatoia (per finestra)
        constexpr UINT_PTR kSubclassId = 0x4E505852; // "NPXR"
        constexpr ULONGLONG kConfirmMs = 500;        // una scheda sparita è chiusa se non torna (riordino)
        constexpr ULONGLONG kReopenTimeoutMs = 8000; // oltre, la riapertura chiesta si dimentica
        constexpr size_t kMaxClosed = 25;

        // Sul thread UI non si tocca il disco: una cartella di rete non raggiungibile bloccherebbe la
        // finestra per secondi. Basta vedere se la descrizione della scheda è un percorso assoluto
        // ("C:\...", "\\server\..."): per una scheda mai salvata è "Senza titolo".
        bool LooksLikeFilePath(std::wstring const& path)
        {
            return (path.size() > 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) || path.starts_with(L"\\\\");
        }

        // Questa invece guarda il disco: solo sul thread che apre il file (vedi OpenInNotepad).
        bool IsExistingFile(std::wstring const& path)
        {
            if (path.empty())
            {
                return false;
            }
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        // Il Blocco note da lanciare: l'alias del pacchetto, che attiva l'istanza già in esecuzione
        // (quella in cui viviamo) e le fa aprire il file come scheda della finestra attiva.
        std::wstring NotepadCommand()
        {
            wchar_t local[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local)))
            {
                const std::wstring alias = std::wstring{ local } + L"\\Microsoft\\WindowsApps\\notepad.exe";
                if (GetFileAttributesW(alias.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    return alias;
                }
            }
            return L"notepad.exe";
        }

        // --- Cronologia delle schede chiuse su disco (closed-tabs.txt nella cartella dati) ---------------------------------
        // Una riga per scheda, "posto<TAB>percorso" in UTF-8, la più recente in fondo; le righe con # sono commenti. Il
        // percorso non può contenere tabulazioni né a capo (caratteri non validi nei nomi di Windows).
        struct StoredTab
        {
            std::wstring path;
            size_t index = 0;
        };

        std::wstring StorePath()
        {
            return DataDirectory() + L"\\closed-tabs.txt";
        }

        // Letture e scritture una alla volta, anche tra finestre (thread diversi) e istanze del Blocco note.
        class StoreLock
        {
        public:
            StoreLock() : m_mutex(CreateMutexW(nullptr, FALSE, L"Local\\VTabsNotepad.ClosedTabs"))
            {
                if (m_mutex)
                {
                    const DWORD wait = WaitForSingleObject(m_mutex, 2000);
                    m_owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
                }
            }
            ~StoreLock()
            {
                if (m_mutex)
                {
                    if (m_owned)
                    {
                        ReleaseMutex(m_mutex);
                    }
                    CloseHandle(m_mutex);
                }
            }
            StoreLock(StoreLock const&) = delete;
            StoreLock& operator=(StoreLock const&) = delete;

        private:
            HANDLE m_mutex = nullptr;
            bool m_owned = false;
        };

        std::vector<StoredTab> LoadStored()
        {
            std::vector<StoredTab> entries;
            const HANDLE file = CreateFileW(StorePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return entries;
            }
            LARGE_INTEGER size{};
            std::string bytes;
            if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20))
            {
                bytes.resize(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
                {
                    read = 0;
                }
                bytes.resize(read);
            }
            CloseHandle(file);
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
                static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.erase(0, 3); // BOM
            }
            const int chars = bytes.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            std::wstring text(static_cast<size_t>(std::max(chars, 0)), L'\0');
            if (chars > 0)
            {
                MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), chars);
            }
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find(L'\n', start);
                if (end == std::wstring::npos)
                {
                    end = text.size();
                }
                std::wstring line = text.substr(start, end - start);
                start = end + 1;
                if (!line.empty() && line.back() == L'\r')
                {
                    line.pop_back();
                }
                const size_t tab = line.find(L'\t');
                if (line.empty() || line[0] == L'#' || tab == std::wstring::npos || tab == 0)
                {
                    continue;
                }
                StoredTab entry;
                entry.index = static_cast<size_t>(wcstoul(line.substr(0, tab).c_str(), nullptr, 10));
                entry.path = line.substr(tab + 1);
                if (LooksLikeFilePath(entry.path))
                {
                    entries.push_back(std::move(entry));
                }
            }
            if (entries.size() > kMaxClosed)
            {
                entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(kMaxClosed));
            }
            return entries;
        }

        // Riscrive tutto (file temporaneo e poi sostituzione: mai un file a metà). Lista vuota = file cancellato.
        void SaveStored(std::vector<StoredTab> const& entries)
        {
            const std::wstring path = StorePath();
            if (entries.empty())
            {
                DeleteFileW(path.c_str());
                return;
            }
            std::wstring text = L"# VTabsNotepad: schede chiuse per Ctrl+Maiusc+T (posto e percorso, la piu' recente in fondo).\r\n";
            for (auto const& entry : entries)
            {
                text += std::to_wstring(entry.index) + L"\t" + entry.path + L"\r\n";
            }
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (bytes <= 0)
            {
                return;
            }
            std::string utf8(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
            const std::wstring temporary = path + L".tmp";
            const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                NPX_LOG(L"ReopenTab: cronologia delle schede chiuse non salvata (errore %lu).", GetLastError());
                return;
            }
            DWORD written = 0;
            const bool ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size();
            CloseHandle(file);
            if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                NPX_LOG(L"ReopenTab: cronologia delle schede chiuse non salvata (errore %lu).", GetLastError());
                DeleteFileW(temporary.c_str());
            }
        }

        void OpenInNotepad(std::wstring const& path)
        {
            // Su un thread a sé: controllare il file e chiamare la shell può costare (molto, su una
            // cartella di rete) e il thread UI della finestra non deve fermarsi. Niente riferimenti
            // all'oggetto: solo le stringhe.
            std::thread([path, command = NotepadCommand(), arguments = L"\"" + path + L"\""] {
                if (!IsExistingFile(path))
                {
                    NPX_LOG(L"ReopenTab: il file della scheda chiusa non c'è più: non si riapre.");
                    return;
                }
                const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                SHELLEXECUTEINFOW info{ sizeof(info) };
                info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
                info.lpVerb = L"open";
                info.lpFile = command.c_str();
                info.lpParameters = arguments.c_str();
                info.nShow = SW_SHOWNORMAL;
                if (!ShellExecuteExW(&info))
                {
                    NPX_LOG(L"ReopenTab: non è stato possibile riaprire il file (errore %lu).", GetLastError());
                }
                if (SUCCEEDED(init))
                {
                    CoUninitialize();
                }
            }).detach();
        }
    }

    ReopenTab::ReopenTab(winrt::NotepadXamlUI::TabsBar const& tabsBar) : m_tabsBar(winrt::make_weak(tabsBar))
    {
    }

    ReopenTab::~ReopenTab()
    {
        Detach();
    }

    bool ReopenTab::Attach()
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return false;
        }
        HWND main = nullptr;
        try
        {
            if (const auto xamlRoot = tabsBar.XamlRoot())
            {
                if (const auto environment = xamlRoot.ContentIslandEnvironment())
                {
                    main = GetAncestor(interop::HwndFromWindowId(environment.AppWindowId()), GA_ROOT);
                }
            }
        }
        catch (...)
        {
        }
        if (!main || !IsWindow(main))
        {
            return false;
        }
        m_main = main;
        m_attached = true;
        return true;
    }

    void ReopenTab::Apply(Settings const& settings)
    {
        if (!m_attached && !Attach())
        {
            return;
        }
        // Gate per-finestra (sviluppo): se esiste dev-windows.txt agiamo solo sulle finestre elencate.
        const bool wanted = settings.reopenTab && WindowApplyAllowed(m_main);
        if (wanted && !m_enabled)
        {
            Enable();
        }
        else if (!wanted && m_enabled)
        {
            // Spenta dalla tray: si dimentica anche la cronologia su disco. Esclusa solo questa finestra (modalità
            // sviluppo): la cronologia resta.
            Disable(!settings.reopenTab);
        }
    }

    void ReopenTab::Enable()
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return;
        }
        m_enabled = true;
        if (!m_subclassed && GetWindowThreadProcessId(m_main, nullptr) == GetCurrentThreadId())
        {
            m_subclassed = SetWindowSubclass(m_main, &SubclassProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this)) != FALSE;
        }
        if (!m_subclassed)
        {
            // Senza il subclass non arriverebbe WM_HOTKEY: la combinazione non si chiede nemmeno,
            // altrimenti la si toglierebbe agli altri programmi senza poi farne niente.
            NPX_LOG(L"ReopenTab: non è stato possibile agganciare la finestra (errore %lu): scorciatoia non attiva.", GetLastError());
        }
        if (!m_items)
        {
            m_items = tabsBar.TabItems();
            const std::weak_ptr<ReopenTab> weak = weak_from_this();
            m_itemsToken = m_items.VectorChanged([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    try
                    {
                        self->Refresh();
                    }
                    catch (...)
                    {
                        NPX_LOG(L"ReopenTab: errore rileggendo le schede (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                    }
                }
            });
        }
        m_known.clear();
        Refresh(); // le schede aperte adesso non sono chiusure
        m_gone.clear();
        SyncHotkey();
        NPX_LOG(L"ReopenTab: \"riapri la scheda chiusa\" attiva (%zu schede).", m_known.size());
    }

    void ReopenTab::Disable(bool forgetHistory)
    {
        m_enabled = false;
        if (forgetHistory)
        {
            StoreLock lock;
            SaveStored({}); // cancella closed-tabs.txt
        }
        Unregister();
        if (m_items && m_itemsToken)
        {
            try
            {
                m_items.VectorChanged(m_itemsToken);
            }
            catch (...)
            {
            }
        }
        m_items = nullptr;
        m_itemsToken = {};
        if (m_subclassed && IsWindow(m_main))
        {
            RemoveWindowSubclass(m_main, &SubclassProc, kSubclassId);
        }
        m_subclassed = false;
        ClearState();
        NPX_LOG(L"ReopenTab: \"riapri la scheda chiusa\" disattivata.");
    }

    void ReopenTab::ClearState()
    {
        // Lo stato della finestra in memoria (la cronologia su disco la gestisce Disable).
        m_known.clear();
        m_gone.clear();
        m_reopening.clear();
    }

    void ReopenTab::Detach()
    {
        if (m_enabled)
        {
            Disable(false); // finestra chiusa: le sue schede chiuse restano riapribili (altre finestre, dopo un riavvio)
        }
        else
        {
            Unregister();
            if (m_subclassed && IsWindow(m_main))
            {
                RemoveWindowSubclass(m_main, &SubclassProc, kSubclassId);
            }
            m_subclassed = false;
            ClearState();
        }
        m_attached = false;
    }

    void ReopenTab::Refresh()
    {
        const auto tabsBar = m_tabsBar.get();
        if (!m_enabled || !tabsBar)
        {
            return;
        }
        const auto items = m_items ? m_items : tabsBar.TabItems();
        const ULONGLONG now = GetTickCount64();

        std::vector<Known> fresh;
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            const auto item = items.GetAt(i);
            Known known;
            known.tabId = item.TabId();
            known.path = std::wstring{ item.Tooltip() }; // per un file salvato è il suo percorso
            fresh.push_back(std::move(known));
        }

        // Schede sparite: possono essere chiuse oppure spostate (un riordino le toglie e le rimette).
        for (size_t i = 0; i < m_known.size(); ++i)
        {
            Known const& old = m_known[i];
            const auto sameId = [&](auto const& entry) { return entry.tabId == old.tabId; };
            if (std::none_of(fresh.begin(), fresh.end(), sameId) && std::none_of(m_gone.begin(), m_gone.end(), sameId))
            {
                m_gone.push_back(Gone{ old.tabId, old.path, i, now });
            }
        }
        // Tornate: era un riordino, non una chiusura.
        std::erase_if(m_gone, [&](Gone const& gone) {
            return std::any_of(fresh.begin(), fresh.end(), [&](Known const& entry) { return entry.tabId == gone.tabId; });
        });
        m_known = std::move(fresh);

        // Riaperture chieste: appena la scheda compare, torna al posto che aveva. Una per giro: lo
        // spostamento cambia gli indici e rientra qui dentro (VectorChanged), e quella chiamata
        // annidata sistema l'eventuale seconda scheda.
        for (size_t i = 0; i < m_known.size() && !m_reopening.empty(); ++i)
        {
            const auto pending = std::find_if(m_reopening.begin(), m_reopening.end(),
                                              [&](Reopening const& entry) { return EqualsIgnoreCase(entry.path, m_known[i].path); });
            if (pending == m_reopening.end())
            {
                continue;
            }
            const size_t wanted = std::min(pending->index, m_known.size() - 1);
            const winrt::guid tabId = m_known[i].tabId; // (ReorderTab rientra qui e rifà m_known)
            m_reopening.erase(pending); // prima dello spostamento: ReorderTab rientra qui dentro
            if (i != wanted)
            {
                try
                {
                    tabsBar.ReorderTab(static_cast<int>(i), static_cast<int>(wanted));
                    // Come Chrome la scheda riaperta resta quella attiva (spostata, il Blocco note riseleziona l'ultimo posto).
                    SelectReopened(tabId);
                }
                catch (...)
                {
                    NPX_LOG(L"ReopenTab: la scheda riaperta non si è potuta rimettere al suo posto (0x%08X).",
                            static_cast<unsigned>(winrt::to_hresult()));
                }
            }
            NPX_VLOG(L"ReopenTab: scheda riaperta al posto %zu (era comparsa al %zu).", wanted, i);
            break;
        }
    }

    void ReopenTab::SelectReopened(winrt::guid const& tabId)
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return;
        }
        // Il Blocco note seleziona la scheda nuova dopo averla aggiunta, cioè dopo questo VectorChanged (in cui la si è
        // spostata): la selezione va rifatta dopo di lui. In coda al dispatcher e, se il caricamento del file la cambia
        // ancora, dopo 400 ms.
        m_selectTabId = tabId;
        const std::weak_ptr<ReopenTab> weak = weak_from_this();
        const auto select = [weak] {
            const auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (const auto bar = self->m_tabsBar.get())
            {
                try
                {
                    bar.SelectTabByTabId(self->m_selectTabId);
                }
                catch (...)
                {
                }
            }
        };
        const auto queue = tabsBar.DispatcherQueue();
        queue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, select);
        if (!m_selectTimer)
        {
            m_selectTimer = queue.CreateTimer();
            m_selectTimer.IsRepeating(false);
            m_selectTimer.Interval(std::chrono::milliseconds(400));
            m_selectTimer.Tick([select](auto&&, auto&&) { select(); });
        }
        m_selectTimer.Stop();
        m_selectTimer.Start();
    }

    void ReopenTab::OnTimer()
    {
        if (!m_enabled)
        {
            return;
        }
        SyncHotkey();
        Refresh();
        ConfirmClosures(false);
        const ULONGLONG now = GetTickCount64();
        std::erase_if(m_reopening, [now](Reopening const& entry) {
            if (now - entry.started <= kReopenTimeoutMs)
            {
                return false;
            }
            NPX_VLOG(L"ReopenTab: la scheda chiesta non è comparsa.");
            return true;
        });
    }

    void ReopenTab::ConfirmClosures(bool immediately)
    {
        const ULONGLONG now = GetTickCount64();
        std::vector<StoredTab> confirmed;
        for (auto it = m_gone.begin(); it != m_gone.end();)
        {
            // Una scheda sparita da poco può essere in viaggio per un riordino; al comando invece non
            // si aspetta: se non è tra quelle vive (Refresh l'ha appena verificato) è chiusa davvero.
            if (!immediately && now - it->when < kConfirmMs)
            {
                ++it;
                continue;
            }
            if (LooksLikeFilePath(it->path))
            {
                confirmed.push_back(StoredTab{ it->path, it->index });
            }
            else
            {
                // Scheda mai salvata: non si può riaprire. Nel log solo la forma della descrizione, non
                // il suo contenuto.
                NPX_VLOG(L"ReopenTab: scheda chiusa senza file salvato (descrizione di %zu caratteri): non si riapre.", it->path.size());
            }
            it = m_gone.erase(it);
        }
        if (confirmed.empty())
        {
            return;
        }
        StoreLock lock;
        auto stored = LoadStored();
        for (auto& entry : confirmed)
        {
            stored.push_back(std::move(entry));
        }
        if (stored.size() > kMaxClosed)
        {
            stored.erase(stored.begin(), stored.end() - static_cast<std::ptrdiff_t>(kMaxClosed));
        }
        SaveStored(stored);
        NPX_VLOG(L"ReopenTab: scheda chiusa ricordata (%zu nella cronologia).", stored.size());
    }

    void ReopenTab::Reopen()
    {
        if (!m_enabled)
        {
            return;
        }
        // Premendo subito dopo aver chiuso una scheda (il riflesso tipico) la chiusura potrebbe non
        // essere ancora confermata: si guarda adesso com'è l'elenco e si conferma senza aspettare.
        Refresh();
        ConfirmClosures(true);
        StoredTab chosen;
        size_t left = 0;
        {
            StoreLock lock;
            auto stored = LoadStored();
            while (!stored.empty())
            {
                StoredTab item = std::move(stored.back());
                stored.pop_back();
                // Già aperta in questa finestra (per esempio tornata con la sessione): si prova con quella chiusa prima.
                if (std::any_of(m_known.begin(), m_known.end(), [&](Known const& entry) { return EqualsIgnoreCase(entry.path, item.path); }))
                {
                    continue;
                }
                chosen = std::move(item);
                break;
            }
            SaveStored(stored);
            left = stored.size();
        }
        if (chosen.path.empty())
        {
            NPX_LOG(L"ReopenTab: Ctrl+Maiusc+T, nessuna scheda chiusa da riaprire.");
            return;
        }
        m_reopening.push_back(Reopening{ chosen.path, chosen.index, GetTickCount64() });
        OpenInNotepad(chosen.path);
        NPX_LOG(L"ReopenTab: riapro una scheda chiusa (%zu ancora nella cronologia).", left);
    }

    void ReopenTab::ReopenIfActive()
    {
        // Il comando vale per la finestra attiva, come la scorciatoia: le altre non fanno nulla (se non
        // ce n'è nessuna attiva, nel log resta scritto perché non è successo niente).
        if (!m_enabled || !m_main)
        {
            return;
        }
        if (GetForegroundWindow() != m_main)
        {
            NPX_VLOG(L"ReopenTab: comando ignorato, questa finestra non è attiva.");
            return;
        }
        Reopen();
    }

    void ReopenTab::SyncHotkey()
    {
        // Solo mentre questa finestra è davvero quella in uso: una finestra ridotta a icona può restare
        // "in primo piano" per Windows. Senza il subclass WM_HOTKEY non arriverebbe a nessuno: meglio
        // lasciare la combinazione agli altri programmi.
        const bool wanted = m_enabled && m_subclassed && m_main && IsWindow(m_main) && !IsIconic(m_main) && GetForegroundWindow() == m_main;
        if (wanted)
        {
            Register();
        }
        else
        {
            Unregister();
        }
    }

    void ReopenTab::Register()
    {
        if (m_registered || !m_main)
        {
            return;
        }
        // MOD_NOREPEAT: tenendo premuto non si ripete (una scheda per pressione).
        if (!RegisterHotKey(m_main, kHotkeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'T'))
        {
            if (!m_failedLogged)
            {
                m_failedLogged = true;
                NPX_LOG(L"ReopenTab: Ctrl+Maiusc+T non disponibile (errore %lu): forse la tiene un altro programma.", GetLastError());
            }
            return;
        }
        m_registered = true;
        m_failedLogged = false;
        NPX_VLOG(L"ReopenTab: Ctrl+Maiusc+T attiva per la finestra %p.", m_main);
    }

    void ReopenTab::Unregister()
    {
        if (!m_registered)
        {
            return;
        }
        UnregisterHotKey(m_main, kHotkeyId);
        m_registered = false;
        NPX_VLOG(L"ReopenTab: Ctrl+Maiusc+T restituita.");
    }

    LRESULT CALLBACK ReopenTab::SubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
    {
        auto* self = reinterpret_cast<ReopenTab*>(refData);
        if (message == WM_NCDESTROY)
        {
            // Si aggiorna lo stato PRIMA di proseguire la catena: più avanti nella catena il Blocco note
            // avvisa che la finestra si chiude, e la mod libera le feature di quella finestra, cioè
            // distrugge questo stesso oggetto. Dopo DefSubclassProc "self" non esiste più.
            if (self)
            {
                if (self->m_registered)
                {
                    // La registrazione è legata al THREAD, non alla finestra: il thread della finestra
                    // sopravvive alla sua distruzione, quindi senza togliere la combinazione adesso
                    // (la finestra esiste ancora, siamo in WM_NCDESTROY) resterebbe occupata per
                    // sempre, togliendola a tutti gli altri programmi.
                    UnregisterHotKey(hwnd, kHotkeyId);
                    self->m_registered = false;
                }
                self->m_subclassed = false;
            }
            RemoveWindowSubclass(hwnd, &SubclassProc, kSubclassId);
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }
        if (self && message == WM_HOTKEY && wParam == kHotkeyId)
        {
            try
            {
                self->Reopen();
            }
            catch (...)
            {
                NPX_LOG(L"ReopenTab: errore riaprendo la scheda (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            }
            return 0;
        }
        if (self && message == WM_ACTIVATE)
        {
            // Finestra attiva: la combinazione è nostra; disattivata: torna agli altri programmi.
            if (LOWORD(wParam) == WA_INACTIVE)
            {
                self->Unregister();
            }
            else
            {
                self->Register();
            }
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
}
