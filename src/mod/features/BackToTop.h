#pragma once

#include "Settings.h"

namespace npx
{
    // Feature "Torna su" per una finestra del Blocco note: un pulsante tondo nell'angolo in basso a
    // destra dell'editor, che compare (sfumando) appena il documento scorre verso il basso e riporta
    // all'inizio con lo scorrimento animato di sistema.
    //
    // La posizione si legge dalla barra di scorrimento verticale del Blocco note, che è uno
    // ScrollViewer XAML nella propria isola, sincronizzato con l'editor nei due sensi: il suo evento
    // ViewChanged dice quando il documento scorre, e ChangeView(0) riporta in cima l'editor (lo stesso
    // percorso del trascinamento della barra). Nessun controllo periodico mentre il pulsante è nascosto.
    //
    // Il pulsante sta in una NOSTRA isola XAML sopra l'editor, trasparente intorno al cerchio come
    // l'isola della barra di scorrimento del Blocco note (WS_EX_LAYERED senza superficie GDI): sotto si
    // vede il testo vivo. L'isola è marcata come nostra (interop::kOverlayProp) perché il layout delle
    // schede verticali non la sposti come una finestra del Blocco note.
    //
    // L'isola resta "parcheggiata" (mostrata) finché c'è un editor e la finestra non è ridotta a icona:
    // comparsa e scomparsa del pulsante cambiano solo l'opacità, e a pulsante nascosto l'isola è
    // trasparente al mouse (WS_EX_TRANSPARENT). Mostrare o nascondere una finestra fa
    // generare a Windows un WM_MOUSEMOVE per la finestra sotto il cursore (l'editor), e il Blocco note
    // risponde come a un mouse vero: barra di scorrimento in modalità "da mouse" (cursore più corto e
    // più in basso). Mostrando l'isola al primo scatto sotto la cima, il cursore della barra cambiava
    // forma e grandezza mentre si scorreva.
    //
    // Vive e viene usata interamente sul thread UI della finestra.
    class BackToTop : public std::enable_shared_from_this<BackToTop>
    {
    public:
        explicit BackToTop(winrt::NotepadXamlUI::TabsBar const& tabsBar);
        ~BackToTop();
        BackToTop(BackToTop const&) = delete;
        BackToTop& operator=(BackToTop const&) = delete;

        void Apply(Settings const& settings);
        // Controllo di sicurezza (timer della finestra, 500 ms): barra di scorrimento ricreata, ecc.
        void OnTimer();
        // La finestra si chiude (o la mod rinuncia): toglie tutto.
        void Detach();

        // ScrollViewer chiamati "ScrollViewer" vivi sul thread della finestra (tra questi c'è la barra
        // di scorrimento verticale dell'editor).
        std::function<std::vector<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>()> scrollViewersProvider;

    private:
        bool Attach();
        bool EnsureIsland();
        void Enable();
        void Disable();
        // Barra di scorrimento verticale dell'editor di questa finestra (o null).
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer FindScrollBar() const;
        void HookScrollBar(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer const& scrollBar);
        void Update();
        void Show();
        void Hide();
        // Isola mostrata (pulsante invisibile e trasparente al mouse finché non serve) / nascosta davvero.
        void Park();
        void Unpark();
        void SetClickThrough(bool clickThrough);
        // Posizione del pulsante nell'angolo dell'editor; false se non c'è un editor visibile.
        bool Place();
        void ScrollToTop();
        // Un passo dello scorrimento verso l'inizio (animazione nostra: vedi ScrollToTop).
        void ScrollStep();
        void FocusEditor();
        void ApplyTheme();

        winrt::weak_ref<winrt::NotepadXamlUI::TabsBar> m_tabsBar;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };
        HWND m_main = nullptr;
        Settings m_settings;
        bool m_attached = false;
        bool m_enabled = false;

        // La nostra isola XAML
        winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource m_source{ nullptr };
        HWND m_islandHwnd = nullptr;
        winrt::Microsoft::UI::Xaml::Controls::Grid m_root{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_button{ nullptr };
        bool m_shown = false;          // pulsante visibile (o in comparsa)
        bool m_bridgeShown = false;    // isola mostrata ("parcheggiata", anche a pulsante nascosto)
        RECT m_placed{};               // dove sta l'isola (coordinate client della finestra)
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_followTimer{ nullptr }; // segue l'editor finché è visibile
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_scrollTimer{ nullptr }; // animazione verso l'inizio
        double m_scrollFrom = 0;
        ULONGLONG m_scrollStart = 0;

        // Barra di scorrimento seguita
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer> m_scrollBar;
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer::ViewChanged_revoker m_viewChangedRevoker;
        winrt::Microsoft::UI::Xaml::XamlRoot::Changed_revoker m_rootChangedRevoker; // isola della barra mostrata/nascosta
        winrt::Microsoft::UI::Xaml::FrameworkElement::ActualThemeChanged_revoker m_themeRevoker;
        bool m_missingLogged = false;
    };
}
