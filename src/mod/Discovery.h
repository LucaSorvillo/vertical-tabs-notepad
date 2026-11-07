#pragma once
// Come la mod trova le parti XAML di ogni finestra del Blocco note, SENZA la diagnostica XAML.
//
// La diagnostica XAML (InitializeXamlDiagnosticsEx con questa DLL come "TAP") faceva caricare la DLL al runtime XAML, che
// poi teneva per sempre il suo oggetto: la mod non poteva più uscire dal processo (con "Esci" della tray deve sparire
// tutto). Al suo posto, agganci a slot di interfacce WinRT pubbliche (ABI stabile), installati una volta per processo
// sostituendo una voce della tabella dei metodi (vtable) condivisa da tutte le istanze della classe, e tolti allo
// scaricamento:
//   - IFrameworkElementOverrides::OnApplyTemplate di TabView e di MenuBar (controlli di WinUI composti, chiamati da XAML
//     attraverso la vtable): ogni barra delle schede e ogni barra dei menu nuova si presenta da sé, sul suo thread;
//   - IFrameworkElementOverrides::MeasureOverride di NotepadXamlUI.ScrollBar (UserControl del Blocco note: XAML ne
//     chiama il layout attraverso la vtable perché è un tipo "custom"): le barre di scorrimento dell'editor;
//   - per le finestre già aperte quando arriva la mod, una ricerca UI Automation in ogni isola XAML (Probe) fa chiamare
//     sul thread della finestra IAutomationPeerOverrides::GetAutomationControlTypeCore dei "peer" di TabViewItem e
//     MenuBarItem e IUIElementOverrides::OnCreateAutomationPeer di NotepadXamlUI.ScrollBar (che non ha un peer suo:
//     XAML lo richiede a ogni visita). Fuori da una ricerca nostra questi agganci non fanno niente.
// Tutti chiamano prima il metodo originale e non trattengono nulla. Microsoft.UI.Xaml.dll e NotepadXamlUI.dll usano CFG
// (non XFG): un puntatore a una nostra funzione nella vtable è un bersaglio valido.

#include <windows.h>

namespace npx::discovery
{
    enum class Kind
    {
        TabView,   // muxc::TabView (dentro NotepadXamlUI.TabsBar)
        MenuBar,   // muxc::MenuBar (dentro NotepadXamlUI.MainMenuBar)
        ScrollBar, // NotepadXamlUI.ScrollBar (barra di scorrimento dell'editor, nella sua isola)
    };

    // Chiamata sul thread della finestra, dentro il layout XAML o dentro una richiesta UI Automation: deve solo prendere
    // nota e rimandare il lavoro (DispatcherQueue). Mai eccezioni. Per ScrollBar arriva a ogni misura dell'elemento.
    using Callback = void (*)(Kind kind, winrt::Microsoft::UI::Xaml::FrameworkElement const& element) noexcept;

    // Installa gli agganci (una volta per processo; le chiamate successive ritornano subito) sul thread UI di window, una
    // finestra del Blocco note di questo processo. Da un thread che non sia quello UI. true se installati (quelli
    // essenziali: barra delle schede).
    bool Install(HWND window, Callback callback);
    bool Installed();
    // Agganci essenziali impossibili in questo processo (XFG attivo nel modulo di WinUI): la mod non fa nulla.
    bool Blocked();

    // Ricerca UI Automation nell'isola XAML island (una figlia DesktopChildSiteBridge di una finestra del Blocco note): fa
    // presentare le parti che contiene. Da un thread MTA che non sia quello della finestra. false se la ricerca non è
    // riuscita.
    bool Probe(HWND island);

    // Scaricamento della mod ("Esci" della tray): rimette nelle vtable le funzioni originali. false se una voce non è più
    // la nostra (un altro aggancio sopra il nostro chiamerebbe ancora il nostro codice): allora la DLL non si scarica.
    // Dopo, Install può ripartire da capo. Le chiamate già dentro i nostri agganci le conta lifetime::Calls().
    bool Uninstall();
}
