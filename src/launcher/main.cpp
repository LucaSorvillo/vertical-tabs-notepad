// VTabsNotepad.exe - launcher con icona nella tray.
//
// Resta in background, rileva ogni istanza del Blocco note (versione Store) e vi inietta
// VTabsNotepadMod.dll. Dal menu della tray si attivano/disattivano le feature: la mod osserva
// settings.ini e applica le modifiche al volo.
//
// Riga di comando:
//   VTabsNotepad.exe            avvia la tray (istanza singola)
//   VTabsNotepad.exe --inject   inietta la mod nei Notepad aperti ed esce
//   VTabsNotepad.exe --dump     chiede ai Notepad aperti di esportare l'albero XAML ed esce
//   VTabsNotepad.exe --reopen-tab  riapre la scheda chiusa nella finestra attiva (come Ctrl+Maiusc+T)
//   VTabsNotepad.exe --exit     chiude l'istanza della tray in esecuzione (come "Esci": la mod si spegne nei Notepad aperti)
//   VTabsNotepad.exe --exit --keep-mods  (compilazione, prove) chiude la tray lasciando la mod accesa

#include "Injector.h"
#include "Settings.h"
#include "Shared.h"
#include "TrayIcon.h"

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace
{
    constexpr wchar_t kWindowClass[] = L"VTabsNotepad.Launcher";
    constexpr wchar_t kMutexName[] = L"Local\\VTabsNotepad.Launcher";
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr UINT WM_TRAY = WM_APP + 1;
    constexpr UINT_PTR kScanTimer = 1;
    constexpr UINT kScanIntervalMs = 2000;
    constexpr wchar_t kTrayGlyph = L'\xE90C'; // "DockLeft": solo se l'icona a contorno non si disegna

    enum MenuId : UINT
    {
        IdVerticalTabs = 100,
        IdBackToTop,
        IdReopenTab,
        IdNoSmoothScroll,
        IdAutostart,
        IdOpenData,
        IdDumpTree,
        IdDevMode,
        IdExit,
        IdExitKeepMods, // --exit --keep-mods (non nel menu)
    };

    bool g_modsOffOnExit = false; // "Esci" (non --keep-mods)

    // "Esci": la mod si spegne in tutte le finestre del Blocco note aperte (vede comparire il file nella cartella dei dati), come
    // se le funzioni fossero spente dal menu; le impostazioni restano quelle scelte. Si riaccende quando la tray riparte.
    void SignalModsOff();

    // dev-windows.txt limita la mod alle finestre di prova: se resta lì per sbaglio, la feature
    // sembra non funzionare. Il launcher lo segnala e permette di rimuoverlo.
    bool IsDevModeActive()
    {
        return GetFileAttributesW(npx::DevWindowsPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    UINT g_taskbarCreatedMsg = 0;
    UINT g_shellHookMsg = 0;
    HICON g_icon = nullptr;
    std::set<std::pair<DWORD, ULONGLONG>> g_handled; // (pid, ora di creazione) già gestiti
    std::map<std::pair<DWORD, ULONGLONG>, int> g_reinjected; // mod sparita e rimessa (al più 3 volte per processo)

    void LauncherLog(const wchar_t* format, ...)
    {
        wchar_t message[1024];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(message, _TRUNCATE, format, args);
        va_end(args);

        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t line[1200];
        _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u %s\r\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                     now.wSecond, message);
        OutputDebugStringW(line);

        const std::wstring path = npx::LogDirectory() + L"\\launcher.log";
        const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            char utf8[4096];
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
            DWORD written = 0;
            if (bytes > 1)
            {
                WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
            }
            CloseHandle(file);
        }
    }

    std::wstring ExeDirectory()
    {
        wchar_t path[MAX_PATH * 2]{};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        std::wstring dir = path;
        dir.resize(dir.find_last_of(L'\\'));
        return dir;
    }

    ULONGLONG ProcessCreationTime(DWORD pid)
    {
        ULONGLONG result = 0;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (GetProcessTimes(process, &creation, &exit, &kernel, &user))
            {
                result = (static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
            }
            CloseHandle(process);
        }
        return result;
    }

    // Mutex per processo attorno a "c'è già la mod? se no inietta": la tray e un'iniezione esplicita (--inject-pid) quasi
    // insieme caricherebbero la DLL due volte, con due riferimenti (con "Esci" la mod ne lascerebbe uno e la DLL
    // resterebbe nel Blocco note, spenta).
    struct InjectLock
    {
        HANDLE mutex = nullptr;
        explicit InjectLock(DWORD pid)
        {
            const std::wstring name = L"Local\\VTabsNotepad.Inject." + std::to_wstring(pid);
            mutex = CreateMutexW(nullptr, FALSE, name.c_str());
            if (mutex)
            {
                WaitForSingleObject(mutex, 15000);
            }
        }
        ~InjectLock()
        {
            if (mutex)
            {
                ReleaseMutex(mutex);
                CloseHandle(mutex);
            }
        }
        InjectLock(InjectLock const&) = delete;
        InjectLock& operator=(InjectLock const&) = delete;
    };

    // Inietta la mod in un processo Notepad, se non è già stato fatto.
    void HandleProcess(DWORD pid)
    {
        const std::pair key{ pid, ProcessCreationTime(pid) };
        if (g_handled.contains(key))
        {
            return;
        }

        const InjectLock lock(pid);
        const npx::ModState state = npx::GetModState(pid);
        if (state == npx::ModState::Unknown)
        {
            return; // elenco dei moduli non leggibile adesso: si riprova al prossimo controllo
        }
        g_handled.insert(key);
        if (state == npx::ModState::Loaded)
        {
            LauncherLog(L"PID %lu: mod già presente.", pid);
            return;
        }

        const std::wstring dll = npx::PrepareModDll(ExeDirectory() + L"\\" + npx::kModDllName);
        if (dll.empty())
        {
            LauncherLog(L"PID %lu: %s non trovata accanto a VTabsNotepad.exe.", pid, npx::kModDllName);
            return;
        }

        std::wstring error;
        if (npx::InjectDll(pid, dll, error))
        {
            LauncherLog(L"PID %lu: mod iniettata (%s).", pid, dll.c_str());
        }
        else
        {
            LauncherLog(L"PID %lu: iniezione fallita - %s", pid, error.c_str());
        }
    }

    void ScanProcesses()
    {
        const auto processes = npx::FindNotepadProcesses();

        // Dimentica i processi terminati. In quelli gestiti la mod deve esserci ancora: se non c'è (si è tolta per un
        // "Esci" di una tray precedente, finito dopo il nostro controllo), la si rimette.
        std::set<std::pair<DWORD, ULONGLONG>> alive;
        std::set<std::pair<DWORD, ULONGLONG>> running;
        for (auto const& process : processes)
        {
            const std::pair key{ process.pid, ProcessCreationTime(process.pid) };
            running.insert(key);
            if (!g_handled.contains(key))
            {
                continue;
            }
            if (npx::GetModState(process.pid) == npx::ModState::NotLoaded)
            {
                int& count = g_reinjected[key];
                if (count < 3)
                {
                    ++count;
                    LauncherLog(L"PID %lu: la mod non c'è più, si rimette.", process.pid);
                    continue; // non più gestito: HandleProcess la inietta di nuovo
                }
            }
            alive.insert(key);
        }
        g_handled = std::move(alive);
        std::erase_if(g_reinjected, [&running](auto const& entry) { return !running.contains(entry.first); });

        for (auto const& process : processes)
        {
            HandleProcess(process.pid);
        }
    }

    void RequestReopenTab()
    {
        for (auto const& process : npx::FindNotepadProcesses())
        {
            if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, npx::ReopenTabEventName(process.pid).c_str()))
            {
                SetEvent(event);
                CloseHandle(event);
            }
        }
    }

    void RequestDump()
    {
        for (auto const& process : npx::FindNotepadProcesses())
        {
            if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, npx::DumpTreeEventName(process.pid).c_str()))
            {
                SetEvent(event);
                CloseHandle(event);
                LauncherLog(L"PID %lu: richiesto dump dell'albero XAML.", process.pid);
            }
        }
    }

    bool IsAutostartEnabled()
    {
        wchar_t value[MAX_PATH * 2]{};
        DWORD size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, kRunKey, npx::kAppName, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
    }

    void SetAutostart(bool enable)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            return;
        }
        if (enable)
        {
            wchar_t exe[MAX_PATH * 2]{};
            GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
            const std::wstring command = L"\"" + std::wstring{ exe } + L"\"";
            RegSetValueExW(key, npx::kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(key, npx::kAppName);
        }
        RegCloseKey(key);
    }

    void UpdateTrayIcon(HWND hwnd, DWORD message)
    {
        const UINT dpi = GetDpiForWindow(hwnd);
        const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
        // Blocco note a contorno che richiama il Blocco note (scelta dell'utente: monocromo, non l'icona di
        // Windows); il glifo solo se GDI+ non parte.
        HICON icon = npx::CreateNotepadOutlineIcon(size, npx::IsTaskbarLight());
        if (!icon)
        {
            icon = npx::CreateGlyphIcon(kTrayGlyph, size, npx::IsTaskbarLight());
        }

        NOTIFYICONDATAW data{ sizeof(data) };
        data.hWnd = hwnd;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = WM_TRAY;
        data.hIcon = icon;
        wcscpy_s(data.szTip, npx::kAppName); // solo il nome dell'app
        Shell_NotifyIconW(message, &data);
        if (message == NIM_ADD)
        {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }

        if (g_icon)
        {
            DestroyIcon(g_icon);
        }
        g_icon = icon;
    }

    void RemoveTrayIcon(HWND hwnd)
    {
        NOTIFYICONDATAW data{ sizeof(data) };
        data.hWnd = hwnd;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }

    void ShowTrayMenu(HWND hwnd, POINT anchor)
    {
        const npx::Settings settings = npx::Settings::Load();

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (settings.verticalTabs ? MF_CHECKED : 0), IdVerticalTabs, npx::Tr(L"Vertical tabs", L"Schede verticali"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (settings.backToTop ? MF_CHECKED : 0), IdBackToTop, npx::Tr(L"Back-to-top button", L"Pulsante «Torna su»"));
        AppendMenuW(menu, MF_STRING | (settings.reopenTab ? MF_CHECKED : 0), IdReopenTab, npx::Tr(L"Reopen closed tabs with Ctrl+Shift+T", L"Riapri la scheda chiusa con Ctrl+Maiusc+T"));
        AppendMenuW(menu, MF_STRING | (settings.noSmoothScroll ? MF_CHECKED : 0), IdNoSmoothScroll, npx::Tr(L"Turn off smooth scrolling", L"Disattiva lo scorrimento fluido"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), IdAutostart, npx::Tr(L"Start with Windows", L"Avvia con Windows"));
        AppendMenuW(menu, MF_STRING, IdOpenData, npx::Tr(L"Open settings and log folder", L"Apri cartella impostazioni e log"));
        AppendMenuW(menu, MF_STRING, IdDumpTree, npx::Tr(L"Export XAML tree (diagnostics)", L"Esporta albero XAML (diagnostica)"));
        if (IsDevModeActive())
        {
            AppendMenuW(menu, MF_STRING, IdDevMode, npx::Tr(L"Turn off dev mode (test windows only)", L"Disattiva modalità sviluppo (solo finestre di prova)"));
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IdExit, npx::Tr(L"Exit", L"Esci"));

        // Necessario perché il menu si chiuda cliccando altrove.
        SetForegroundWindow(hwnd);
        const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
        TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, anchor.x, anchor.y, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    void OnMenuCommand(HWND hwnd, UINT id)
    {
        npx::Settings settings = npx::Settings::Load();
        switch (id)
        {
        case IdVerticalTabs:
            settings.verticalTabs = !settings.verticalTabs;
            settings.Save();
            break;
        case IdBackToTop:
            settings.backToTop = !settings.backToTop;
            settings.Save();
            break;
        case IdReopenTab:
            settings.reopenTab = !settings.reopenTab;
            settings.Save();
            break;
        case IdNoSmoothScroll:
            settings.noSmoothScroll = !settings.noSmoothScroll;
            settings.Save();
            break;
        case IdAutostart:
            SetAutostart(!IsAutostartEnabled());
            break;
        case IdOpenData:
            ShellExecuteW(hwnd, L"open", npx::DataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdDumpTree:
            RequestDump();
            ShellExecuteW(hwnd, L"open", npx::LogDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdDevMode:
            DeleteFileW(npx::DevWindowsPath().c_str());
            UpdateTrayIcon(hwnd, NIM_MODIFY);
            LauncherLog(L"Modalità sviluppo disattivata (dev-windows.txt rimosso).");
            break;
        case IdExit:
            g_modsOffOnExit = true; // alla fine di wWinMain
            DestroyWindow(hwnd);
            break;
        case IdExitKeepMods:
            LauncherLog(L"Uscita con --keep-mods: la mod resta accesa.");
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
    }

    void SignalModsOff()
    {
        const HANDLE file = CreateFileW(npx::TrayExitedPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            LauncherLog(L"Esci: segnale per la mod non scritto (%lu): la mod resta accesa fino alla chiusura del Blocco note.", GetLastError());
            return;
        }
        static const char kText[] = "VTabsNotepad closed with Exit: the mod is off until the tray starts again.\r\n";
        DWORD written = 0;
        WriteFile(file, kText, sizeof(kText) - 1, &written, nullptr);
        CloseHandle(file);
        LauncherLog(L"Esci: la mod si spegne nelle finestre del Blocco note aperte.");
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_shellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");
            RegisterShellHookWindow(hwnd);
            UpdateTrayIcon(hwnd, NIM_ADD);
            ScanProcesses();
            SetTimer(hwnd, kScanTimer, kScanIntervalMs, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == kScanTimer)
            {
                ScanProcesses();
            }
            return 0;

        case WM_TRAY:
            switch (LOWORD(lParam))
            {
            case WM_CONTEXTMENU:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                // Con NOTIFYICON_VERSION_4 le coordinate del punto di ancoraggio arrivano in wParam.
                ShowTrayMenu(hwnd, POINT{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) });
                break;
            default:
                break;
            }
            return 0;

        case WM_COMMAND:
            OnMenuCommand(hwnd, LOWORD(wParam));
            return 0;

        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
            {
                UpdateTrayIcon(hwnd, NIM_MODIFY);
            }
            return 0;

        case WM_DPICHANGED:
            UpdateTrayIcon(hwnd, NIM_MODIFY);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kScanTimer);
            DeregisterShellHookWindow(hwnd);
            RemoveTrayIcon(hwnd);
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        if (message == g_taskbarCreatedMsg && g_taskbarCreatedMsg != 0)
        {
            UpdateTrayIcon(hwnd, NIM_ADD); // explorer riavviato
            return 0;
        }
        if (message == g_shellHookMsg && g_shellHookMsg != 0)
        {
            if (wParam == HSHELL_WINDOWCREATED)
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(reinterpret_cast<HWND>(lParam), &pid);
                if (pid && npx::IsStoreNotepadPath(npx::ProcessImagePath(pid)))
                {
                    HandleProcess(pid);
                }
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool HasArgument(const wchar_t* argument)
    {
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        bool found = false;
        for (int i = 1; i < count && !found; ++i)
        {
            found = npx::EqualsIgnoreCase(args[i], argument);
        }
        LocalFree(args);
        return found;
    }

    // Valore che segue un'opzione, es. "--inject-pid 1234" -> 1234. 0 se assente.
    DWORD ArgumentValue(const wchar_t* name)
    {
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        DWORD value = 0;
        for (int i = 1; i < count - 1; ++i)
        {
            if (npx::EqualsIgnoreCase(args[i], name))
            {
                value = static_cast<DWORD>(_wtoi(args[i + 1]));
                break;
            }
        }
        LocalFree(args);
        return value;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    if (const DWORD targetPid = ArgumentValue(L"--inject-pid"))
    {
        // Inietta solo nel processo indicato (utile in sviluppo per non toccare le altre finestre).
        if (npx::IsStoreNotepadPath(npx::ProcessImagePath(targetPid)))
        {
            HandleProcess(targetPid);
        }
        else
        {
            LauncherLog(L"--inject-pid %lu: non è un Blocco note (Store) valido.", targetPid);
        }
        return 0;
    }
    if (HasArgument(L"--inject"))
    {
        ScanProcesses();
        return 0;
    }
    if (HasArgument(L"--dump"))
    {
        RequestDump();
        return 0;
    }
    if (HasArgument(L"--reopen-tab"))
    {
        RequestReopenTab();
        return 0;
    }
    if (HasArgument(L"--exit"))
    {
        if (HWND existing = FindWindowW(kWindowClass, nullptr))
        {
            PostMessageW(existing, WM_COMMAND, HasArgument(L"--keep-mods") ? IdExitKeepMods : IdExit, 0);
        }
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Già in esecuzione: si limita a iniettare eventuali nuovi Notepad.
        ScanProcesses();
        return 0;
    }

    // Crea settings.ini con i valori predefiniti, così è facile modificarlo a mano.
    if (GetFileAttributesW(npx::SettingsPath().c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        npx::Settings{}.Save();
    }
    // Chiusa l'ultima volta con "Esci": la mod nei Notepad aperti si riaccende (vede sparire il file).
    const bool modsBackOn = DeleteFileW(npx::TrayExitedPath().c_str()) != FALSE;

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    // Finestra top-level mai mostrata: serve per tray, shell hook e timer.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, npx::kAppName, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!hwnd)
    {
        return 1;
    }
    LauncherLog(L"VTabsNotepad avviato (%s).", ExeDirectory().c_str());
    if (modsBackOn)
    {
        LauncherLog(L"Avvio dopo \"Esci\": la mod si riaccende nelle finestre del Blocco note aperte.");
    }
    if (IsDevModeActive())
    {
        LauncherLog(L"Attenzione: modalità sviluppo attiva (%s): la mod agisce solo sulle finestre elencate.",
                    npx::DevWindowsPath().c_str());
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_modsOffOnExit)
    {
        SignalModsOff();
    }
    LauncherLog(L"VTabsNotepad chiuso.");
    if (g_icon)
    {
        DestroyIcon(g_icon);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
