#pragma once

#include "Settings.h"
#include "TabColors.h"

namespace npx
{
    // Feature "riapri la scheda chiusa" (Ctrl+Maiusc+T) per una finestra del Blocco note, come nel
    // browser: la scheda chiusa per sbaglio torna al suo posto nell'elenco, e premendo di nuovo si va a
    // ritroso.
    //
    // Di ogni scheda chiusa si ricordano soltanto il percorso del file, il posto e il colore che aveva, nel file
    // closed-tabs.txt della cartella dati (le ultime 25, una riga ciascuna), come le schede chiuse di
    // Chrome: valgono per tutte le finestre e anche dopo aver chiuso e riaperto il Blocco note. Nessun
    // contenuto dei documenti: una scheda mai salvata non si può riaprire, e al comando non succede
    // nulla — a quella ci pensa la richiesta di salvataggio che il Blocco note fa già quando la si
    // chiude. Spegnendo la funzione dalla tray il file si cancella. Il file si rilegge a ogni chiusura
    // e a ogni comando (sotto un mutex con nome: più finestre, o istanze del Blocco note): nessuna copia
    // in memoria da tenere allineata.
    //
    // Chiudendo una finestra il Blocco note toglie le sue schede da TabItems appena prima di
    // distruggerla (~150 ms): non diventano "chiuse", perché la conferma chiede mezzo secondo e la
    // finestra se ne va prima (e l'ultima finestra le ritrova comunque nella sessione ripristinata).
    //
    // La combinazione si chiede a Windows (RegisterHotKey) solo mentre questa finestra è attiva
    // (WM_ACTIVATE nel nostro subclass, più un controllo di sicurezza nel timer) e si restituisce
    // appena perde il fuoco: resta libera per gli altri programmi che la usano (nei browser, per
    // esempio) e della tastiera non si legge nient'altro, perché Windows ci consegna solo questa.
    // La registrazione è legata al thread, non alla finestra: va restituita anche quando la finestra
    // viene distrutta mentre la teneva (vedi WM_NCDESTROY), altrimenti resta occupata per sempre.
    //
    // Il file si riapre lanciando il Blocco note con quel file, come un doppio clic: è la stessa
    // istanza, che lo mette come scheda nella finestra attiva (in fondo); appena la scheda compare la si
    // rimette al suo posto con TabsBar.ReorderTab, lo stesso metodo del riordino col mouse.
    //
    // Le chiusure si riconoscono dalle schede che spariscono da TabItems: un riordino le fa sparire e
    // ricomparire, quindi una chiusura si considera vera solo se la scheda non torna entro mezzo
    // secondo. Sul thread UI non si guarda mai il disco (una cartella di rete non raggiungibile
    // bloccherebbe la finestra): basta che la descrizione della scheda sia un percorso assoluto, e il
    // file si controlla sul thread che lo riapre.
    //
    // Vive e viene usata interamente sul thread UI della finestra.
    class ReopenTab : public std::enable_shared_from_this<ReopenTab>
    {
    public:
        explicit ReopenTab(winrt::NotepadXamlUI::TabsBar const& tabsBar);
        ~ReopenTab();
        ReopenTab(ReopenTab const&) = delete;
        ReopenTab& operator=(ReopenTab const&) = delete;

        void Apply(Settings const& settings);
        // Timer della finestra (500 ms): conferma le chiusure e tiene allineata la scorciatoia.
        void OnTimer();
        // La finestra si chiude (o la mod rinuncia): toglie tutto.
        void Detach();
        // Comando della tray (--reopen-tab): agisce solo sulla finestra attiva, come la scorciatoia.
        void ReopenIfActive();
        // Colore da rimettere alla scheda riaperta (impostato da ModCore: passa dalla colonna, se c'è, che aggiorna la riga).
        std::function<void(winrt::guid const&, tabcolors::TabColor)> restoreColor;

    private:
        struct Known   // scheda aperta adesso
        {
            winrt::guid tabId{};
            std::wstring path; // descrizione della scheda (per i file salvati è il percorso)
        };
        struct Gone    // scheda sparita da TabItems: chiusa, o spostata da un riordino
        {
            winrt::guid tabId{};
            std::wstring path;
            size_t index = 0;
            ULONGLONG when = 0;
            tabcolors::TabColor color = tabcolors::TabColor::None; // colore che aveva (torna con lei)
        };
        struct Reopening // riapertura chiesta: si attende che la scheda compaia per rimetterla al suo posto
        {
            std::wstring path;
            size_t index = 0;
            ULONGLONG started = 0;
            tabcolors::TabColor color = tabcolors::TabColor::None;
        };

        static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

        bool Attach();
        void Enable();
        // forgetHistory: funzione spenta dalla tray (si cancella anche closed-tabs.txt); false quando la
        // finestra si chiude o è esclusa dalla modalità sviluppo (la cronologia resta per le altre e per dopo).
        void Disable(bool forgetHistory);
        void ClearState();
        // Rilegge le schede: nuove, sparite (candidate chiusure) e tornate (riordino).
        void Refresh();
        // Promuove a "chiuse" le schede sparite e non tornate. Con immediately (al comando) non si
        // aspetta la mezza conferma: se la scheda non è più tra quelle vive, è chiusa.
        void ConfirmClosures(bool immediately);
        void Reopen();
        // La scheda riaperta e spostata al suo posto torna quella attiva: il Blocco note, dopo averla aggiunta in fondo,
        // seleziona di nuovo l'ultima posizione (un'altra scheda); la si riseleziona dopo di lui.
        void SelectReopened(winrt::guid const& tabId);
        void SyncHotkey();
        void Register();
        void Unregister();

        winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> m_tabsBar;
        HWND m_main = nullptr;
        bool m_attached = false;
        bool m_enabled = false;
        bool m_subclassed = false;
        bool m_registered = false;
        bool m_failedLogged = false;

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::NotepadXamlUI::TabItemViewModel> m_items{ nullptr };
        winrt::event_token m_itemsToken{};
        std::vector<Known> m_known;
        std::vector<Gone> m_gone;
        std::vector<Reopening> m_reopening; // riaperture chieste e non ancora comparse (più d'una se si preme in fretta)
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_selectTimer{ nullptr };
        winrt::guid m_selectTabId{};
    };
}
