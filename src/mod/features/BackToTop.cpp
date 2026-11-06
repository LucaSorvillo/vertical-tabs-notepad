#include "pch.h"
#include "BackToTop.h"

#include "Interop.h"
#include "Log.h"
#include "Shared.h"

namespace npx
{
    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        using winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority;

        constexpr int kSizeDip = 40;        // diametro del pulsante
        constexpr int kSideMarginDip = 12;  // distanza dalla barra di scorrimento verticale
        constexpr int kBottomMarginDip = 16; // distanza dal bordo inferiore dell'editor
        constexpr int kFadeMs = 150;
        constexpr int kFollowMs = 120;      // mentre è visibile, il pulsante segue l'editor (finestra ridimensionata, pannello...)
        constexpr double kScrollMs = 260.0; // durata dello scorrimento verso l'inizio
        constexpr int kScrollStepMs = 15;   // un passo per fotogramma (circa)
        constexpr double kMinOffset = 0.5;  // basta aver cominciato a scorrere

        // Pulsante tondo con la freccia in su. Colori pieni (il pulsante sta sopra il testo: niente
        // trasparenze) presi dalla palette Fluent, per tema; il modello è minimo, con gli stati di
        // passaggio del mouse e pressione. La radice non ha sfondo: intorno al cerchio l'isola è
        // trasparente e si vede il testo.
        constexpr wchar_t kButtonXaml[] = LR"xaml(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Grid.Resources>
    <ResourceDictionary>
      <ResourceDictionary.ThemeDictionaries>
        <ResourceDictionary x:Key="Dark">
          <SolidColorBrush x:Key="NpxTopFill" Color="#3C3C3C"/>
          <SolidColorBrush x:Key="NpxTopFillHover" Color="#474747"/>
          <SolidColorBrush x:Key="NpxTopFillPressed" Color="#333333"/>
          <SolidColorBrush x:Key="NpxTopStroke" Color="#555555"/>
          <SolidColorBrush x:Key="NpxTopGlyph" Color="#FFFFFF"/>
        </ResourceDictionary>
        <ResourceDictionary x:Key="Light">
          <SolidColorBrush x:Key="NpxTopFill" Color="#FFFFFF"/>
          <SolidColorBrush x:Key="NpxTopFillHover" Color="#F5F5F5"/>
          <SolidColorBrush x:Key="NpxTopFillPressed" Color="#EBEBEB"/>
          <SolidColorBrush x:Key="NpxTopStroke" Color="#D6D6D6"/>
          <SolidColorBrush x:Key="NpxTopGlyph" Color="#1A1A1A"/>
        </ResourceDictionary>
      </ResourceDictionary.ThemeDictionaries>
    </ResourceDictionary>
  </Grid.Resources>
  <Button x:Name="TopButton" Width="40" Height="40" Padding="0" IsTabStop="False" HorizontalAlignment="Center" VerticalAlignment="Center"
          ToolTipService.ToolTip="Torna su" AutomationProperties.Name="Torna su" AutomationProperties.AutomationId="NpxBackToTop">
    <Button.Template>
      <ControlTemplate TargetType="Button">
        <Grid x:Name="Disc" CornerRadius="20" BorderThickness="1" Background="{ThemeResource NpxTopFill}" BorderBrush="{ThemeResource NpxTopStroke}">
          <VisualStateManager.VisualStateGroups>
            <VisualStateGroup x:Name="CommonStates">
              <VisualState x:Name="Normal"/>
              <VisualState x:Name="PointerOver">
                <VisualState.Setters>
                  <Setter Target="Disc.Background" Value="{ThemeResource NpxTopFillHover}"/>
                </VisualState.Setters>
              </VisualState>
              <VisualState x:Name="Pressed">
                <VisualState.Setters>
                  <Setter Target="Disc.Background" Value="{ThemeResource NpxTopFillPressed}"/>
                </VisualState.Setters>
              </VisualState>
              <VisualState x:Name="Disabled"/>
            </VisualStateGroup>
          </VisualStateManager.VisualStateGroups>
          <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
        </Grid>
      </ControlTemplate>
    </Button.Template>
    <FontIcon Glyph="&#xE74A;" FontSize="16" Foreground="{ThemeResource NpxTopGlyph}"/>
  </Button>
</Grid>)xaml";

        // Editor visibile (contenitore NotepadTextBox, figlio diretto della finestra principale).
        HWND VisibleEditor(HWND main)
        {
            for (HWND child = GetWindow(main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                if (IsWindowVisible(child) && interop::ClassOf(child) == L"NotepadTextBox")
                {
                    return child;
                }
            }
            return nullptr;
        }

        bool IsEmpty(RECT const& r)
        {
            return r.right <= r.left || r.bottom <= r.top;
        }
    }

    BackToTop::BackToTop(winrt::NotepadXamlUI::TabsBar const& tabsBar) : m_tabsBar(winrt::make_weak(tabsBar)), m_dispatcher(tabsBar.DispatcherQueue())
    {
    }

    BackToTop::~BackToTop()
    {
        Detach();
    }

    void BackToTop::Apply(Settings const& settings)
    {
        if (!m_attached && !Attach())
        {
            return;
        }
        m_settings = settings;
        // Gate per-finestra (sviluppo): se esiste dev-windows.txt agiamo solo sulle finestre elencate.
        const bool wanted = settings.backToTop && WindowApplyAllowed(m_main);
        if (wanted && !m_enabled)
        {
            Enable();
        }
        else if (!wanted && m_enabled)
        {
            Disable();
        }
    }

    bool BackToTop::Attach()
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return false;
        }
        HWND main = nullptr;
        try
        {
            if (const auto xamlRoot = tabsBar.XamlRoot())
            {
                if (const auto environment = xamlRoot.ContentIslandEnvironment())
                {
                    main = GetAncestor(interop::HwndFromWindowId(environment.AppWindowId()), GA_ROOT);
                }
            }
        }
        catch (...)
        {
        }
        if (!main || !IsWindow(main))
        {
            return false;
        }
        m_main = main;
        const std::weak_ptr<BackToTop> weak = weak_from_this();
        m_themeRevoker = tabsBar.ActualThemeChanged(winrt::auto_revoke, [weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->ApplyTheme();
            }
        });
        m_attached = true;
        return true;
    }

    // La nostra isola si crea solo alla prima attivazione: con la feature spenta la mod non aggiunge nulla.
    bool BackToTop::EnsureIsland()
    {
        if (m_source)
        {
            return true;
        }
        if (!m_main || !IsWindow(m_main))
        {
            return false;
        }
        try
        {
            m_root = mux::Markup::XamlReader::Load(kButtonXaml).as<muxc::Grid>();
            m_button = m_root.FindName(L"TopButton").as<muxc::Button>();
            const std::weak_ptr<BackToTop> weak = weak_from_this();
            m_button.Click([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->ScrollToTop();
                }
            });
            // Comparsa e scomparsa in dissolvenza: basta cambiare l'opacità.
            mux::ScalarTransition fade;
            fade.Duration(std::chrono::milliseconds(kFadeMs));
            m_root.OpacityTransition(fade);
            m_root.Opacity(0);

            m_source = winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource();
            m_source.Initialize(interop::WindowIdFromHwnd(m_main));
            m_source.Content(m_root);
            const auto bridge = m_source.SiteBridge();
            m_islandHwnd = interop::HwndFromWindowId(bridge.WindowId());
            bridge.Hide();
            m_bridgeShown = false;
        }
        catch (...)
        {
            NPX_LOG(L"BackToTop: impossibile creare l'isola del pulsante (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            if (m_source)
            {
                try
                {
                    m_source.Close();
                }
                catch (...)
                {
                }
                m_source = nullptr;
            }
            m_root = nullptr;
            m_button = nullptr;
            return false;
        }

        // Trasparente intorno al cerchio, come l'isola della barra di scorrimento del Blocco note; e
        // marcata come nostra, perché il layout delle schede verticali non la sposti. Nasce con il pulsante
        // nascosto: trasparente anche al mouse.
        SetPropW(m_islandHwnd, interop::kOverlayProp, reinterpret_cast<HANDLE>(1));
        SetWindowLongPtrW(m_islandHwnd, GWL_EXSTYLE, GetWindowLongPtrW(m_islandHwnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
        ApplyTheme();
        NPX_LOG(L"BackToTop: pulsante creato (finestra %llx, isola %llx).", reinterpret_cast<uintptr_t>(m_main), reinterpret_cast<uintptr_t>(m_islandHwnd));
        return true;
    }

    void BackToTop::ApplyTheme()
    {
        if (!m_root)
        {
            return;
        }
        if (const auto tabsBar = m_tabsBar.get())
        {
            m_root.RequestedTheme(tabsBar.ActualTheme());
        }
    }

    void BackToTop::Enable()
    {
        if (!EnsureIsland())
        {
            return;
        }
        m_enabled = true;
        m_missingLogged = false;
        if (const auto scrollBar = FindScrollBar())
        {
            HookScrollBar(scrollBar);
        }
        Update();
    }

    void BackToTop::Disable()
    {
        m_enabled = false;
        m_viewChangedRevoker.revoke();
        m_rootChangedRevoker.revoke();
        m_scrollBar = nullptr;
        m_shown = false;
        if (m_scrollTimer)
        {
            m_scrollTimer.Stop();
        }
        if (m_followTimer)
        {
            m_followTimer.Stop();
        }
        if (m_root)
        {
            m_root.Opacity(0);
        }
        SetClickThrough(true);
        Unpark();
    }

    void BackToTop::Detach()
    {
        Disable();
        m_themeRevoker.revoke();
        m_followTimer = nullptr;
        m_scrollTimer = nullptr;
        if (m_islandHwnd && IsWindow(m_islandHwnd))
        {
            RemovePropW(m_islandHwnd, interop::kOverlayProp);
        }
        if (m_source)
        {
            try
            {
                m_source.Close();
            }
            catch (...)
            {
            }
            m_source = nullptr;
        }
        m_root = nullptr;
        m_button = nullptr;
        m_islandHwnd = nullptr;
        m_attached = false;
    }

    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer BackToTop::FindScrollBar() const
    {
        if (!scrollViewersProvider)
        {
            return nullptr;
        }
        // La barra verticale dell'editor è l'unico ScrollViewer "ScrollViewer" di questa finestra che
        // sta in un'isola stretta e alta (quelli della striscia schede e degli elenchi stanno in isole larghe).
        for (auto const& candidate : scrollViewersProvider())
        {
            try
            {
                const auto xamlRoot = candidate.XamlRoot();
                const auto environment = xamlRoot ? xamlRoot.ContentIslandEnvironment() : nullptr;
                if (!environment || GetAncestor(interop::HwndFromWindowId(environment.AppWindowId()), GA_ROOT) != m_main)
                {
                    continue;
                }
                const auto size = xamlRoot.Size();
                if (size.Width <= 0 || size.Width > 40 || size.Height < 3 * size.Width)
                {
                    continue;
                }
                return candidate;
            }
            catch (...)
            {
            }
        }
        return nullptr;
    }

    void BackToTop::HookScrollBar(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer const& scrollBar)
    {
        m_viewChangedRevoker.revoke();
        m_rootChangedRevoker.revoke();
        m_scrollBar = winrt::make_weak(scrollBar);
        const std::weak_ptr<BackToTop> weak = weak_from_this();
        const auto update = [weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->Update();
            }
        };
        m_viewChangedRevoker = scrollBar.ViewChanged(winrt::auto_revoke, update);
        // Su un documento che non scorre il Blocco note nasconde l'isola della barra senza aggiornarla:
        // conta anche la sua visibilità (XamlRoot.IsHostVisible, che cambia con XamlRoot.Changed).
        if (const auto xamlRoot = scrollBar.XamlRoot())
        {
            m_rootChangedRevoker = xamlRoot.Changed(winrt::auto_revoke, update);
        }
        NPX_VLOG(L"BackToTop: barra di scorrimento dell'editor trovata.");
    }

    void BackToTop::OnTimer()
    {
        if (!m_enabled)
        {
            return;
        }
        // La barra di scorrimento può nascere dopo di noi o essere ricreata.
        if (!m_scrollBar.get())
        {
            if (const auto scrollBar = FindScrollBar())
            {
                HookScrollBar(scrollBar);
            }
            else if (!m_missingLogged)
            {
                m_missingLogged = true;
                NPX_VLOG(L"BackToTop: barra di scorrimento dell'editor non ancora trovata.");
            }
        }
        Update();
    }

    void BackToTop::Update()
    {
        // Isola parcheggiata finché c'è un editor (posto per il pulsante) e la finestra non è ridotta a icona.
        if (!m_enabled || !m_source || !m_main || IsIconic(m_main) || !Place())
        {
            Hide();
            Unpark();
            return;
        }
        Park();
        bool wanted = false;
        if (const auto scrollBar = m_scrollBar.get())
        {
            try
            {
                const auto xamlRoot = scrollBar.XamlRoot();
                wanted = xamlRoot && xamlRoot.IsHostVisible() && scrollBar.VerticalOffset() > kMinOffset && scrollBar.ScrollableHeight() > 0;
            }
            catch (...)
            {
            }
        }
        wanted ? Show() : Hide();
    }

    void BackToTop::SetClickThrough(bool clickThrough)
    {
        if (!m_islandHwnd || !IsWindow(m_islandHwnd))
        {
            return;
        }
        const LONG_PTR style = GetWindowLongPtrW(m_islandHwnd, GWL_EXSTYLE);
        const LONG_PTR wanted = clickThrough ? (style | WS_EX_TRANSPARENT) : (style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
        if (wanted != style)
        {
            SetWindowLongPtrW(m_islandHwnd, GWL_EXSTYLE, wanted);
        }
    }

    void BackToTop::Park()
    {
        if (m_bridgeShown || !m_source || !m_islandHwnd || !IsWindow(m_islandHwnd))
        {
            return; // già mostrata (o isola già distrutta: finestra in chiusura)
        }
        SetClickThrough(!m_shown);
        try
        {
            m_source.SiteBridge().Show();
            m_bridgeShown = true;
            NPX_VLOG(L"BackToTop: isola del pulsante parcheggiata.");
        }
        catch (...)
        {
        }
    }

    void BackToTop::Unpark()
    {
        // Solo se la finestra dell'isola esiste ancora: chiudendo la finestra del Blocco note l'isola è già distrutta e
        // Hide() su un ponte chiuso è un accesso non valido (crash del Blocco note), che try/catch non intercetta.
        if (m_bridgeShown && m_source && m_islandHwnd && IsWindow(m_islandHwnd))
        {
            try
            {
                m_source.SiteBridge().Hide();
            }
            catch (...)
            {
            }
        }
        m_bridgeShown = false;
    }

    bool BackToTop::Place()
    {
        const HWND editor = m_main ? VisibleEditor(m_main) : nullptr;
        if (!editor || !m_islandHwnd)
        {
            return false;
        }
        const RECT area = interop::RectInClientOf(editor, m_main);
        if (IsEmpty(area))
        {
            return false;
        }
        // La barra di scorrimento verticale (isola stretta sul bordo destro dell'editor) e, se c'è, quella
        // orizzontale (isola bassa sul bordo inferiore) si sovrappongono all'editor: il pulsante sta accanto.
        const UINT dpi = std::max<UINT>(GetDpiForWindow(m_main), USER_DEFAULT_SCREEN_DPI);
        const int barMax = MulDiv(40, dpi, 96);
        LONG right = area.right;
        LONG bottom = area.bottom;
        for (HWND child = GetWindow(m_main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (child == m_islandHwnd || !IsWindowVisible(child) || interop::IsOverlay(child) ||
                interop::ClassOf(child) != L"Microsoft.UI.Content.DesktopChildSiteBridge")
            {
                continue;
            }
            const RECT bar = interop::RectInClientOf(child, m_main);
            const LONG width = bar.right - bar.left;
            const LONG height = bar.bottom - bar.top;
            if (width > 0 && width <= barMax && height > (area.bottom - area.top) / 2 && bar.right >= area.right - barMax && bar.left < area.right)
            {
                right = std::min(right, bar.left);
            }
            else if (height > 0 && height <= barMax && width > (area.right - area.left) / 2 && bar.bottom >= area.bottom - barMax && bar.top < area.bottom)
            {
                bottom = std::min(bottom, bar.top);
            }
        }
        const int size = MulDiv(kSizeDip, dpi, 96);
        const RECT wanted{ right - MulDiv(kSideMarginDip, dpi, 96) - size, bottom - MulDiv(kBottomMarginDip, dpi, 96) - size,
                           right - MulDiv(kSideMarginDip, dpi, 96), bottom - MulDiv(kBottomMarginDip, dpi, 96) };
        if (wanted.left < area.left || wanted.top < area.top)
        {
            return false; // editor troppo piccolo
        }
        // Sempre sopra l'editor (il Blocco note crea un editor nuovo per ogni documento aperto).
        HWND above = GetWindow(m_islandHwnd, GW_HWNDPREV);
        bool onTop = true;
        for (; above; above = GetWindow(above, GW_HWNDPREV))
        {
            if (above == editor)
            {
                onTop = false;
                break;
            }
        }
        const RECT current = interop::RectInClientOf(m_islandHwnd, m_main);
        if (!EqualRect(&wanted, &m_placed) || !onTop || !EqualRect(&wanted, &current))
        {
            SetWindowPos(m_islandHwnd, HWND_TOP, wanted.left, wanted.top, wanted.right - wanted.left, wanted.bottom - wanted.top,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            m_placed = wanted;
        }
        return true;
    }

    void BackToTop::Show()
    {
        // (Update ha già messo l'isola al suo posto e parcheggiata: qui nessuna finestra cambia.)
        if (!m_bridgeShown)
        {
            return;
        }
        if (!m_shown)
        {
            m_shown = true;
            SetClickThrough(false);
            m_root.Opacity(1);
            NPX_VLOG(L"BackToTop: pulsante visibile.");
        }
        // Finché è visibile segue l'editor (finestra ridimensionata, pannello delle schede, cambio di scheda).
        if (!m_followTimer)
        {
            m_followTimer = m_dispatcher.CreateTimer();
            m_followTimer.Interval(std::chrono::milliseconds(kFollowMs));
            m_followTimer.IsRepeating(true);
            const std::weak_ptr<BackToTop> weak = weak_from_this();
            m_followTimer.Tick([weak](auto&&, auto&&) {
                if (const auto self = weak.lock(); self && self->m_shown)
                {
                    self->Update();
                }
            });
        }
        if (!m_followTimer.IsRunning())
        {
            m_followTimer.Start();
        }
    }

    void BackToTop::Hide()
    {
        if (m_followTimer)
        {
            m_followTimer.Stop();
        }
        if (!m_shown)
        {
            return;
        }
        m_shown = false;
        // Il mouse passa subito al testo sotto (anche durante la dissolvenza); l'isola resta mostrata: nasconderla farebbe
        // generare a Windows un movimento del mouse per l'editor (vedi BackToTop.h).
        SetClickThrough(true);
        if (m_root)
        {
            m_root.Opacity(0);
        }
        NPX_VLOG(L"BackToTop: pulsante nascosto.");
    }

    void BackToTop::ScrollToTop()
    {
        // L'editor segue la barra di scorrimento come quando la si trascina. L'animazione di sistema
        // (ChangeView animato) però si ferma a metà, contrastata dalla sincronizzazione del Blocco note
        // con l'editor: l'animazione la facciamo noi, con piccoli salti (ChangeView senza animazione,
        // che l'editor segue sempre) in kScrollMs, rallentando verso la fine.
        const auto scrollBar = m_scrollBar.get();
        if (scrollBar)
        {
            m_scrollFrom = scrollBar.VerticalOffset();
            m_scrollStart = GetTickCount64();
            if (!m_scrollTimer)
            {
                m_scrollTimer = m_dispatcher.CreateTimer();
                m_scrollTimer.Interval(std::chrono::milliseconds(kScrollStepMs));
                m_scrollTimer.IsRepeating(true);
                const std::weak_ptr<BackToTop> weak = weak_from_this();
                m_scrollTimer.Tick([weak](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        self->ScrollStep();
                    }
                });
            }
            NPX_VLOG(L"BackToTop: torna su (da %.0f).", m_scrollFrom);
            m_scrollTimer.Start();
            ScrollStep();
        }
        FocusEditor();
    }

    void BackToTop::ScrollStep()
    {
        const auto scrollBar = m_scrollBar.get();
        const double elapsed = static_cast<double>(GetTickCount64() - m_scrollStart);
        const double t = std::min(1.0, elapsed / kScrollMs);
        const double eased = 1.0 - std::pow(1.0 - t, 3.0); // rallenta verso la fine
        const double offset = t >= 1.0 ? 0.0 : m_scrollFrom * (1.0 - eased);
        if (scrollBar)
        {
            try
            {
                scrollBar.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>(offset), nullptr, true);
            }
            catch (...)
            {
                NPX_LOG(L"BackToTop: scorrimento non riuscito (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                m_scrollTimer.Stop();
                return;
            }
        }
        if (t >= 1.0 || !scrollBar)
        {
            m_scrollTimer.Stop();
        }
    }

    void BackToTop::FocusEditor()
    {
        // Il clic ha dato il focus alla nostra isola: torna all'editor, a scorrimento avviato.
        const std::weak_ptr<BackToTop> weak = weak_from_this();
        m_dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [weak] {
            const auto self = weak.lock();
            if (!self || !self->m_main)
            {
                return;
            }
            const HWND target = interop::FindDescendantByClass(self->m_main, L"RichEditD2DPT", true);
            if (!target)
            {
                return;
            }
            const DWORD current = GetCurrentThreadId();
            const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
            const bool attached = targetThread != current && AttachThreadInput(current, targetThread, TRUE);
            SetFocus(target);
            if (attached)
            {
                AttachThreadInput(current, targetThread, FALSE);
            }
        });
    }
}
