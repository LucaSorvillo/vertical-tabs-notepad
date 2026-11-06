#pragma once

#include "ScrollBarFit.h"
#include "Settings.h"
#include "TabColors.h"
#include "WindowLayout.h"

namespace npx
{
    // Feature "schede verticali" per una finestra del Blocco note.
    //
    // Il pannello è un controllo nostro, costruito con componenti WinUI di sistema, ospitato in una
    // NOSTRA isola XAML (DesktopWindowXamlSource, lo stesso meccanismo che usa il Blocco note) e
    // legato ai dati nativi della TabsBar: elenco, selezione, titolo, stato "modificato".
    // L'albero XAML del Blocco note non viene riorganizzato: la barra schede orizzontale resta viva
    // (è il "motore" di apertura/chiusura/salvataggio) ed è solo resa invisibile.
    // Chiusura, "nuova scheda", menu contestuale e riordino passano dai controlli e dai metodi nativi,
    // quindi seguono esattamente la logica del Blocco note (incluso il prompt di salvataggio).
    //
    // Vive e viene usato interamente sul thread UI della finestra. Gli handler registrati su oggetti
    // nativi tengono solo un weak_ptr: anche se la finestra si chiude, non possono richiamare un
    // oggetto distrutto.
    class VerticalTabs : public std::enable_shared_from_this<VerticalTabs>
    {
    public:
        explicit VerticalTabs(winrt::NotepadXamlUI::TabsBar const& tabsBar);
        ~VerticalTabs();

        VerticalTabs(VerticalTabs const&) = delete;
        VerticalTabs& operator=(VerticalTabs const&) = delete;

        void Apply(Settings const& settings);
        void OnTimer();
        // Radice del pannello (null finché non è stato creato): usata per la diagnostica.
        winrt::Microsoft::UI::Xaml::UIElement PanelRoot() const { return m_root; }
        // restore=true: ripristina il Blocco note com'era; false: la finestra si sta chiudendo.
        void Detach(bool restore);

        // Chiamata (sul thread della finestra) quando la finestra del Blocco note viene distrutta.
        std::function<void()> onWindowClosed;
        // Barre dei menu del Blocco note vive sul thread della finestra (tra queste c'è la nostra).
        std::function<std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement>()> menuBarsProvider;
        // ScrollViewer chiamati "ScrollViewer" vivi sul thread della finestra (per la barra di scorrimento dell'editor).
        std::function<std::vector<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>()> scrollViewersProvider;

    private:
        struct Row
        {
            winrt::NotepadXamlUI::TabItemViewModel item{ nullptr };
            winrt::guid tabId{};
            winrt::Microsoft::UI::Xaml::Controls::ListViewItem element{ nullptr }; // contenitore di sé stesso
            winrt::Microsoft::UI::Xaml::Controls::Border tint{ nullptr }; // fondo della riga (colore della scheda) e bordino
            // Stanghetta della scheda attiva: nel colore della scheda, o grigio neutro (del tema) se non ne ha
            winrt::Microsoft::UI::Xaml::Controls::Border pillColor{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::Border pillNeutral{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::Grid grid{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::TextBlock title{ nullptr };
            winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::Button close{ nullptr };
            // Icona della scheda: iniziale del titolo, su fondo neutro o sul colore assegnato
            winrt::Microsoft::UI::Xaml::Controls::Grid avatar{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::Border avatarNeutral{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::TextBlock avatarNeutralText{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::Border avatarColor{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::TextBlock avatarColorText{ nullptr };
            winrt::Microsoft::UI::Xaml::Shapes::Ellipse badge{ nullptr }; // "modificata", da compresso
            winrt::event_token propertyToken{};
            bool hovered = false;
        };

        bool Attach(winrt::NotepadXamlUI::TabsBar const& tabsBar);
        bool EnsurePanel();
        void BuildPanel();
        void ApplyBackdrop();
        void Enable();
        void Disable();
        void ApplyAppearance();
        // Intestazione del pannello (comprimi/espandi) e linee del bordo secondo la geometria del layout:
        // con la colonna in cima alla finestra i pulsanti stanno nella fascia del titolo, accanto a quelli della
        // finestra, e separatore e divisore cominciano dove comincia l'editor.
        void ApplyHeaderLayout(bool compact);
        void OnPanelGeometry(int captionPx, int contentTopPx);
        // Regione dell'isola del pannello: senza la fascia del titolo, salvo i suoi pulsanti. Lì sotto c'è la
        // finestra principale, che risponde come la sua barra del titolo (vedi WindowLayout). Aggiornata dopo
        // ogni layout, solo se cambia.
        void UpdateCaptionRegion();
        double PanelScale() const;
        // L'elenco è alto quanto le sue righe, fino allo spazio lasciato dal "+" sottostante.
        void UpdateListHeight();
        // Lo scorrimento orizzontale (rotella inclinata, due dita sul touchpad) non deve muovere l'elenco.
        void BlockHorizontalWheel();
        // Impostazioni per il layout: larghezza effettiva (stretta se il pannello è compresso).
        Settings LayoutSettings() const;
        // Titoli come quelli delle schede orizzontali: carattere, dimensione, peso e colore di una scheda
        // normale e di quella attiva, letti dal testo delle schede native già realizzate (la striscia,
        // anche invisibile, resta in layout), più il colore che il loro modello dà al titolo al passaggio
        // del mouse. Riletti periodicamente (cambi di tema compresi); true se è cambiato qualcosa.
        bool ReadNativeTitleStyle();

        // Barra dei menu del Blocco note (sopra editor e pannello): con il pannello attivo diventa
        // trasparente sul materiale della finestra, cioè prende il colore della barra del titolo.
        // Lo sfondo originale non viene sovrascritto: lo copre un'animazione che lo tiene trasparente
        // e, fermandola, torna il valore del Blocco note (con i suoi riferimenti al tema).
        void UpdateMenuBarTint();
        void RemoveMenuBarTint();
        // Zone vuote della barra dei menu (trascinabili con la riga del titolo nascosta, vedi WindowLayout)
        void UpdateMenuBarHoles();
        winrt::Microsoft::UI::Xaml::FrameworkElement FindMenuBar() const;

        // Pannello compresso/espanso
        void SetCollapsed(bool collapsed);

        // Dati
        void RebuildRows();
        void InsertRow(uint32_t index);
        void StashRow(uint32_t index);
        void FlushStash();
        void ClearRows();
        void OnItemsChanged(winrt::Windows::Foundation::Collections::IVectorChangedEventArgs const& args);
        void ScheduleRefresh();
        std::unique_ptr<Row> CreateRow(winrt::NotepadXamlUI::TabItemViewModel const& item);
        void BuildRowElement(Row& row);
        void ReleaseRow(Row& row);
        void UpdateRow(Row& row);
        void RefreshVisibleRows();
        void ResetVisibleList();
        Row* FindRow(winrt::guid const& tabId);
        Row* FindRow(winrt::Windows::Foundation::IInspectable const& element);
        int IndexOfTab(winrt::guid const& tabId) const;

        // Selezione
        void SyncSelectionFromNative();
        void OnListSelectionChanged();
        void FocusEditor();

        // Riordino con trascinamento (gestito con il puntatore: il drag & drop OLE non raggiunge la
        // nostra isola). La riga si sposta dal vivo; al rilascio l'ordine passa al Blocco note.
        void OnListPointerPressed(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnListPointerMoved(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnListPointerReleased(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void BeginDrag(winrt::Microsoft::UI::Xaml::Input::Pointer const& pointer);
        void UpdateDrag();
        void EndDrag(bool commit);
        void AutoScrollStep();
        Row* RowFromSource(winrt::Windows::Foundation::IInspectable const& source, bool& onCloseButton);

        // Menu contestuale nativo, con in più il sottomenu "Colore scheda"
        void ShowNativeContextMenu(winrt::guid const& tabId, winrt::Microsoft::UI::Xaml::UIElement const& element,
                                   winrt::Windows::Foundation::Point const& localPoint);
        void AddColorMenu(winrt::Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, winrt::guid const& tabId);
        void RemoveColorMenu();
        void SetTabColor(winrt::guid const& tabId, tabcolors::TabColor color);

        // Divisore trascinabile per la larghezza del pannello
        void SetSplitterHighlight(bool on);
        void ResizeFromCursor();
        void ApplyPaneWidth(int widthDip);
        void PersistPaneWidth();

        // Comandi sui controlli nativi
        winrt::Microsoft::UI::Xaml::Controls::TabView NativeTabView();
        winrt::Microsoft::UI::Xaml::Controls::TabViewItem NativeContainer(int index);
        void CloseTab(winrt::guid const& tabId);
        void NewTab();
        void SetNativeStripHidden(bool hidden);
        void ScrollNativeStripToSelection();

        winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> m_tabsBar;
        HWND m_main = nullptr; // finestra top-level di questa TabsBar
        WindowLayout m_layout;
        std::unique_ptr<ScrollBarFit> m_vScrollFit; // barra di scorrimento verticale allungata con l'editor salito
        std::unique_ptr<ScrollBarFit> m_hScrollFit; // barra di scorrimento orizzontale accorciata della colonna
        Settings m_settings;
        bool m_attached = false;
        bool m_enabled = false;
        bool m_syncing = false;

        // La nostra isola XAML
        winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource m_source{ nullptr };
        HWND m_panelHwnd = nullptr;
        // Geometria del pannello in cima alla finestra (px dal suo bordo superiore; 0 = colonna accanto all'editor)
        int m_captionPx = 0;
        int m_contentTopPx = 0;
        // Regione applicata all'isola (UpdateCaptionRegion): fascia e pulsanti che restano (px dell'isola)
        int m_regionCaptionPx = 0;
        std::vector<RECT> m_regionKeep;

        // Interfaccia del pannello
        winrt::Microsoft::UI::Xaml::Controls::Grid m_root{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_header{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_toggle{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::FontIcon m_toggleIcon{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_body{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ListView m_list{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_newTab{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_separator{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_separatorActive{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_splitter{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> m_visible{ nullptr };
        bool m_horizontalWheelBlocked = false;
        bool m_compactVisual = false; // aspetto compresso attualmente applicato a righe e intestazione
        // Aspetto dei titoli (ReadNativeTitleStyle). Finché non è letto: dimensione del TabView di WinUI
        // (TabViewItemHeaderFontSize, 12; le righe della ListView userebbero 14), pesi 400/600 come le
        // schede native e colore ereditato.
        struct TitleStyle
        {
            double size = 12.0;
            winrt::Microsoft::UI::Xaml::Media::FontFamily family{ nullptr };
            winrt::Windows::UI::Text::FontWeight normalWeight{ 400 };
            winrt::Windows::UI::Text::FontWeight selectedWeight{ 600 };
            winrt::Microsoft::UI::Xaml::Media::Brush normal{ nullptr };
            winrt::Microsoft::UI::Xaml::Media::Brush hover{ nullptr }; // null: il passaggio del mouse non lo cambia
            winrt::Microsoft::UI::Xaml::Media::Brush selected{ nullptr };
        };
        TitleStyle m_titleStyle;
        bool m_titleStyleRead = false; // letti almeno scheda normale e scheda attiva
        uint32_t m_titleStyleTicks = 0;

        // Dati nativi (stesso ordine di TabItems())
        std::vector<std::unique_ptr<Row>> m_rows;
        std::vector<std::unique_ptr<Row>> m_stash; // righe tolte dal Blocco note: riusate se la scheda ricompare (riordino)
        bool m_refreshQueued = false;
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::NotepadXamlUI::TabItemViewModel> m_items{ nullptr };
        winrt::event_token m_itemsToken{};
        winrt::NotepadXamlUI::TabsBar::SelectionChanged_revoker m_nativeSelectionRevoker;
        winrt::Microsoft::UI::Xaml::FrameworkElement::ActualThemeChanged_revoker m_themeRevoker;
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::TabView> m_nativeTabView;

        // Trascinamento in corso
        bool m_dragPending = false; // tasto premuto su una riga, soglia di movimento non ancora superata
        bool m_dragging = false;
        uint32_t m_dragPointerId = 0;
        winrt::guid m_dragTabId{};
        int m_dragFromIndex = -1;
        winrt::Windows::Foundation::Point m_dragStart{}; // DIP, rispetto all'elenco
        double m_dragY = 0;                              // ultima posizione verticale del puntatore (DIP)
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_autoScrollTimer{ nullptr };

        // Menu contestuale aperto (e voci nostre aggiunte al menu nativo, tolte alla chiusura)
        winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutBase::Closed_revoker m_flyoutClosedRevoker;
        winrt::Microsoft::UI::Xaml::Controls::MenuFlyout m_extendedMenu{ nullptr };
        std::vector<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> m_menuExtras;

        // Ridimensionamento in corso
        bool m_resizing = false;
        bool m_splitterHovered = false;
        int m_grabOffsetPx = 0; // larghezza alla presa -/+ x del cursore: larghezza = offset +/- x attuale

        // Barra schede nativa (resa invisibile mentre il pannello è attivo)
        bool m_nativeHidden = false;
        double m_nativeOpacity = 1.0;
        bool m_nativeHitTest = true;

        // Barra dei menu resa trasparente (animazione attiva finché il pannello è attivo)
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::FrameworkElement> m_menuBar;
        winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_menuBarStoryboard{ nullptr };
        bool m_menuBarMissingLogged = false;
        winrt::Microsoft::UI::Xaml::FrameworkElement::LayoutUpdated_revoker m_menuBarLayoutRevoker; // zone vuote ricalcolate
    };
}
