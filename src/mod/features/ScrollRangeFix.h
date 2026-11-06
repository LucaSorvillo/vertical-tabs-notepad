#pragma once

#include "Settings.h"

namespace npx
{
    // Correzione di un difetto del Blocco note (della RichEdit di Office che fa da editor), presente anche senza la mod: quando
    // un documento con righe più larghe della vista ("a capo automatico" spento, serve la barra orizzontale) passa dal "non ci
    // sta in altezza" al "ci sta" (finestra più alta, o la colonna in cima della mod che alza l'editor di ~49 px), la RichEdit
    // mette come massimo della barra VERTICALE quello ORIZZONTALE (la larghezza del testo) e smette di aggiornarne la pagina.
    // La barra XAML del Blocco note mostra allora una corsa che non esiste: la rotella non fa nulla (l'editor sa che il testo ci
    // sta) e trascinando la barra il testo non si muove. Visto su un documento di 49 righe (906 px di testo: nella vista
    // originale da 889 px non ci stava, in quella della mod da 938 sì) e riprodotto nel Blocco note senza mod.
    //
    // Ogni 500 ms, per l'editor visibile di questa finestra: due GetScrollInfo (nessun messaggio). Firma del difetto: massimo
    // verticale uguale a quello orizzontale, con corsa verticale > 0 e barra orizzontale necessaria. Solo allora si conferma che
    // il testo ci sta davvero in altezza (posizione dell'ultima riga e altezza di riga chieste all'editor, sul suo thread, con
    // SendMessageTimeout: niente blocchi) e si azzera la corsa verticale (SetScrollInfo): la barra XAML la segue e sparisce.
    //
    // Secondo difetto, anche questo del Blocco note senza mod: le sue barre XAML rileggono le corse dell'editor solo quando la
    // RichEdit lo avvisa che sta per ridisegnarsi (WM_COMMAND EN_UPDATE al genitore NotepadTextBox), cioè quando si scorre o si
    // scrive. Ma la RichEdit impagina i documenti in background e allunga la corsa dopo (300 righe aperte: massimo da 2108 a
    // 5438 px in ~0,4 s) senza ridisegnare nulla: la barra resta con il cursore della corsa vecchia (45% invece del 17,6%) e al
    // primo scatto di rotella cambia di colpo grandezza e posizione. Succede aprendo un file, con le schede ripristinate all'avvio,
    // e quando cambia la pagina dell'editor senza un ridisegno. Quindi, sempre ogni 500 ms: se massimo o pagina (verticale o
    // orizzontale) dell'editor visibile sono cambiati dal controllo precedente (o è cambiato l'editor), si manda al genitore lo
    // stesso EN_UPDATE (PostMessage): il Blocco note rilegge le corse e aggiorna le barre. Il testo non si muove, l'indicatore
    // della barra non compare (provato: uno scorrimento di 1 px avanti e indietro la faceva lampeggiare) e la scheda non risulta
    // modificata. Se il Blocco note sapeva già tutto, rifà gli stessi conti.
    //
    // I controlli girano ogni 100 ms su un timer proprio (due GetScrollInfo: niente): cambiando scheda e scorrendo subito, la
    // barra è già quella giusta. La conferma della corsa fasulla (messaggi all'editor) si ripete al massimo ogni 2 s per lo
    // stesso stato. Sempre attiva (corregge difetti), salvo il gate della modalità sviluppo. Sul thread UI della finestra.
    class ScrollRangeFix
    {
    public:
        explicit ScrollRangeFix(winrt::NotepadXamlUI::TabsBar const& tabsBar) : m_tabsBar(winrt::make_weak(tabsBar))
        {
        }
        ~ScrollRangeFix();
        ScrollRangeFix(ScrollRangeFix const&) = delete;
        ScrollRangeFix& operator=(ScrollRangeFix const&) = delete;

        void Apply(Settings const& settings);
        // Controllo periodico della finestra (500 ms): di riserva, se il timer proprio non c'è.
        void OnTimer();
        void Detach();

    private:
        // Massimi e pagine dell'editor visibile all'ultimo controllo.
        struct Ranges
        {
            HWND surface = nullptr;
            int verticalMax = 0;
            UINT verticalPage = 0;
            int horizontalMax = 0;
            UINT horizontalPage = 0;

            bool operator==(Ranges const&) const = default;
        };

        HWND MainWindow();
        void StartTimer();
        void StopTimer();
        void Check();
        // true se ha corretto la corsa verticale fasulla.
        bool FixPhantomRange(HWND surface, SCROLLINFO const& vertical, SCROLLINFO const& horizontal, Ranges const& now);
        // EN_UPDATE al genitore dell'editor se le sue corse sono cambiate dall'ultima volta.
        void RefreshBarsIfChanged(Ranges const& now);

        winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> m_tabsBar;
        HWND m_main = nullptr;
        bool m_enabled = false;
        bool m_loggedFirst = false;
        bool m_loggedRefresh = false;
        Ranges m_ranges;
        Ranges m_notPhantom;              // stato con la firma ma testo che non ci sta (o risposta incerta)...
        ULONGLONG m_notPhantomUntil = 0;  // ...da non richiedere all'editor fino a questo momento
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
        std::shared_ptr<int> m_alive = std::make_shared<int>(0); // il tick non tocca un oggetto distrutto
    };
}
