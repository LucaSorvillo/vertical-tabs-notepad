#include "pch.h"
#include "Glass.h"

#include "Lifetime.h"
#include "Log.h"

#include <dwmapi.h>

namespace npx::glass
{
    namespace
    {
        using ExtendFrameFn = HRESULT(WINAPI*)(HWND, const MARGINS*);

        struct WindowGlass
        {
            std::optional<MARGINS> requested; // ultima zona chiesta dal Blocco note
            std::optional<int> limit;         // nostro limite al margine superiore
            int fallbackTop = 0;
        };

        std::atomic<ExtendFrameFn> g_original{ nullptr };
        std::atomic<bool> g_hooked{ false };
        void** g_slot = nullptr; // voce delle importazioni sostituita (per rimetterla allo scaricamento)
        std::mutex g_installLock;
        std::mutex g_lock;
        std::unordered_map<HWND, WindowGlass> g_windows;

        ExtendFrameFn Original()
        {
            const ExtendFrameFn original = g_original.load();
            return original ? original : &DwmExtendFrameIntoClientArea;
        }

        bool IsSheet(MARGINS const& m)
        {
            return m.cxLeftWidth < 0 || m.cxRightWidth < 0 || m.cyTopHeight < 0 || m.cyBottomHeight < 0;
        }

        MARGINS Limited(MARGINS m, int limit)
        {
            if (IsSheet(m))
            {
                return MARGINS{ 0, 0, limit, 0 }; // tutta la finestra di vetro: resta solo la fascia in cima
            }
            m.cyTopHeight = std::min(m.cyTopHeight, limit);
            return m;
        }

        MARGINS Effective(WindowGlass const& glass)
        {
            const MARGINS base = glass.requested.value_or(MARGINS{ 0, 0, glass.fallbackTop, 0 });
            return glass.limit ? Limited(base, *glass.limit) : base;
        }

        HRESULT WINAPI ExtendFrameHook(HWND hwnd, const MARGINS* margins)
        {
            const lifetime::CallScope call;
            if (!margins)
            {
                return Original()(hwnd, margins);
            }
            MARGINS applied = *margins;
            {
                std::scoped_lock lock(g_lock);
                std::erase_if(g_windows, [](auto const& entry) { return !IsWindow(entry.first); });
                WindowGlass& glass = g_windows[hwnd];
                glass.requested = *margins;
                if (glass.limit)
                {
                    applied = Limited(*margins, *glass.limit);
                }
            }
            if (applied.cyTopHeight != margins->cyTopHeight)
            {
                NPX_VLOG(L"Zona vetro: il Blocco note chiede %d px in cima, ridotti a %d (editor salito).", margins->cyTopHeight,
                         applied.cyTopHeight);
            }
            return Original()(hwnd, &applied);
        }

        // Voce della tabella delle importazioni (IAT) di 'module' per dll!function, o nullptr.
        void** FindImport(HMODULE module, const char* dll, const char* function)
        {
            auto* base = reinterpret_cast<BYTE*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return nullptr;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return nullptr;
            }
            const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (directory.VirtualAddress == 0 || directory.Size == 0)
            {
                return nullptr;
            }
            for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); descriptor->Name;
                 ++descriptor)
            {
                if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll) != 0)
                {
                    continue;
                }
                auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
                const auto* name = reinterpret_cast<const IMAGE_THUNK_DATA*>(
                    base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
                for (; name->u1.AddressOfData; ++name, ++slot)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal))
                    {
                        continue;
                    }
                    const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData);
                    if (strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                    {
                        return reinterpret_cast<void**>(&slot->u1.Function);
                    }
                }
            }
            return nullptr;
        }
    }

    bool Install()
    {
        std::scoped_lock lock(g_installLock);
        if (g_hooked.load())
        {
            return true;
        }
        void** slot = FindImport(GetModuleHandleW(nullptr), "dwmapi.dll", "DwmExtendFrameIntoClientArea");
        if (!slot)
        {
            NPX_LOG(L"Zona vetro: DwmExtendFrameIntoClientArea non è tra le importazioni del Blocco note (niente aggancio).");
            return false;
        }
        void* const current = *slot;
        if (current != reinterpret_cast<void*>(&ExtendFrameHook))
        {
            DWORD protect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect))
            {
                NPX_LOG(L"Zona vetro: tabella delle importazioni non modificabile (%lu).", GetLastError());
                return false;
            }
            g_original.store(reinterpret_cast<ExtendFrameFn>(current)); // prima dello scambio: l'aggancio la usa subito
            const bool swapped = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&ExtendFrameHook), current) == current;
            VirtualProtect(slot, sizeof(void*), protect, &protect);
            if (!swapped)
            {
                NPX_LOG(L"Zona vetro: la voce delle importazioni è cambiata mentre la si agganciava (niente aggancio).");
                return false;
            }
        }
        g_slot = slot;
        g_hooked = true;
        NPX_LOG(L"Zona vetro: aggancio di DwmExtendFrameIntoClientArea installato.");
        return true;
    }

    bool Uninstall()
    {
        std::scoped_lock lock(g_installLock);
        if (!g_slot)
        {
            g_hooked = false;
            return true;
        }
        void* const original = reinterpret_cast<void*>(g_original.load());
        DWORD protect = 0;
        if (!original || !VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &protect))
        {
            NPX_LOG(L"Zona vetro: voce delle importazioni non ripristinabile (%lu).", GetLastError());
            return false;
        }
        const bool ours = InterlockedCompareExchangePointer(g_slot, original, reinterpret_cast<void*>(&ExtendFrameHook)) ==
                          reinterpret_cast<void*>(&ExtendFrameHook);
        VirtualProtect(g_slot, sizeof(void*), protect, &protect);
        if (!ours)
        {
            NPX_LOG(L"Zona vetro: la voce delle importazioni non è più la nostra (un altro aggancio sopra): la mod non si scarica.");
            return false;
        }
        g_slot = nullptr;
        g_hooked = false;
        {
            std::scoped_lock windows(g_lock);
            g_windows.clear();
        }
        NPX_LOG(L"Zona vetro: importazione originale rimessa.");
        return true;
    }

    bool Hooked()
    {
        return g_hooked;
    }

    void SetTopLimit(HWND hwnd, std::optional<int> limitPx, int fallbackTopPx)
    {
        if (!hwnd)
        {
            return;
        }
        MARGINS target{};
        {
            std::scoped_lock lock(g_lock);
            WindowGlass& glass = g_windows[hwnd];
            glass.fallbackTop = fallbackTopPx;
            if (glass.limit == limitPx)
            {
                return;
            }
            glass.limit = limitPx;
            target = Effective(glass);
        }
        const HRESULT hr = Original()(hwnd, &target);
        NPX_LOG(L"Zona vetro: margine superiore %d px (%s, 0x%08X).", target.cyTopHeight,
                limitPx ? L"fino all'editor salito" : L"quello del Blocco note", static_cast<unsigned>(hr));
    }

    void Reapply(HWND hwnd)
    {
        MARGINS target{};
        {
            std::scoped_lock lock(g_lock);
            const auto it = g_windows.find(hwnd);
            if (it == g_windows.end() || !it->second.limit)
            {
                return;
            }
            target = Effective(it->second);
        }
        Original()(hwnd, &target);
    }

    void Forget(HWND hwnd)
    {
        std::scoped_lock lock(g_lock);
        g_windows.erase(hwnd);
    }
}
