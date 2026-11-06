#pragma once

#include <windows.h>

namespace npx
{
    // Icona della tray: un blocco note a contorno con tre anelli in alto e le righe (richiama il Blocco note
    // senza essere la sua icona), monocromo come le icone di sistema: bianco sulla barra delle applicazioni
    // scura, nero su quella chiara. Disegnato con GDI+ alla misura chiesta; null se GDI+ non parte.
    HICON CreateNotepadOutlineIcon(int size, bool lightTaskbar);

    // Crea l'icona della tray disegnando un glifo di "Segoe Fluent Icons" (o "Segoe MDL2 Assets"),
    // bianco o nero a seconda del tema della barra delle applicazioni (ripiego di CreateNotepadOutlineIcon).
    HICON CreateGlyphIcon(wchar_t glyph, int size, bool lightTaskbar);

    // true se la barra delle applicazioni usa il tema chiaro.
    bool IsTaskbarLight();
}
