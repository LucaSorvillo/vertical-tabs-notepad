#pragma once

// Larghezza del testo dell'editor come la conta il Blocco note, nella vista formattata del Markdown (le tabelle).
//
// Il Blocco note impagina le tabelle in RTF con celle di larghezza fissa (\cellx): la larghezza disponibile la calcola da
// GetClientRect della finestra PRINCIPALE, meno la barra di scorrimento verticale (GetSystemMetricsForDpi(SM_CXVSCROLL))
// e 16 DIP (Notepad.exe: GetDpiForWindow, GetSystemMetricsForDpi e MulDiv(16, dpi, 96) subito dopo GetClientRect). Con la
// colonna delle schede l'editor è più stretto della finestra della larghezza della colonna, e le tabelle ne uscivano a
// destra di altrettanto. La ricalcola quando dispone l'editor (MoveWindow) e quando cambia lo zoom.
//
// Un aggancio alla GetClientRect importata da Notepad.exe (come glass:: per DwmExtendFrameIntoClientArea) toglie alla
// larghezza la parte coperta dalla colonna, solo per le finestre principali con la colonna (SetReduction) e solo se
// chiamato da quel calcolo: lo si riconosce dal codice dopo la chiamata (le tre chiamate attraverso le importazioni e il 2
// di SM_CXVSCROLL), non da indirizzi fissi. Se un aggiornamento del Blocco note cambia quel codice, l'aggancio non tocca
// nulla. Le chiamate dentro l'aggancio le conta lifetime::Calls().
#include <windows.h>

namespace npx::editorwidth
{
    // Installa l'aggancio (se c'è già non fa niente). false se non è possibile.
    bool Install();
    // Scaricamento della mod: rimette l'importazione di prima (false se la voce non è più la nostra).
    bool Uninstall();

    // Pixel che la colonna toglie alla larghezza dell'editor della finestra principale main (0: nessuno, la finestra torna
    // come senza mod). Il Blocco note la usa al suo prossimo calcolo (vedi WindowLayout::SetTextWidthReduction).
    void SetReduction(HWND main, int px);
}
