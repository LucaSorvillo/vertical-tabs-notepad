#pragma once

namespace npx::TreeDump
{
    // Esporta in %LOCALAPPDATA%\VTabsNotepad\logs\tree-<pid>-<n>.txt l'albero visuale della
    // finestra che contiene 'element' (tipi, nomi, dimensioni, layout, testi).
    // Va chiamata sul thread UI dell'elemento. Restituisce il percorso del file.
    std::wstring DumpWindow(winrt::Microsoft::UI::Xaml::UIElement const& element, std::wstring_view reason);
}
