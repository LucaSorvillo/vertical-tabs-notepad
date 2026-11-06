#pragma once
// Impostazioni di VTabsNotepad, salvate in %LOCALAPPDATA%\VTabsNotepad\settings.ini.
// Il launcher le modifica dal menu della tray; la mod dentro Notepad osserva il file
// e riapplica le modifiche al volo.

namespace npx
{
    struct Settings
    {
        // [VerticalTabs] (il pannello sta sempre a sinistra; le opzioni "Pannello a destra" e "Ricerca nelle schede"
        // sono state tolte)
        bool verticalTabs = true;                   // Schede verticali attive
        int paneWidth = 240;                        // Larghezza del pannello espanso (DIP)
        bool collapsed = false;                     // Pannello compresso: solo le icone delle schede

        // [BackToTop]
        bool backToTop = true;                      // Pulsante "Torna su" nell'angolo dell'editor

        // [ReopenTab]
        bool reopenTab = true;                      // Ctrl+Maiusc+T riapre la scheda chiusa

        // Le barre di scorrimento subito larghe sotto il mouse (ScrollBarHover) sono sempre attive: l'opzione
        // [ScrollBars] FastHover è stata tolta su richiesta dell'utente.

        // [SmoothScroll]
        bool noSmoothScroll = true;                 // Rotella sul testo senza lo scorrimento fluido dell'editor (WheelScroll)

        // [Debug]
        bool dumpTreeOnAttach = false;              // Esporta l'albero XAML appena la mod si aggancia
        bool verboseLog = false;

        static constexpr int kMinPaneWidth = 140;
        static constexpr int kMaxPaneWidth = 600;
        static constexpr int kCollapsedPaneWidth = 56; // larghezza fissa del pannello compresso (DIP)

        static Settings Load();
        bool Save() const;

        bool operator==(Settings const&) const = default;
    };
}
