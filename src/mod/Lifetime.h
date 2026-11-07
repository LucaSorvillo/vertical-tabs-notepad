#pragma once

// Vita della DLL nel processo del Blocco note. Con "Esci" della tray la mod si toglie del tutto e la DLL esce
// (ModCore::TryUnload): prima però nessun thread deve trovarsi dentro il nostro codice.
namespace npx::lifetime
{
    // Chiamate in corso nel nostro codice arrivate da Windows o dal Blocco note: subclass, agganci (hook, vtable,
    // importazioni), procedure di finestra, callback WinEvent. Un ciclo modale del Blocco note (per esempio lo
    // spostamento della finestra) può tenerne una sullo stack a lungo: la DLL aspetta che torni a zero.
    std::atomic<int>& Calls();

    struct CallScope
    {
        CallScope() noexcept
        {
            ++Calls();
        }
        ~CallScope()
        {
            --Calls();
        }
        CallScope(CallScope const&) = delete;
        CallScope& operator=(CallScope const&) = delete;
    };

    // La mod si sta togliendo dal processo: niente lavoro nuovo (finestre, agganci, thread).
    bool Unloading();
}
