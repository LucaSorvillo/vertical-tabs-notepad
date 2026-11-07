#include "pch.h"
#include "PositionPin.h"

#include "Interop.h"
#include "Lifetime.h"
#include "Log.h"

namespace npx::pin
{
    namespace
    {
        constexpr UINT_PTR kSubclassId = 0x4E505850; // 'NPXP'

        struct Entry
        {
            RECT natural{};         // dove lo vuole il Blocco note (coordinate client del genitore)
            RECT wanted{};          // dove lo tiene la mod
            bool active = false;    // regola attiva (Set con i due rettangoli diversi; Clear la spegne)
            bool requested = false; // subclass installato o in arrivo
        };

        // Thread dell'editor con una richiesta di subclass in sospeso: hook che la esegue e quante ne mancano.
        struct PendingHook
        {
            HHOOK hook = nullptr;
            int pending = 0;
        };

        std::mutex g_lock;
        std::unordered_map<HWND, Entry> g_entries;
        std::unordered_map<DWORD, PendingHook> g_hooks;
        std::atomic<bool> g_closing{ false }; // la mod si toglie: niente subclass nuovi

        UINT InstallMessage()
        {
            static const UINT message = RegisterWindowMessageW(L"VTabsNotepad.PositionPin");
            return message;
        }

        // Mandato (SendMessage) a una finestra fissata per togliere il subclass dal suo thread.
        UINT RemoveMessage()
        {
            static const UINT message = RegisterWindowMessageW(L"VTabsNotepad.PositionPin.Remove");
            return message;
        }
        constexpr LRESULT kRemoved = 0x4E505852; // 'NPXR'

        bool Same(RECT const& a, RECT const& b)
        {
            return EqualRect(&a, &b) != FALSE;
        }

        LRESULT CALLBACK SubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
        {
            const lifetime::CallScope call;
            if (message == RemoveMessage() && message != 0)
            {
                RemoveWindowSubclass(hwnd, &SubclassProc, kSubclassId);
                std::scoped_lock lock(g_lock);
                g_entries.erase(hwnd);
                return kRemoved;
            }
            if (message == WM_WINDOWPOSCHANGING && lParam)
            {
                auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
                const bool move = (pos->flags & SWP_NOMOVE) == 0;
                const bool size = (pos->flags & SWP_NOSIZE) == 0;
                if (move || size)
                {
                    Entry entry;
                    bool found = false;
                    {
                        std::scoped_lock lock(g_lock);
                        if (const auto it = g_entries.find(hwnd); it != g_entries.end() && it->second.active)
                        {
                            entry = it->second;
                            found = true;
                        }
                    }
                    if (found)
                    {
                        // Il Blocco note chiede di nuovo proprio il suo rettangolo (in tutto ciò che la richiesta cambia) e la
                        // finestra è dove la vuole la mod: resta lì, senza spostamenti né WM_SIZE.
                        const bool naturalAgain =
                            (!move || (pos->x == entry.natural.left && pos->y == entry.natural.top)) &&
                            (!size || (pos->cx == entry.natural.right - entry.natural.left && pos->cy == entry.natural.bottom - entry.natural.top));
                        if (naturalAgain && Same(interop::RectInClientOf(hwnd, GetAncestor(hwnd, GA_PARENT)), entry.wanted))
                        {
                            pos->flags |= SWP_NOMOVE | SWP_NOSIZE;
                            NPX_VLOG(L"PositionPin: %s tenuto nel layout della mod (il Blocco note lo rimetteva a %ld,%ld %ldx%ld).",
                                     interop::ClassOf(hwnd).c_str(), entry.natural.left, entry.natural.top, entry.natural.right - entry.natural.left,
                                     entry.natural.bottom - entry.natural.top);
                        }
                    }
                }
            }
            else if (message == WM_NCDESTROY)
            {
                RemoveWindowSubclass(hwnd, &SubclassProc, kSubclassId);
                std::scoped_lock lock(g_lock);
                g_entries.erase(hwnd);
            }
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        // Sul thread della finestra.
        void Install(HWND hwnd)
        {
            if (!IsWindow(hwnd) || g_closing.load())
            {
                return;
            }
            if (!SetWindowSubclass(hwnd, &SubclassProc, kSubclassId, 0))
            {
                NPX_LOG(L"PositionPin: subclass di %s non installato (%lu).", interop::ClassOf(hwnd).c_str(), GetLastError());
                std::scoped_lock lock(g_lock);
                if (const auto it = g_entries.find(hwnd); it != g_entries.end())
                {
                    it->second.requested = false; // si riproverà al prossimo Set
                }
            }
        }

        // Sui thread degli editor: esegue le richieste di subclass, poi l'hook si toglie.
        LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam)
        {
            const lifetime::CallScope call;
            if (code == HC_ACTION && wParam == PM_REMOVE)
            {
                auto* msg = reinterpret_cast<MSG*>(lParam);
                if (msg->message == InstallMessage() && msg->message != 0)
                {
                    if (msg->hwnd && GetWindowThreadProcessId(msg->hwnd, nullptr) == GetCurrentThreadId())
                    {
                        Install(msg->hwnd);
                    }
                    msg->message = WM_NULL;
                    HHOOK done = nullptr;
                    {
                        std::scoped_lock lock(g_lock);
                        if (const auto it = g_hooks.find(GetCurrentThreadId()); it != g_hooks.end() && --it->second.pending <= 0)
                        {
                            done = it->second.hook;
                            g_hooks.erase(it);
                        }
                    }
                    const LRESULT result = CallNextHookEx(nullptr, code, wParam, lParam);
                    if (done)
                    {
                        UnhookWindowsHookEx(done);
                    }
                    return result;
                }
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
    }

    void Set(HWND hwnd, RECT const& natural, RECT const& wanted)
    {
        if (!hwnd || g_closing.load())
        {
            return;
        }
        bool install = false;
        {
            std::scoped_lock lock(g_lock);
            Entry& entry = g_entries[hwnd];
            entry.natural = natural;
            entry.wanted = wanted;
            entry.active = !Same(natural, wanted);
            if (entry.active && !entry.requested)
            {
                entry.requested = true;
                install = true;
            }
        }
        if (!install)
        {
            return;
        }
        const DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
        if (!thread)
        {
            return;
        }
        if (thread == GetCurrentThreadId())
        {
            Install(hwnd);
            return;
        }
        // Thread dell'editor: il subclass si installa da lì (SetWindowSubclass vale solo sul thread della finestra).
        {
            std::scoped_lock lock(g_lock);
            PendingHook& pending = g_hooks[thread];
            if (!pending.hook)
            {
                // Stesso processo e procedura nella nostra DLL (già caricata): hMod nullo.
                pending.hook = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, thread);
                if (!pending.hook)
                {
                    g_hooks.erase(thread);
                    if (const auto it = g_entries.find(hwnd); it != g_entries.end())
                    {
                        it->second.requested = false;
                    }
                    NPX_LOG(L"PositionPin: hook sul thread %lu non installato (%lu).", thread, GetLastError());
                    return;
                }
            }
            ++pending.pending;
        }
        PostMessageW(hwnd, InstallMessage(), 0, 0);
    }

    void Clear(HWND hwnd)
    {
        std::scoped_lock lock(g_lock);
        if (const auto it = g_entries.find(hwnd); it != g_entries.end())
        {
            it->second.active = false;
        }
    }

    bool Uninstall()
    {
        g_closing = true;
        std::vector<HHOOK> hooks;
        std::vector<HWND> windows;
        {
            std::scoped_lock lock(g_lock);
            for (auto const& [thread, pending] : g_hooks)
            {
                hooks.push_back(pending.hook);
            }
            g_hooks.clear();
            for (auto const& [hwnd, entry] : g_entries)
            {
                windows.push_back(hwnd);
            }
        }
        // Prima gli hook delle richieste in sospeso (un messaggio di installazione ancora in coda arriva poi alla finestra,
        // che lo ignora), poi i subclass, ognuno dal thread della sua finestra.
        for (const HHOOK hook : hooks)
        {
            UnhookWindowsHookEx(hook);
        }
        size_t removed = 0;
        bool ok = true;
        for (const HWND hwnd : windows)
        {
            DWORD_PTR result = 0;
            if (IsWindow(hwnd) &&
                !SendMessageTimeoutW(hwnd, RemoveMessage(), 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 3000, &result))
            {
                ok = false; // thread della finestra fermo: si riprova più tardi
                continue;
            }
            removed += result == kRemoved ? 1 : 0;
            std::scoped_lock lock(g_lock);
            g_entries.erase(hwnd);
        }
        NPX_LOG(L"PositionPin: %zu subclass tolti%s.", removed, ok ? L"" : L" (una finestra non risponde: si riprova)");
        return ok;
    }

    void Resume()
    {
        g_closing = false;
    }
}
