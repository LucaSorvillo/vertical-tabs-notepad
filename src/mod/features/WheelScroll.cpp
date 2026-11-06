#include "pch.h"
#include "WheelScroll.h"

#include "Interop.h"
#include "Log.h"
#include "Shared.h"

#include <richedit.h> // EM_GETSCROLLPOS / EM_SETSCROLLPOS
#include <unordered_set>

namespace npx
{
    namespace
    {
        bool HasClass(HWND hwnd, const wchar_t* name)
        {
            wchar_t cls[64]{};
            return hwnd && GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, name) == 0;
        }

        // Superficie di testo di un editor del Blocco note (RichEditD2DPT dentro NotepadTextBox) a cui va la rotella, o null.
        HWND TextSurface(HWND target)
        {
            if (HasClass(target, L"RichEditD2DPT") && HasClass(GetParent(target), L"NotepadTextBox"))
            {
                return target;
            }
            if (HasClass(target, L"NotepadTextBox"))
            {
                return FindWindowExW(target, nullptr, L"RichEditD2DPT", nullptr);
            }
            return nullptr;
        }

        // Frazioni di scatto non ancora scorse, per thread dell'editor (la procedura dell'hook gira lì; un thread per scheda).
        struct WheelState
        {
            HWND surface = nullptr;
            int lineUnits = 0; // righe x WHEEL_DELTA
            int pageUnits = 0; // per "una schermata alla volta"
        };
        std::mutex g_stateLock;
        std::unordered_map<DWORD, WheelState> g_states;
        // Solo per le prove (dev-wheel-pixels.txt nella cartella dati, letto a ogni Apply): salta lo scorrimento a righe e
        // usa subito quello a pixel, per verificarlo senza dover riprodurre le righe rifiutate.
        std::atomic<bool> g_forcePixels{ false };

        // Passo che sostituisce lo scatto: righe (con segno: + verso la fine), "una schermata" (+1/-1) o niente (frazione di
        // scatto: si accumula).
        struct Step
        {
            int lines = 0;
            int pages = 0;
        };

        Step NextStep(HWND surface, int delta)
        {
            Step step;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == 0)
            {
                return step; // rotella senza scorrimento nelle impostazioni di Windows
            }
            std::scoped_lock lock(g_stateLock);
            WheelState& state = g_states[GetCurrentThreadId()];
            if (state.surface != surface)
            {
                state = WheelState{ surface };
            }
            if (lines == WHEEL_PAGESCROLL)
            {
                if ((state.pageUnits > 0) != (delta > 0))
                {
                    state.pageUnits = 0; // cambio di verso: il resto di prima non conta più
                }
                state.pageUnits += delta;
                if (std::abs(state.pageUnits) >= WHEEL_DELTA)
                {
                    const bool up = state.pageUnits > 0;
                    state.pageUnits += up ? -WHEEL_DELTA : WHEEL_DELTA;
                    step.pages = up ? -1 : 1;
                }
                return step;
            }
            if (state.lineUnits != 0 && (state.lineUnits > 0) != (delta > 0))
            {
                state.lineUnits = 0;
            }
            state.lineUnits += delta * static_cast<int>(std::min<UINT>(lines, 1000));
            const int whole = state.lineUnits / WHEEL_DELTA; // verso lo zero
            if (whole != 0)
            {
                state.lineUnits -= whole * WHEEL_DELTA;
                step.lines = -whole; // rotella in su (delta > 0) = testo verso l'inizio
            }
            return step;
        }

        void ForgetRemainder()
        {
            std::scoped_lock lock(g_stateLock);
            g_states.erase(GetCurrentThreadId());
        }

        // Posizione interna della RichEdit (pixel del documento): si aggiorna subito, a differenza di quella della barra.
        POINT ScrollPos(HWND surface)
        {
            POINT pt{};
            SendMessageW(surface, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&pt));
            return pt;
        }

        // Altezza di una riga vicino alla prima visibile (pixel), 0 se non si ricava.
        int LineHeight(HWND surface)
        {
            const auto count = static_cast<int>(SendMessageW(surface, EM_GETLINECOUNT, 0, 0));
            int first = static_cast<int>(SendMessageW(surface, EM_GETFIRSTVISIBLELINE, 0, 0));
            if (count < 2)
            {
                return 0;
            }
            first = std::clamp(first, 0, count - 2);
            const LRESULT a = SendMessageW(surface, EM_LINEINDEX, first, 0);
            const LRESULT b = SendMessageW(surface, EM_LINEINDEX, first + 1, 0);
            if (a < 0 || b < 0)
            {
                return 0;
            }
            POINTL pa{};
            POINTL pb{};
            SendMessageW(surface, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&pa), a);
            SendMessageW(surface, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&pb), b);
            const LONG height = pb.y - pa.y;
            return height > 0 && height < 2000 ? static_cast<int>(height) : 0;
        }

        // Fine vera del testo per la RichEdit (pixel del documento): y massima = nMax - nPage, dove si fermano SB_BOTTOM, le
        // righe (EM_LINESCROLL) e la rotella nativa. Senza corsa (testo che ci sta: pagina 0 o massimo < pagina) è 0. -1 se
        // non si sa: nessuna informazione, o la corsa fasulla con il massimo verticale uguale all'orizzontale (ScrollRangeFix).
        LONG LastScrollY(HWND surface)
        {
            SCROLLINFO vertical{ sizeof(vertical), SIF_RANGE | SIF_PAGE };
            if (!GetScrollInfo(surface, SB_VERT, &vertical))
            {
                return -1;
            }
            if (vertical.nPage == 0 || vertical.nMax < static_cast<int>(vertical.nPage))
            {
                return 0;
            }
            SCROLLINFO horizontal{ sizeof(horizontal), SIF_RANGE | SIF_PAGE };
            if (GetScrollInfo(surface, SB_HORZ, &horizontal) && horizontal.nMax == vertical.nMax && horizontal.nPage > 0 &&
                horizontal.nMax >= static_cast<int>(horizontal.nPage))
            {
                return -1;
            }
            return std::max<LONG>(0, vertical.nMax - static_cast<LONG>(vertical.nPage));
        }

        // Scorre subito dell'equivalente dello scatto. false se l'editor non si è mosso in nessun modo: allora lo scatto torna
        // al Blocco note (animazione, ma si muove; un caso visto: righe rifiutate da una scheda con zoom e "a capo
        // automatico", in uno stato interno della RichEdit che poi l'animazione sblocca; in fondo al testo non si muove
        // nemmeno lui).
        bool ScrollNow(HWND surface, Step const& step)
        {
            const POINT before = ScrollPos(surface);
            if (!g_forcePixels.load(std::memory_order_relaxed))
            {
                if (step.pages != 0)
                {
                    SendMessageW(surface, WM_VSCROLL, step.pages > 0 ? SB_PAGEDOWN : SB_PAGEUP, 0);
                }
                else
                {
                    SendMessageW(surface, EM_LINESCROLL, 0, step.lines);
                }
                if (ScrollPos(surface).y != before.y)
                {
                    return true;
                }
            }
            // A righe non si è mosso (in fondo, testo che ci sta, o righe rifiutate): a pixel, della stessa distanza.
            int distance = 0;
            if (step.pages != 0)
            {
                SCROLLINFO info{ sizeof(info), SIF_PAGE };
                if (GetScrollInfo(surface, SB_VERT, &info) && info.nPage > 0)
                {
                    distance = step.pages * static_cast<int>(info.nPage);
                }
            }
            else if (const int height = LineHeight(surface))
            {
                distance = step.lines * height;
            }
            if (distance != 0)
            {
                LONG y = before.y + distance;
                if (distance > 0)
                {
                    // Mai oltre la fine: EM_SETSCROLLPOS, a differenza delle righe, la accetta e porta il testo nel vuoto (in
                    // fondo al documento, o in uno che ci sta in altezza), da dove poi torna indietro a scatti.
                    const LONG last = LastScrollY(surface);
                    if (last < 0 || before.y >= last)
                    {
                        return false;
                    }
                    y = std::min(y, last);
                }
                POINT target{ before.x, std::max<LONG>(0, y) };
                SendMessageW(surface, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&target));
                if (ScrollPos(surface).y != before.y)
                {
                    return true;
                }
            }
            return false;
        }

        // Sui thread degli editor: lo scatto di rotella verticale sul testo diventa uno scorrimento immediato.
        LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam)
        {
            if (code == HC_ACTION && wParam == PM_REMOVE)
            {
                auto* msg = reinterpret_cast<MSG*>(lParam);
                // Ctrl+rotella = zoom, Maiusc+rotella: restano del Blocco note.
                if (msg->message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(msg->wParam) & (MK_CONTROL | MK_SHIFT)) == 0)
                {
                    if (const HWND surface = TextSurface(msg->hwnd))
                    {
                        const Step step = NextStep(surface, GET_WHEEL_DELTA_WPARAM(msg->wParam));
                        if (step.lines == 0 && step.pages == 0)
                        {
                            msg->message = WM_NULL; // frazione di scatto: si accumula
                        }
                        else if (ScrollNow(surface, step))
                        {
                            msg->message = WM_NULL; // fatto, senza animazione
                        }
                        else
                        {
                            ForgetRemainder(); // lo scatto va al Blocco note così com'è
                        }
                    }
                }
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
    }

    WheelScroll::~WheelScroll()
    {
        UnhookAll();
    }

    HWND WheelScroll::MainWindow()
    {
        if (m_main && IsWindow(m_main))
        {
            return m_main;
        }
        m_main = nullptr;
        try
        {
            if (const auto tabsBar = m_tabsBar.get())
            {
                if (const auto xamlRoot = tabsBar.XamlRoot())
                {
                    if (const auto environment = xamlRoot.ContentIslandEnvironment())
                    {
                        m_main = GetAncestor(interop::HwndFromWindowId(environment.AppWindowId()), GA_ROOT);
                    }
                }
            }
        }
        catch (...)
        {
        }
        return m_main;
    }

    void WheelScroll::Apply(Settings const& settings)
    {
        const HWND main = MainWindow();
        if (!main)
        {
            return; // ci riprova il prossimo Apply (la finestra si sta ancora costruendo)
        }
        g_forcePixels = GetFileAttributesW((DataDirectory() + L"\\dev-wheel-pixels.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
        // Gate per-finestra (sviluppo): se esiste dev-windows.txt agiamo solo sulle finestre elencate.
        const bool wanted = settings.noSmoothScroll && WindowApplyAllowed(main);
        if (wanted && !m_enabled)
        {
            m_enabled = true;
            HookEditors();
            NPX_LOG(L"WheelScroll: scorrimento fluido della rotella disattivato (%zu editor agganciati).", m_hooks.size());
        }
        else if (!wanted && m_enabled)
        {
            m_enabled = false;
            UnhookAll();
            NPX_LOG(L"WheelScroll: di nuovo con lo scorrimento fluido del Blocco note.");
        }
    }

    void WheelScroll::OnTimer()
    {
        if (m_enabled)
        {
            HookEditors();
        }
    }

    void WheelScroll::Detach()
    {
        m_enabled = false;
        UnhookAll();
    }

    void WheelScroll::HookEditors()
    {
        const HWND main = MainWindow();
        if (!main)
        {
            return;
        }
        // Un editor per scheda aperta (anche nascosto), ciascuno sul suo thread.
        std::unordered_set<DWORD> current;
        for (HWND child = GetWindow(main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (!HasClass(child, L"NotepadTextBox"))
            {
                continue;
            }
            const HWND surface = FindWindowExW(child, nullptr, L"RichEditD2DPT", nullptr);
            const DWORD thread = GetWindowThreadProcessId(surface ? surface : child, nullptr);
            if (!thread)
            {
                continue;
            }
            current.insert(thread);
            if (m_hooks.contains(thread))
            {
                continue;
            }
            // Thread dello stesso processo e procedura nella nostra DLL (già caricata): hMod nullo.
            if (const HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, thread))
            {
                m_hooks.emplace(thread, hook);
                NPX_VLOG(L"WheelScroll: editor del thread %lu agganciato.", thread);
            }
            else if (!m_failureLogged)
            {
                m_failureLogged = true;
                NPX_LOG(L"WheelScroll: aggancio dell'editor del thread %lu non riuscito (%lu).", thread, GetLastError());
            }
        }
        // Editor spariti (schede chiuse): via i loro hook.
        for (auto it = m_hooks.begin(); it != m_hooks.end();)
        {
            if (!current.contains(it->first))
            {
                UnhookWindowsHookEx(it->second);
                {
                    std::scoped_lock lock(g_stateLock);
                    g_states.erase(it->first);
                }
                it = m_hooks.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void WheelScroll::UnhookAll()
    {
        std::scoped_lock lock(g_stateLock);
        for (auto const& [thread, hook] : m_hooks)
        {
            UnhookWindowsHookEx(hook);
            g_states.erase(thread);
        }
        m_hooks.clear();
    }
}
