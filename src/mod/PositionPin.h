#pragma once

namespace npx::pin
{
    // Tiene ferme nel layout della mod le finestre che la mod sposta nella fascia dell'editor (il contenitore NotepadTextBox
    // con la superficie di testo RichEditD2DPT dentro, le isole delle barre di scorrimento, la barra dei menu salita nella
    // riga del titolo) quando il Blocco note riafferma il loro posto naturale.
    //
    // Il Blocco note ricalcola il suo layout a ogni attivazione della finestra (anche senza cambiarne la misura) e
    // rimette l'editor dove lo vuole lui: più in basso e senza la colonna. WindowLayout lo riportava subito al suo posto,
    // ma la superficie di testo riceveva comunque due WM_SIZE nel mezzo del clic che aveva attivato la finestra (quel
    // clic arriva all'editor mentre il Blocco note gestisce l'attivazione) e, ridimensionata con il focus, riportava in
    // vista il cursore lampeggiante prima di posare il clic: il testo saltava e il clic finiva su un'altra riga.
    //
    // Per ogni finestra fissata si ricorda il rettangolo naturale (quello del Blocco note) e quello voluto dalla mod, nelle
    // coordinate client del genitore. Un subclass sul thread della finestra (gli editor hanno ciascuno il proprio thread:
    // lo si installa da lì con un hook WH_GETMESSAGE e un messaggio registrato; le isole stanno sul thread della finestra
    // principale e lo ricevono subito) guarda WM_WINDOWPOSCHANGING: se il Blocco
    // note chiede di nuovo proprio il rettangolo naturale e la finestra è già dove la vuole la mod, la richiesta diventa
    // "nessuno spostamento" (SWP_NOMOVE | SWP_NOSIZE). Un rettangolo naturale nuovo (finestra ridimensionata, layout del
    // Blocco note cambiato) passa come prima e WindowLayout lo ricalcola. Con Clear la finestra torna libera (il subclass
    // resta, inerte, fino alla sua distruzione).
    //
    // Set e Clear si chiamano dal thread UI della finestra principale; il subclass gira sui thread degli editor.
    void Set(HWND hwnd, RECT const& natural, RECT const& wanted);
    void Clear(HWND hwnd);

    // Scaricamento della mod ("Esci" della tray): toglie tutti i subclass (ognuno dal thread della sua finestra, con un
    // messaggio mandato) e gli hook delle richieste in sospeso; da lì in poi Set non installa più niente. Da un thread che
    // non sia quello di una finestra fissata. false se una finestra non ha risposto (si riprova: le altre sono già libere).
    bool Uninstall();
    // Scaricamento annullato (la tray è ripartita prima che la DLL uscisse): Set torna a installare.
    void Resume();
}
