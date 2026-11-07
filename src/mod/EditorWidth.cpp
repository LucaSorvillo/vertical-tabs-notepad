#include "pch.h"
#include "EditorWidth.h"

#include "Interop.h"
#include "Lifetime.h"
#include "Log.h"

#include <intrin.h> // _ReturnAddress

namespace npx::editorwidth
{
    namespace
    {
        using GetClientRectFn = BOOL(WINAPI*)(HWND, LPRECT);

        std::atomic<GetClientRectFn> g_original{ nullptr };
        std::atomic<bool> g_hooked{ false };
        void** g_slot = nullptr; // voce delle importazioni sostituita (per rimetterla allo scaricamento)
        std::mutex g_installLock;

        std::mutex g_lock;
        std::unordered_map<HWND, int> g_reductions;    // finestra principale -> pixel coperti dalla colonna
        std::unordered_map<const void*, bool> g_sites; // indirizzo di ritorno -> è il calcolo della larghezza del testo?
        std::atomic<bool> g_any{ false };              // almeno una riduzione (via rapida per tutte le altre chiamate)
        bool g_loggedFirst = false;

        // Le funzioni che il calcolo della larghezza del testo chiama dopo GetClientRect (indirizzi veri, come nelle
        // importazioni di Notepad.exe: GetProcAddress segue anche i rimandi verso kernelbase).
        struct Apis
        {
            const void* dpi = nullptr;
            const void* metrics = nullptr;
            const void* mulDiv = nullptr;
        };
        Apis const& Targets()
        {
            static const Apis apis = [] {
                Apis found;
                if (const HMODULE user32 = GetModuleHandleW(L"user32.dll"))
                {
                    found.dpi = reinterpret_cast<const void*>(GetProcAddress(user32, "GetDpiForWindow"));
                    found.metrics = reinterpret_cast<const void*>(GetProcAddress(user32, "GetSystemMetricsForDpi"));
                }
                if (const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll"))
                {
                    found.mulDiv = reinterpret_cast<const void*>(GetProcAddress(kernel32, "MulDiv"));
                }
                return found;
            }();
            return apis;
        }

        // [p, p + length) sta tutto dentro l'immagine di Notepad.exe?
        bool InMainImage(const BYTE* p, size_t length)
        {
            static const std::pair<const BYTE*, const BYTE*> image = [] {
                const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
                return std::pair{ base, base + nt->OptionalHeader.SizeOfImage };
            }();
            return p >= image.first && p + length <= image.second;
        }

        // Il codice dopo la chiamata (code = indirizzo di ritorno) è il calcolo della larghezza del testo: entro 80 byte, chiamate
        // attraverso le importazioni (FF 15 rel32) a GetDpiForWindow, poi GetSystemMetricsForDpi preceduta da "mov ecx, 2"
        // (SM_CXVSCROLL), poi MulDiv, in quest'ordine.
        bool IsTextWidthSite(const BYTE* code)
        {
            constexpr size_t kWindow = 80;
            Apis const& apis = Targets();
            if (!apis.dpi || !apis.metrics || !apis.mulDiv || !InMainImage(code, kWindow + 8))
            {
                return false;
            }
            int step = 0;        // 0 aspetta GetDpiForWindow, 1 GetSystemMetricsForDpi, 2 MulDiv
            bool vscroll = false; // "mov ecx, 2" visto dopo GetDpiForWindow
            for (size_t i = 0; i + 6 <= kWindow; ++i)
            {
                if (step == 1 && code[i] == 0xB9 && code[i + 1] == 2 && code[i + 2] == 0 && code[i + 3] == 0 && code[i + 4] == 0)
                {
                    vscroll = true;
                    i += 4;
                    continue;
                }
                if (code[i] != 0xFF || code[i + 1] != 0x15)
                {
                    continue;
                }
                int32_t displacement = 0;
                memcpy(&displacement, code + i + 2, sizeof(displacement));
                const BYTE* slot = code + i + 6 + displacement;
                if (!InMainImage(slot, sizeof(void*)))
                {
                    continue;
                }
                const void* target = *reinterpret_cast<void* const*>(slot);
                if (step == 0 && target == apis.dpi)
                {
                    step = 1;
                }
                else if (step == 1 && vscroll && target == apis.metrics)
                {
                    step = 2;
                }
                else if (step == 2 && target == apis.mulDiv)
                {
                    return true;
                }
                i += 5;
            }
            return false;
        }

        BOOL WINAPI GetClientRectHook(HWND hwnd, LPRECT rect)
        {
            const lifetime::CallScope call;
            const GetClientRectFn original = g_original.load();
            const BOOL ok = original ? original(hwnd, rect) : FALSE;
            if (!ok || !rect || !g_any.load(std::memory_order_relaxed))
            {
                return ok;
            }
            const void* site = _ReturnAddress();
            int reduction = 0;
            {
                std::scoped_lock lock(g_lock);
                const auto it = g_reductions.find(hwnd);
                if (it == g_reductions.end())
                {
                    return ok;
                }
                const auto [known, added] = g_sites.try_emplace(site, false);
                if (added)
                {
                    known->second = IsTextWidthSite(static_cast<const BYTE*>(site));
                }
                if (!known->second)
                {
                    return ok;
                }
                reduction = it->second;
                if (!g_loggedFirst)
                {
                    g_loggedFirst = true;
                    NPX_LOG(L"EditorWidth: il Blocco note misura la larghezza del testo (tabelle Markdown) sulla finestra intera: tolti i %d px "
                            L"della colonna.",
                            reduction);
                }
            }
            rect->right = std::max(rect->left, rect->right - reduction);
            return ok;
        }
    }

    bool Install()
    {
        std::scoped_lock lock(g_installLock);
        if (g_hooked.load())
        {
            return true;
        }
        void** slot = interop::FindImport(GetModuleHandleW(nullptr), "user32.dll", "GetClientRect");
        if (!slot)
        {
            NPX_LOG(L"EditorWidth: GetClientRect non è tra le importazioni del Blocco note (niente aggancio).");
            return false;
        }
        void* const current = *slot;
        if (current != reinterpret_cast<void*>(&GetClientRectHook))
        {
            DWORD protect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect))
            {
                NPX_LOG(L"EditorWidth: tabella delle importazioni non modificabile (%lu).", GetLastError());
                return false;
            }
            g_original.store(reinterpret_cast<GetClientRectFn>(current)); // prima dello scambio: l'aggancio la usa subito
            const bool swapped = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&GetClientRectHook), current) == current;
            VirtualProtect(slot, sizeof(void*), protect, &protect);
            if (!swapped)
            {
                NPX_LOG(L"EditorWidth: la voce delle importazioni è cambiata mentre la si agganciava (niente aggancio).");
                return false;
            }
        }
        g_slot = slot;
        g_hooked = true;
        NPX_VLOG(L"EditorWidth: aggancio di GetClientRect installato.");
        return true;
    }

    bool Uninstall()
    {
        std::scoped_lock lock(g_installLock);
        {
            std::scoped_lock state(g_lock);
            g_reductions.clear();
            g_sites.clear();
            g_any = false;
        }
        if (!g_slot)
        {
            g_hooked = false;
            return true;
        }
        void* const original = reinterpret_cast<void*>(g_original.load());
        DWORD protect = 0;
        if (!original || !VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &protect))
        {
            NPX_LOG(L"EditorWidth: voce delle importazioni non ripristinabile (%lu).", GetLastError());
            return false;
        }
        const bool ours = InterlockedCompareExchangePointer(g_slot, original, reinterpret_cast<void*>(&GetClientRectHook)) ==
                          reinterpret_cast<void*>(&GetClientRectHook);
        VirtualProtect(g_slot, sizeof(void*), protect, &protect);
        if (!ours)
        {
            NPX_LOG(L"EditorWidth: la voce delle importazioni non è più la nostra (un altro aggancio sopra): la mod non si scarica.");
            return false;
        }
        g_slot = nullptr;
        g_hooked = false;
        NPX_LOG(L"EditorWidth: importazione originale rimessa.");
        return true;
    }

    void SetReduction(HWND main, int px)
    {
        if (!main)
        {
            return;
        }
        std::scoped_lock lock(g_lock);
        if (px > 0)
        {
            g_reductions[main] = px;
        }
        else
        {
            g_reductions.erase(main);
        }
        g_any = !g_reductions.empty();
    }
}
