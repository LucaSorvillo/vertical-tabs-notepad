#pragma once

#include "CaptionButtons.h"
#include "Settings.h"

namespace npx
{
    // Riserva una colonna a sinistra nella finestra nativa del Blocco note per il pannello delle
    // schede verticali (sempre a sinistra: l'opzione "Pannello a destra" è stata tolta).
    //
    // Le parti del Blocco note (editor, barre di scorrimento, eventuali altre isole) sono finestre
    // figlie posizionate dal suo codice nativo. Per ognuna ricordiamo il rettangolo "naturale" che
    // le ha dato il Blocco note e lo trasliamo fuori dalla colonna con regole idempotenti: se il
    // Blocco note la sposta di nuovo, quello diventa il nuovo rettangolo naturale.
    // Il Blocco note crea una finestra editor (NotepadTextBox, con il proprio thread) per ogni
    // documento aperto e mostra solo quella della scheda attiva. Le isole sospese sopra l'editor (la barra di ricerca di
    // Ctrl+F) si spostano con lui senza cambiare misura: il loro contenuto è impaginato per quella.
    //
    // La striscia schede nativa (invisibile) viene spostata fuori dall'area visibile e la sua zona
    // nella barra del titolo risponde come barra del titolo (trascinabile).
    //
    // La colonna va dalla cima della finestra al fondo dell'editor, come le schede verticali di Chrome: anche le
    // figlie sopra l'editor (la barra dei menu) cominciano dal suo bordo destro. In cima alla colonna c'è la fascia
    // del titolo del pannello (comprimi/espandi): dove non ci sono pulsanti l'isola del pannello non c'è
    // (la sua regione la esclude, vedi VerticalTabs) e la finestra principale risponde come sulla sua barra del
    // titolo (trascinamento, doppio clic, menu di sistema).
    // Con la colonna in cima la riga del titolo, rimasta vuota, si nasconde (come Edge con "nascondi la barra del
    // titolo"): la barra dei menu sale lì, dal bordo della colonna fino ai pulsanti della finestra (più un tratto
    // libero da cui trascinarla), e l'editor con la sua barra di scorrimento verticale si allunga verso l'alto (anche
    // la superficie del testo dentro, FitInnerWindows). Restano 3 px in cima: il bordo superiore, che ridimensiona. La
    // fascia del titolo della colonna è alta quanto la barra dei menu, e i pulsanti della finestra sono i nostri
    // (CaptionButtons), alti dalla cima della finestra al fondo della barra dei menu: quelli di DWM sono più bassi.
    //
    // I cambiamenti si intercettano con il subclass della finestra principale (WM_SIZE, WM_NCHITTEST,
    // WM_NCDESTROY), un hook WinEvent su spostamenti/visibilità delle finestre del processo e un
    // controllo periodico di sicurezza. Va creato e usato sul thread UI della finestra.
    class WindowLayout
    {
    public:
        WindowLayout() = default;
        ~WindowLayout();
        WindowLayout(WindowLayout const&) = delete;
        WindowLayout& operator=(WindowLayout const&) = delete;

        // Aggancia la finestra principale (subclass "passivo": serve anche ad accorgersi della chiusura).
        void Attach(HWND main);
        // nativeTabs = isola XAML della barra schede originale; panel = bridge della nostra isola.
        void SetManagedWindows(HWND nativeTabs, HWND panel);
        void Detach();

        // Attiva/aggiorna o disattiva il layout verticale (con ripristino del layout originale).
        void SetActive(bool active, Settings const& settings);

        // Ricontrolla e, se serve, riapplica il layout. Idempotente ed economico.
        void Check();

        // Porta temporaneamente la striscia schede nativa (invisibile) con l'angolo in alto a sinistra in
        // un punto (coordinate client della finestra principale), ad esempio per aprire lì il suo menu
        // contestuale; std::nullopt la riporta fuori dall'area visibile.
        void PinNativeTabs(std::optional<POINT> clientTopLeft);

        bool IsActive() const { return m_active; }
        HWND Main() const { return m_main; }

        // Finestra a cui restituire il focus della tastiera (la superficie dell'editor visibile).
        HWND EditorFocusTarget() const;

        // Zone vuote della barra dei menu (px, coordinate della sua isola), ad esempio tra "Visualizza" e "Novità".
        // Con la riga del titolo nascosta la barra dei menu sta nella riga del titolo, ma la sua isola XAML si prende il
        // mouse (che non passa da WM_NCHITTEST): lì la sua regione ha un buco e la finestra principale risponde come
        // barra del titolo (trascinamento, doppio clic, menu di sistema). Le calcola VerticalTabs dall'albero XAML;
        // islandSize è la misura dell'isola per cui valgono (se l'isola nel frattempo è cambiata, non si applicano).
        void SetMenuBarHoles(std::vector<RECT> holes, SIZE islandSize);

        // I nostri pulsanti della finestra (finestra null se ci sono quelli di DWM).
        CaptionButtons& Buttons() { return m_buttons; }

        // Mostra/nasconde il pannello: lo fa l'isola XAML (non basta ShowWindow sull'HWND).
        std::function<void(bool)> onPanelVisibility;
        // Geometria del pannello in cima alla finestra, in px dal suo bordo superiore: altezza della fascia del
        // titolo (i suoi pulsanti) e inizio dell'editor (da lì in giù il separatore e il divisore); 0, 0 con la
        // colonna accanto all'editor. Chiamato quando cambia.
        std::function<void(int captionPx, int contentTopPx)> onPanelGeometry;
        // Come cambiano, rispetto al layout del Blocco note, le isole delle barre di scorrimento dell'editor: la verticale
        // si allunga verso l'alto di raisePx (editor salito nella riga del titolo nascosta), l'orizzontale (dal bordo
        // sinistro dell'editor) si accorcia di shrinkPx (la colonna a sinistra). 0, 0 = come le vuole il Blocco note.
        // Chiamato a ogni layout (anche se non cambia: il Blocco note può aver ricalcolato le sue misure), sul thread
        // della finestra.
        std::function<void(int raisePx, int shrinkPx)> onScrollBarsChange;
        // La finestra principale sta per essere distrutta (chiamato sul suo thread, una volta).
        std::function<void()> onMainDestroyed;

    private:
        struct Tracked
        {
            RECT natural{};
            RECT applied{};
        };

        static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD idThread,
                                          DWORD time);
        static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

        void InstallHooks();
        void RemoveHooks();
        void RestoreChildren();
        void RequestNativeRelayout();
        void SetPanelShown(bool shown);
        HWND FindEditor() const;
        RECT NaturalRect(HWND hwnd, RECT const& current);
        void Move(HWND hwnd, RECT const& rect);
        // Restringe le finestre interne (la superficie di testo dentro NotepadTextBox) come il loro contenitore.
        void FitInnerWindows(HWND container, RECT const& containerNatural, RECT const& containerWanted);
        bool IsInHiddenTabsArea(LPARAM screenPoint) const;
        bool IsInPanelCaption(LPARAM screenPoint) const;
        bool IsInMenuBarHole(LPARAM screenPoint) const;
        void ApplyMenuBarRegion();
        void HideNativeTabs(RECT const& natural);
        void SetGeometry(int captionPx, int contentTopPx);
        // Invalida la parte dell'editor coperta da una pittura della finestra principale (regione in coordinate client).
        void RepaintEditorUnder(HRGN painted) const;

        HWND m_main = nullptr;
        HWND m_nativeTabs = nullptr;
        HWND m_panel = nullptr;
        Settings m_settings;
        bool m_active = false;
        bool m_panelShown = false;
        bool m_panelLowered = false; // pannello in fondo all'ordine Z: sotto un'isola sospesa che sporge sulla colonna
        bool m_inCheck = false;
        bool m_subclassed = false;
        ULONGLONG m_noEditorSince = 0; // da quando non c'è un editor visibile (0 = c'è)
        HWINEVENTHOOK m_hookShowHide = nullptr;
        HWINEVENTHOOK m_hookLocation = nullptr;
        std::unordered_map<HWND, Tracked> m_tracked;
        RECT m_nativeTabsNatural{}; // dove il Blocco note mette la striscia schede (nella barra del titolo)
        std::optional<POINT> m_nativeTabsPin; // posizione temporanea (menu contestuale aperto)
        RECT m_panelCaption{};      // fascia del titolo in cima alla colonna (coordinate client; vuota se la colonna non è in cima)
        int m_glassFallbackTop = 0; // inizio naturale dell'editor: dove finisce la zona vetro del Blocco note (Glass.h)
        HWND m_menuBarHwnd = nullptr;     // isola della barra dei menu salita nella riga del titolo (nullptr se non lo è)
        std::vector<RECT> m_menuBarHoles; // zone vuote richieste (coordinate dell'isola)
        SIZE m_menuBarHolesSize{};        // misura dell'isola per cui sono state calcolate
        HWND m_regionHwnd = nullptr;      // isola a cui è applicata la regione con i buchi
        std::vector<RECT> m_regionHoles;  // buchi applicati
        std::pair<int, int> m_geometry{ -1, -1 }; // ultima geometria comunicata (onPanelGeometry)
        CaptionButtons m_buttons;
    };
}
