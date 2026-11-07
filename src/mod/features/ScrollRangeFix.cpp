#include "pch.h"
#include "ScrollRangeFix.h"

#include "Interop.h"
#include "Log.h"
#include "Shared.h"

#include <richedit.h> // EM_GETSCROLLPOS

namespace npx
{
    namespace
    {
        bool HasClass(HWND hwnd, const wchar_t* name)
        {
            wchar_t cls[64]{};
            return hwnd && GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, name) == 0;
        }

        // Superficie di testo (RichEditD2DPT) dell'editor visibile della finestra.
        HWND VisibleSurface(HWND main)
        {
            for (HWND child = GetWindow(main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                if (IsWindowVisible(child) && HasClass(child, L"NotepadTextBox"))
                {
                    return FindWindowExW(child, nullptr, L"RichEditD2DPT", nullptr);
                }
            }
            return nullptr;
        }

        // Messaggio all'editor (sta su un altro thread) con un tempo massimo: false se non risponde in tempo.
        bool Ask(HWND surface, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result = nullptr)
        {
            DWORD_PTR value = 0;
            if (!SendMessageTimeoutW(surface, message, wParam, lParam, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 150, &value))
            {
                return false;
            }
            if (result)
            {
                *result = static_cast<LRESULT>(value);
            }
            return true;
        }

        // Inizio della riga "line" in coordinate client (y), o false (riga senza posizione: per esempio quella vuota dopo
        // l'ultimo a capo, che sta dopo l'ultimo carattere). I buffer per le risposte dell'editor stanno nello heap: se il
        // tempo scade, l'editor può ancora elaborare il messaggio più tardi e scriverci; in quel caso (raro) non si liberano.
        bool LineTop(HWND surface, LRESULT line, LONG& y)
        {
            LRESULT start = -1;
            if (!Ask(surface, EM_LINEINDEX, static_cast<WPARAM>(line), 0, &start) || start < 0)
            {
                return false;
            }
            auto* point = new POINTL{ LONG_MIN, LONG_MIN };
            if (!Ask(surface, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(point), start))
            {
                return false; // (point resta: l'editor potrebbe ancora scriverci)
            }
            y = point->y;
            const bool valid = point->x != LONG_MIN && point->y != LONG_MIN;
            delete point;
            return valid;
        }

        // Il testo ci sta in altezza nella pagina? Fondo dell'ultima riga (coordinate del documento) contro la pagina. Si parte
        // dall'ultima riga con una posizione valida (con la precedente dà l'altezza di riga) e si aggiungono quelle dopo.
        // Nessuna risposta certa (editor occupato, altezza di riga sconosciuta) = no: non si tocca nulla.
        bool TextFits(HWND surface, UINT page)
        {
            LRESULT lines = 0;
            if (!Ask(surface, EM_GETLINECOUNT, 0, 0, &lines) || lines < 2)
            {
                return false;
            }
            for (LRESULT line = lines - 1; line >= 1 && line >= lines - 4; --line)
            {
                LONG top = 0;
                LONG previous = 0;
                if (!LineTop(surface, line, top) || !LineTop(surface, line - 1, previous) || top <= previous)
                {
                    continue;
                }
                const LONG height = top - previous;
                if (height <= 0 || height >= 2000)
                {
                    return false;
                }
                auto* scroll = new POINT{};
                if (!Ask(surface, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(scroll)))
                {
                    return false; // (scroll resta: l'editor potrebbe ancora scriverci)
                }
                const LONG bottom = top + scroll->y + height * static_cast<LONG>(lines - line);
                delete scroll;
                return bottom <= static_cast<LONG>(page);
            }
            return false;
        }
    }

    HWND ScrollRangeFix::MainWindow()
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

    ScrollRangeFix::~ScrollRangeFix()
    {
        StopTimer();
    }

    void ScrollRangeFix::Apply(Settings const& settings)
    {
        if (const HWND main = MainWindow())
        {
            // Sempre attiva, tranne con la tray chiusa con "Esci". Gate per-finestra (sviluppo): se esiste dev-windows.txt
            // agiamo solo sulle finestre elencate.
            m_enabled = WindowApplyAllowed(main) && !settings.trayExited;
        }
        if (m_enabled)
        {
            StartTimer();
        }
        else
        {
            StopTimer();
            m_ranges = {}; // rientrando, il primo controllo fa rileggere tutto al Blocco note
        }
    }

    void ScrollRangeFix::OnTimer()
    {
        if (!m_enabled || m_timer)
        {
            return; // (con il timer proprio i controlli sono già ogni 100 ms)
        }
        try
        {
            Check();
        }
        catch (...)
        {
        }
    }

    void ScrollRangeFix::Detach()
    {
        m_enabled = false;
        StopTimer();
    }

    void ScrollRangeFix::StartTimer()
    {
        if (m_timer)
        {
            return;
        }
        try
        {
            const auto tabsBar = m_tabsBar.get();
            const auto queue = tabsBar ? tabsBar.DispatcherQueue() : nullptr;
            if (!queue)
            {
                return; // resta il controllo della finestra ogni 500 ms
            }
            m_timer = queue.CreateTimer();
            m_timer.Interval(std::chrono::milliseconds(100));
            m_timer.IsRepeating(true);
            std::weak_ptr<int> alive = m_alive;
            m_timer.Tick([this, alive](auto&&, auto&&) {
                if (alive.expired() || !m_enabled)
                {
                    return;
                }
                try
                {
                    Check();
                }
                catch (...)
                {
                }
            });
            m_timer.Start();
        }
        catch (...)
        {
            m_timer = nullptr;
        }
    }

    void ScrollRangeFix::StopTimer()
    {
        if (m_timer)
        {
            try
            {
                m_timer.Stop();
            }
            catch (...)
            {
            }
            m_timer = nullptr;
        }
    }

    void ScrollRangeFix::Check()
    {
        const HWND main = MainWindow();
        const HWND surface = main ? VisibleSurface(main) : nullptr;
        if (!surface)
        {
            return;
        }
        // Senza barra (GetScrollInfo non riuscito) valgono come corsa zero: anche il passaggio a "nessuna corsa" va detto.
        SCROLLINFO vertical{ sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS };
        SCROLLINFO horizontal{ sizeof(horizontal), SIF_RANGE | SIF_PAGE };
        const bool hasVertical = GetScrollInfo(surface, SB_VERT, &vertical) != FALSE;
        const bool hasHorizontal = GetScrollInfo(surface, SB_HORZ, &horizontal) != FALSE;
        Ranges now{ surface };
        if (hasVertical)
        {
            now.verticalMax = vertical.nMax;
            now.verticalPage = vertical.nPage;
        }
        if (hasHorizontal)
        {
            now.horizontalMax = horizontal.nMax;
            now.horizontalPage = horizontal.nPage;
        }
        if (hasVertical && hasHorizontal && FixPhantomRange(surface, vertical, horizontal, now))
        {
            return; // il controllo dopo vede la corsa corretta (e la fa rileggere al Blocco note)
        }
        RefreshBarsIfChanged(now);
    }

    bool ScrollRangeFix::FixPhantomRange(HWND surface, SCROLLINFO const& vertical, SCROLLINFO const& horizontal, Ranges const& now)
    {
        // Firma del difetto: massimo verticale = orizzontale, con una corsa verticale e la barra orizzontale necessaria.
        const bool signature = vertical.nMax == horizontal.nMax && vertical.nPage > 0 && vertical.nMax >= static_cast<int>(vertical.nPage) &&
                               horizontal.nPage > 0 && horizontal.nMax >= static_cast<int>(horizontal.nPage);
        if (!signature)
        {
            return false;
        }
        // Stesso stato già chiesto all'editor da poco, con risposta "non ci sta" (o incerta): non lo si richiede a ogni tick.
        const ULONGLONG tick = GetTickCount64();
        if (now == m_notPhantom && tick < m_notPhantomUntil)
        {
            return false;
        }
        if (!TextFits(surface, vertical.nPage))
        {
            m_notPhantom = now;
            m_notPhantomUntil = tick + 2000;
            return false;
        }
        // Nessuna corsa verticale: è quella giusta, il testo ci sta. La barra XAML del Blocco note si adegua e sparisce.
        SCROLLINFO fixed{ sizeof(fixed), SIF_RANGE | SIF_PAGE | SIF_POS };
        SetScrollInfo(surface, SB_VERT, &fixed, TRUE);
        if (!m_loggedFirst)
        {
            m_loggedFirst = true;
            NPX_LOG(L"ScrollRangeFix: corsa verticale fasulla dell'editor corretta (massimo %d = orizzontale, pagina %u, il testo ci sta).",
                    vertical.nMax, vertical.nPage);
        }
        else
        {
            NPX_VLOG(L"ScrollRangeFix: corsa verticale fasulla corretta (massimo %d, pagina %u).", vertical.nMax, vertical.nPage);
        }
        return true;
    }

    void ScrollRangeFix::RefreshBarsIfChanged(Ranges const& now)
    {
        if (now == m_ranges)
        {
            return;
        }
        const bool sameEditor = now.surface == m_ranges.surface;
        m_ranges = now;
        const HWND parent = GetParent(now.surface);
        if (!parent)
        {
            return;
        }
        // Come la RichEdit prima di ridisegnarsi: il Blocco note rilegge le corse e aggiorna le sue barre (vedi ScrollRangeFix.h).
        PostMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(now.surface), EN_UPDATE), reinterpret_cast<LPARAM>(now.surface));
        if (sameEditor && !m_loggedRefresh)
        {
            m_loggedRefresh = true;
            NPX_LOG(L"ScrollRangeFix: corse dell'editor cambiate (verticale %d/%u, orizzontale %d/%u): il Blocco note le rilegge per le "
                    L"sue barre (EN_UPDATE).",
                    now.verticalMax, now.verticalPage, now.horizontalMax, now.horizontalPage);
        }
        else
        {
            NPX_VLOG(L"ScrollRangeFix: barre del Blocco note aggiornate (%s; verticale %d/%u, orizzontale %d/%u).",
                     sameEditor ? L"corse cambiate" : L"editor nuovo", now.verticalMax, now.verticalPage, now.horizontalMax, now.horizontalPage);
        }
    }
}
