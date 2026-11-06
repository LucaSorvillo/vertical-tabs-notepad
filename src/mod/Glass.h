#pragma once

namespace npx::glass
{
    // Zona "vetro" della finestra del Blocco note: la cornice di sistema estesa nell'area client
    // (DwmExtendFrameIntoClientArea), dove dietro la riga del titolo e la barra dei menu si vede il materiale Mica Alt. Il
    // Blocco note la estende fino al fondo della barra dei menu. Lì DWM compone i pixel con il loro alfa: l'editor, che
    // disegna senza alfa, si somma allo sfondo e diventa più chiaro (#272727 + #202020 = #474747). Con la riga del titolo
    // nascosta l'editor sale dentro quella zona, quindi la si accorcia fino al suo bordo.
    //
    // La zona la sceglie il Blocco note: Notepad.exe importa la funzione direttamente e la richiama, ad esempio quando la
    // finestra viene ingrandita o ripristinata. Un aggancio nella sua tabella delle importazioni ricorda ogni richiesta e,
    // se per quella finestra c'è un limite, ne accorcia il margine superiore.

    // Installa l'aggancio (una volta per processo: la DLL non viene mai scaricata). false se non è possibile.
    bool Install();
    bool Hooked();

    // Limite del margine superiore della zona vetro (px client) per una finestra, o nessun limite (torna quella del
    // Blocco note). fallbackTopPx è il margine del Blocco note da usare se la sua richiesta non è nota (finestra creata
    // prima dell'aggancio). Applica subito la zona risultante, solo se il limite cambia.
    void SetTopLimit(HWND hwnd, std::optional<int> limitPx, int fallbackTopPx);

    // Riapplica la zona attuale (senza aggancio il Blocco note la reimposta da solo, ad esempio ingrandendo la finestra).
    void Reapply(HWND hwnd);

    // La finestra non è più gestita (si sta chiudendo).
    void Forget(HWND hwnd);
}
