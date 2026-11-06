#pragma once

namespace npx
{
    // Barre di scorrimento dell'editor del Blocco note dentro il layout della mod. WindowLayout cambia le isole che le
    // ospitano: la verticale si allunga verso l'alto di "raise" px (editor salito nella riga del titolo nascosta),
    // l'orizzontale si accorcia della larghezza della colonna (parte dal bordo sinistro dell'editor, spostato). Ma il
    // Blocco note dimensiona ciascuna barra sul SUO layout, cioè sull'isola non modificata, con due valori locali in DIP
    // interi sull'asse della barra (put_Height / put_Width, a ogni suo ricalcolo del layout):
    //   - lo ScrollViewer (la vista): la verticale non arrivava in fondo (mancavano raise px), l'orizzontale sforava a
    //     destra oltre l'isola della larghezza della colonna (freccia destra e fine della corsa tagliate fuori);
    //   - il suo contenuto, un Canvas "ScrollContent" alto/largo vista + corsa del testo: il Blocco note traduce la
    //     posizione della barra in quella del testo 1:1 su (contenuto - vista). Cambiando solo la vista, trascinando il
    //     cursore in fondo il testo si fermava prima della fine (verticale) e a testo tutto a destra il cursore restava
    //     al 93% (orizzontale).
    //
    // Qui si scrivono vista e contenuto del Blocco note + la stessa differenza dell'isola (la corsa resta la sua), e li si
    // riscrive ogni volta che il Blocco note li reimposta: la callback sulla proprietà è sincrona, dentro la sua put_,
    // quindi nessun fotogramma con la barra sbagliata. Un'animazione HoldEnd sopra il valore locale non bastava: in WinUI 3
    // un nuovo valore locale vince sull'animazione conclusa. I valori scritti da noi non sono mai interi (quelli del Blocco
    // note sì): ogni sua put_ cambia il valore effettivo e si riconosce sempre, anche quando coincide con il nostro.
    // Con differenza 0, o spegnendo la funzione, tornano i valori del Blocco note.
    //
    // Un oggetto per barra (asse). Vive e viene usato interamente sul thread UI della finestra.
    class ScrollBarFit
    {
    public:
        enum class Axis
        {
            Vertical,   // altezze, nell'isola stretta e alta
            Horizontal, // larghezze, nell'isola larga e bassa
        };

        ScrollBarFit(HWND main, Axis axis) : m_main(main), m_axis(axis)
        {
        }
        ScrollBarFit(ScrollBarFit const&) = delete;
        ScrollBarFit& operator=(ScrollBarFit const&) = delete;

        // ScrollViewer chiamati "ScrollViewer" vivi sul thread della finestra (tra questi ci sono le barre dell'editor).
        std::function<std::vector<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>()> scrollViewersProvider;

        // Dal layout: di quanti px l'isola è più grande (+) o più piccola (-) di come la vuole il Blocco note, sul suo asse
        // (0 = niente; tornano i valori del Blocco note).
        void SetDelta(int deltaPx);
        // Controllo periodico (500 ms): barra ricreata o non ancora trovata, modifiche sfuggite alle callback.
        void Refresh();
        // Rimette i valori del Blocco note e si stacca dalla barra (funzione spenta).
        void Reset();
        // La finestra si chiude: si dimentica tutto senza toccare XAML.
        void Abandon();

    private:
        // Un elemento di cui si corregge la misura sull'asse: la vista (lo ScrollViewer) o il suo contenuto.
        struct Slot
        {
            winrt::weak_ref<winrt::Microsoft::UI::Xaml::FrameworkElement> element;
            int64_t token = 0;                                     // RegisterPropertyChangedCallback sulla misura
            double base = std::numeric_limits<double>::quiet_NaN(); // ultimo valore del Blocco note (DIP)
            double ours = std::numeric_limits<double>::quiet_NaN(); // valore scritto da noi (DIP); NaN = vale il suo
        };

        winrt::Microsoft::UI::Xaml::DependencyProperty SizeProperty() const;
        const wchar_t* AxisName() const;
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer Find() const;
        void Apply();
        void Attach(Slot& slot, winrt::Microsoft::UI::Xaml::FrameworkElement const& element);
        void Detach(Slot& slot, bool restore);
        // Rilegge i valori locali (del Blocco note o nostri) e scrive quelli voluti, vista e contenuto insieme.
        void Sync();
        void ReadBase(Slot& slot, winrt::Microsoft::UI::Xaml::FrameworkElement const& element) const;
        void Restore(Slot& slot, winrt::Microsoft::UI::Xaml::FrameworkElement const& element);
        void Write(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, double value);
        double Local(winrt::Microsoft::UI::Xaml::FrameworkElement const& element) const;
        void OnSizeChanged();

        HWND m_main = nullptr;
        Axis m_axis = Axis::Vertical;
        int m_delta = 0;
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> m_scrollViewer;
        Slot m_view;    // lo ScrollViewer
        Slot m_content; // il suo contenuto (Canvas "ScrollContent")
        std::shared_ptr<int> m_alive = std::make_shared<int>(0); // le callback non toccano un oggetto distrutto
        bool m_writing = false;
        // Valvola di sicurezza: riscritture a raffica (non succede: il Blocco note non reagisce alle misure) = si lasciano le sue.
        ULONGLONG m_burstStart = 0;
        int m_burstCount = 0;
        ULONGLONG m_pausedUntil = 0;
        bool m_unknownLogged = false;
        bool m_appliedLogged = false;
    };
}
