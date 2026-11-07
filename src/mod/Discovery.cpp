#include "pch.h"
#include "Discovery.h"

#include "Lifetime.h"
#include "Log.h"

#include <UIAutomationClient.h>

namespace npx::discovery
{
    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        namespace muxap = winrt::Microsoft::UI::Xaml::Automation::Peers;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;
        using winrt::Windows::Foundation::Size;

        // Slot nelle vtable (0-5: IUnknown e IInspectable). Vedi abi<...> in winrt/impl/Microsoft.UI.Xaml*.0.h.
        constexpr size_t kMeasureOverrideSlot = 6;              // IFrameworkElementOverrides
        constexpr size_t kOnApplyTemplateSlot = 8;              // IFrameworkElementOverrides
        constexpr size_t kOnCreateAutomationPeerSlot = 6;       // IUIElementOverrides
        constexpr size_t kGetAutomationControlTypeCoreSlot = 9; // IAutomationPeerOverrides

        using OnApplyTemplateFn = int32_t(__stdcall*)(void* self);
        using MeasureOverrideFn = int32_t(__stdcall*)(void* self, Size available, Size* desired);
        using CreatePeerFn = int32_t(__stdcall*)(void* self, void** peer);
        using GetControlTypeFn = int32_t(__stdcall*)(void* self, int32_t* type);

        // Una voce di vtable sostituita: la funzione di prima (resta anche dopo il ripristino: una chiamata già entrata nel
        // nostro aggancio la usa ancora) e dove sta la voce (per rimetterla allo scaricamento).
        template <typename Fn>
        struct Slot
        {
            const wchar_t* name;
            std::atomic<Fn> original{ nullptr };
            void** entry = nullptr;
        };

        Slot<OnApplyTemplateFn> g_tabViewTemplate{ L"OnApplyTemplate di TabView" };
        Slot<GetControlTypeFn> g_tabItemType{ L"GetAutomationControlTypeCore del peer di TabViewItem" };
        Slot<OnApplyTemplateFn> g_menuBarTemplate{ L"OnApplyTemplate di MenuBar" };
        Slot<GetControlTypeFn> g_menuItemType{ L"GetAutomationControlTypeCore del peer di MenuBarItem" };
        Slot<MeasureOverrideFn> g_scrollBarMeasure{ L"MeasureOverride di NotepadXamlUI.ScrollBar" };
        Slot<CreatePeerFn> g_scrollBarPeer{ L"OnCreateAutomationPeer di NotepadXamlUI.ScrollBar" };

        std::atomic<Callback> g_callback{ nullptr };
        std::atomic<bool> g_installed{ false };
        std::atomic<bool> g_blocked{ false }; // XFG attivo dove servono gli agganci essenziali: niente, per sempre
        std::atomic<bool> g_xfg{ false };     // XFG visto in un modulo (ultimo tentativo)
        std::atomic<int> g_probes{ 0 };       // ricerche UI Automation nostre in corso
        std::mutex g_installLock;

        void Notify(Kind kind, mux::FrameworkElement const& element) noexcept
        {
            if (const Callback callback = g_callback.load())
            {
                callback(kind, element);
            }
        }

        // Primo antenato visuale (o l'elemento stesso) del tipo Target.
        template <typename Target>
        Target Ancestor(mux::DependencyObject node)
        {
            for (int depth = 0; node && depth < 24; ++depth)
            {
                if (auto target = node.try_as<Target>())
                {
                    return target;
                }
                node = muxm::VisualTreeHelper::GetParent(node);
            }
            return nullptr;
        }

        // self è l'interfaccia Abi di un controllo: si presenta se è di tipo Target.
        template <typename Abi, typename Target>
        void NotifyControl(void* self, Kind kind) noexcept
        {
            if (!g_callback.load())
            {
                return;
            }
            try
            {
                Abi overrides{ nullptr };
                winrt::copy_from_abi(overrides, self);
                if (const auto target = overrides.try_as<Target>())
                {
                    Notify(kind, target);
                }
            }
            catch (...)
            {
            }
        }

        // self è l'interfaccia Abi di una barra di scorrimento del Blocco note (la vtable potrebbe essere condivisa con
        // altre classi del Blocco note uguali: lo si controlla con l'interfaccia della classe).
        template <typename Abi>
        void NotifyScrollBar(void* self) noexcept
        {
            if (!g_callback.load())
            {
                return;
            }
            try
            {
                Abi overrides{ nullptr };
                winrt::copy_from_abi(overrides, self);
                if (overrides.try_as<winrt::NotepadXamlUI::ScrollBar>())
                {
                    if (const auto element = overrides.try_as<mux::FrameworkElement>())
                    {
                        Notify(Kind::ScrollBar, element);
                    }
                }
            }
            catch (...)
            {
            }
        }

        // self è il peer di un elemento: si presenta il primo antenato di tipo Target del suo proprietario.
        template <typename Target>
        void NotifyOwnerAncestor(void* self, Kind kind) noexcept
        {
            if (!g_callback.load())
            {
                return;
            }
            try
            {
                muxap::IAutomationPeerOverrides overrides{ nullptr };
                winrt::copy_from_abi(overrides, self);
                if (const auto peer = overrides.try_as<muxap::FrameworkElementAutomationPeer>())
                {
                    if (const auto target = Ancestor<Target>(peer.Owner()))
                    {
                        Notify(kind, target);
                    }
                }
            }
            catch (...)
            {
            }
        }

        int32_t __stdcall TabViewTemplateHook(void* self) noexcept
        {
            const lifetime::CallScope call;
            // Prima l'originale: il modello della barra deve essere applicato come sempre.
            const int32_t result = g_tabViewTemplate.original.load()(self);
            NotifyControl<mux::IFrameworkElementOverrides, muxc::TabView>(self, Kind::TabView);
            return result;
        }

        int32_t __stdcall MenuBarTemplateHook(void* self) noexcept
        {
            const lifetime::CallScope call;
            const int32_t result = g_menuBarTemplate.original.load()(self);
            NotifyControl<mux::IFrameworkElementOverrides, muxc::MenuBar>(self, Kind::MenuBar);
            return result;
        }

        int32_t __stdcall ScrollBarMeasureHook(void* self, Size available, Size* desired) noexcept
        {
            const lifetime::CallScope call;
            const int32_t result = g_scrollBarMeasure.original.load()(self, available, desired);
            NotifyScrollBar<mux::IFrameworkElementOverrides>(self);
            return result;
        }

        int32_t __stdcall ScrollBarPeerHook(void* self, void** peer) noexcept
        {
            const lifetime::CallScope call;
            const int32_t result = g_scrollBarPeer.original.load()(self, peer);
            if (g_probes.load() > 0)
            {
                NotifyScrollBar<mux::IUIElementOverrides>(self);
            }
            return result;
        }

        int32_t __stdcall TabItemTypeHook(void* self, int32_t* type) noexcept
        {
            const lifetime::CallScope call;
            const int32_t result = g_tabItemType.original.load()(self, type);
            if (g_probes.load() > 0)
            {
                NotifyOwnerAncestor<muxc::TabView>(self, Kind::TabView);
            }
            return result;
        }

        int32_t __stdcall MenuItemTypeHook(void* self, int32_t* type) noexcept
        {
            const lifetime::CallScope call;
            const int32_t result = g_menuItemType.original.load()(self, type);
            if (g_probes.load() > 0)
            {
                NotifyOwnerAncestor<muxc::MenuBar>(self, Kind::MenuBar);
            }
            return result;
        }

        // XFG (eXtended Flow Guard) attivo nel modulo: chi chiama attraverso la vtable controllerebbe un'impronta del tipo
        // della funzione che la nostra non ha (chiusura immediata del processo). Oggi i moduli di WinUI e del Blocco note
        // usano solo CFG; se un aggiornamento accendesse XFG, niente aggancio invece di un crash.
        bool ModuleHasXfg(const void* address)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(address), &module) ||
                !module)
            {
                return true; // indirizzo fuori da un modulo: meglio non toccare
            }
            const auto* base = reinterpret_cast<const BYTE*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
            if (!directory.VirtualAddress)
            {
                return false;
            }
            const auto* config = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY*>(base + directory.VirtualAddress);
            if (config->Size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, GuardFlags) + sizeof(DWORD))
            {
                return false; // struttura vecchia, senza GuardFlags: niente XFG
            }
            constexpr DWORD kXfgEnabled = 0x00800000; // IMAGE_GUARD_XFG_ENABLED
            return (config->GuardFlags & kXfgEnabled) != 0;
        }

        // Sostituisce la voce index della vtable dell'interfaccia abi con hook; l'originale si salva PRIMA dello scambio (una
        // chiamata che arriva subito dopo lo trova già). Idempotente.
        template <typename Fn>
        bool PatchSlot(void* abi, size_t index, void* hook, Slot<Fn>& slot)
        {
            void** vtable = *static_cast<void***>(abi);
            void** entry = vtable + index;
            void* const current = *entry;
            if (current == hook)
            {
                slot.entry = entry;
                return true; // già nostro
            }
            if (ModuleHasXfg(current) || ModuleHasXfg(vtable))
            {
                g_xfg = true;
                NPX_LOG(L"Agganci: XFG attivo nel modulo di %s, vtable non toccata.", slot.name);
                return false;
            }
            slot.original.store(reinterpret_cast<Fn>(current));
            DWORD protection = 0;
            if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &protection))
            {
                NPX_LOG(L"Agganci: VirtualProtect non riuscita per %s (%lu).", slot.name, GetLastError());
                return false;
            }
            const bool swapped = InterlockedCompareExchangePointer(entry, hook, current) == current;
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(void*), protection, &ignored);
            if (swapped)
            {
                slot.entry = entry;
            }
            return swapped;
        }

        // Rimette la funzione originale nella voce, se è ancora la nostra.
        template <typename Fn>
        bool RestoreSlot(Slot<Fn>& slot, void* hook)
        {
            if (!slot.entry)
            {
                return true;
            }
            void* const restored = reinterpret_cast<void*>(slot.original.load());
            DWORD protection = 0;
            if (!restored || !VirtualProtect(slot.entry, sizeof(void*), PAGE_READWRITE, &protection))
            {
                NPX_LOG(L"Agganci: voce della vtable di %s non ripristinabile (%lu).", slot.name, GetLastError());
                return false;
            }
            const bool ours = InterlockedCompareExchangePointer(slot.entry, restored, hook) == hook;
            DWORD ignored = 0;
            VirtualProtect(slot.entry, sizeof(void*), protection, &ignored);
            if (!ours)
            {
                NPX_LOG(L"Agganci: la voce della vtable di %s non è più la nostra (un altro aggancio sopra): la mod non si scarica.", slot.name);
                return false;
            }
            slot.entry = nullptr;
            return true;
        }

        void* AbiOf(winrt::Windows::Foundation::IUnknown const& object)
        {
            return winrt::get_abi(object);
        }

        bool InstallOnThisThread()
        {
            // Istanze nostre, mai in un albero: servono solo per arrivare alle vtable delle classi.
            g_xfg = false;
            bool essential = false;
            try
            {
                muxc::TabView tabView;
                const bool templ =
                    PatchSlot(AbiOf(tabView.as<mux::IFrameworkElementOverrides>()), kOnApplyTemplateSlot, reinterpret_cast<void*>(&TabViewTemplateHook), g_tabViewTemplate);
                muxc::TabViewItem item;
                const auto peer = muxap::FrameworkElementAutomationPeer::CreatePeerForElement(item);
                const auto overrides = peer ? peer.try_as<muxap::IAutomationPeerOverrides>() : nullptr;
                const bool type = overrides && PatchSlot(AbiOf(overrides), kGetAutomationControlTypeCoreSlot, reinterpret_cast<void*>(&TabItemTypeHook), g_tabItemType);
                essential = templ && type;
                NPX_LOG(L"Agganci: barra delle schede %s (OnApplyTemplate %s, peer delle schede %s).", essential ? L"pronta" : L"NON pronta",
                        templ ? L"sì" : L"no", type ? L"sì" : L"no");
            }
            catch (...)
            {
                NPX_LOG(L"Agganci della barra delle schede: errore 0x%08X.", static_cast<unsigned>(winrt::to_hresult()));
            }
            if (!essential)
            {
                g_blocked = g_xfg.load();
                return false;
            }

            // Barra dei menu e barre di scorrimento dell'editor: senza, le schede verticali funzionano lo stesso (la barra dei
            // menu resta dov'è, le barre di scorrimento senza adattamenti).
            try
            {
                muxc::MenuBar menuBar;
                const bool templ =
                    PatchSlot(AbiOf(menuBar.as<mux::IFrameworkElementOverrides>()), kOnApplyTemplateSlot, reinterpret_cast<void*>(&MenuBarTemplateHook), g_menuBarTemplate);
                muxc::MenuBarItem item;
                const auto peer = muxap::FrameworkElementAutomationPeer::CreatePeerForElement(item);
                const auto overrides = peer ? peer.try_as<muxap::IAutomationPeerOverrides>() : nullptr;
                const bool type = overrides && PatchSlot(AbiOf(overrides), kGetAutomationControlTypeCoreSlot, reinterpret_cast<void*>(&MenuItemTypeHook), g_menuItemType);
                NPX_LOG(L"Agganci: barra dei menu (OnApplyTemplate %s, peer delle voci %s).", templ ? L"sì" : L"no", type ? L"sì" : L"no");
            }
            catch (...)
            {
                NPX_LOG(L"Agganci della barra dei menu: errore 0x%08X.", static_cast<unsigned>(winrt::to_hresult()));
            }
            try
            {
                // Controllo del Blocco note (classe attivabile del suo pacchetto): una barra nuova, mai mostrata.
                winrt::NotepadXamlUI::ScrollBar bar;
                const bool measure =
                    PatchSlot(AbiOf(bar.as<mux::IFrameworkElementOverrides>()), kMeasureOverrideSlot, reinterpret_cast<void*>(&ScrollBarMeasureHook), g_scrollBarMeasure);
                const bool peer =
                    PatchSlot(AbiOf(bar.as<mux::IUIElementOverrides>()), kOnCreateAutomationPeerSlot, reinterpret_cast<void*>(&ScrollBarPeerHook), g_scrollBarPeer);
                NPX_LOG(L"Agganci: barre di scorrimento dell'editor (MeasureOverride %s, OnCreateAutomationPeer %s).", measure ? L"sì" : L"no",
                        peer ? L"sì" : L"no");
            }
            catch (...)
            {
                NPX_LOG(L"Agganci delle barre di scorrimento: errore 0x%08X.", static_cast<unsigned>(winrt::to_hresult()));
            }
            return true;
        }

        UINT InstallMessage()
        {
            static const UINT message = RegisterWindowMessageW(L"VTabsNotepad.Mod.InstallDiscovery");
            return message;
        }

        std::atomic<int> g_installResult{ -1 }; // -1 nessuna risposta, 0 fallito, 1 riuscito

        LRESULT CALLBACK InstallProc(int code, WPARAM wParam, LPARAM lParam)
        {
            const lifetime::CallScope call;
            const auto* message = reinterpret_cast<CWPSTRUCT const*>(lParam);
            if (code == HC_ACTION && message && message->message == InstallMessage() && g_installResult.load() == -1)
            {
                g_installResult = InstallOnThisThread() ? 1 : 0;
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
    }

    bool Installed()
    {
        return g_installed.load();
    }

    bool Blocked()
    {
        return g_blocked.load();
    }

    bool Install(HWND window, Callback callback)
    {
        g_callback = callback;
        std::scoped_lock lock(g_installLock);
        if (g_installed.load())
        {
            return true;
        }
        if (g_blocked.load())
        {
            return false;
        }
        const DWORD thread = GetWindowThreadProcessId(window, nullptr);
        if (!thread || thread == GetCurrentThreadId())
        {
            return false;
        }
        // Il lavoro va fatto sul thread della finestra (XAML è legato al suo thread): un gancio WH_CALLWNDPROC solo su quel
        // thread, poi un messaggio nostro mandato alla finestra; il gancio lo vede prima della finestra.
        g_installResult = -1;
        // hMod NULL: un thread di questo processo e la funzione nel nostro codice (con il modulo, user32 lo caricherebbe di
        // nuovo per il thread, e quel riferimento in più terrebbe la DLL nel processo dopo "Esci").
        const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, InstallProc, nullptr, thread);
        if (!hook)
        {
            NPX_LOG(L"Agganci: SetWindowsHookEx non riuscita (%lu).", GetLastError());
            return false;
        }
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(window, InstallMessage(), 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 5000, &ignored);
        UnhookWindowsHookEx(hook);
        const bool ok = g_installResult.load() == 1;
        g_installed = ok;
        return ok;
    }

    bool Uninstall()
    {
        std::scoped_lock lock(g_installLock);
        g_callback = nullptr; // un aggancio già in corso non presenta più nulla
        bool ok = RestoreSlot(g_tabViewTemplate, reinterpret_cast<void*>(&TabViewTemplateHook));
        ok = RestoreSlot(g_tabItemType, reinterpret_cast<void*>(&TabItemTypeHook)) && ok;
        ok = RestoreSlot(g_menuBarTemplate, reinterpret_cast<void*>(&MenuBarTemplateHook)) && ok;
        ok = RestoreSlot(g_menuItemType, reinterpret_cast<void*>(&MenuItemTypeHook)) && ok;
        ok = RestoreSlot(g_scrollBarMeasure, reinterpret_cast<void*>(&ScrollBarMeasureHook)) && ok;
        ok = RestoreSlot(g_scrollBarPeer, reinterpret_cast<void*>(&ScrollBarPeerHook)) && ok;
        if (ok)
        {
            g_installed = false;
        }
        NPX_LOG(L"Agganci: funzioni originali rimesse nelle vtable (%s).", ok ? L"tutte" : L"NON tutte");
        return ok;
    }

    bool Probe(HWND island)
    {
        if (!g_installed.load() || !IsWindow(island))
        {
            return false;
        }
        try
        {
            winrt::com_ptr<IUIAutomation> automation;
            winrt::check_hresult(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), automation.put_void()));
            winrt::com_ptr<IUIAutomationElement> root;
            winrt::check_hresult(automation->ElementFromHandle(island, root.put()));
            // Una condizione sul tipo di controllo: per valutarla UI Automation chiede il tipo di ogni elemento dell'isola
            // (e XAML crea o cerca il peer di ognuno), quindi la visita passa da tutti.
            VARIANT type{};
            type.vt = VT_I4;
            type.lVal = UIA_TabItemControlTypeId;
            winrt::com_ptr<IUIAutomationCondition> condition;
            winrt::check_hresult(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, condition.put()));
            winrt::com_ptr<IUIAutomationElementArray> found;
            ++g_probes;
            const HRESULT hr = root->FindAll(TreeScope_Subtree, condition.get(), found.put());
            --g_probes;
            return SUCCEEDED(hr);
        }
        catch (...)
        {
            NPX_LOG(L"Ricerca UI Automation in un'isola: errore 0x%08X.", static_cast<unsigned>(winrt::to_hresult()));
            return false;
        }
    }
}
