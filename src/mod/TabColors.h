#pragma once

namespace npx::tabcolors
{
    // Colori assegnabili alle schede (gli stessi dei gruppi di schede di Chrome).
    enum class TabColor : uint8_t
    {
        None,
        Grey,
        Blue,
        Red,
        Yellow,
        Green,
        Pink,
        Purple,
        Cyan,
        Orange,
    };

    struct Swatch
    {
        TabColor color;
        const wchar_t* key;         // nome nel file
        const wchar_t* displayName; // nome nel menu
        winrt::Windows::UI::Color fillLight;
        winrt::Windows::UI::Color fillDark;
        winrt::Windows::UI::Color textLight; // iniziale sopra il colore
        winrt::Windows::UI::Color textDark;
    };

    // Colori disponibili (senza "nessuno"), nell'ordine del menu.
    std::span<const Swatch> Palette();
    Swatch const* Find(TabColor color);

    // Colore di una scheda (TabId, lo stesso identificativo della sessione del Blocco note).
    // Salvati in %LOCALAPPDATA%\VTabsNotepad\tab-colors.ini e condivisi da tutte le finestre, ognuna sul
    // proprio thread: accesso protetto da un mutex.
    TabColor Get(winrt::guid const& tabId);
    void Set(winrt::guid const& tabId, TabColor color);
}
