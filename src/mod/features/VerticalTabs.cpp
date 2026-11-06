#include "pch.h"
#include "VerticalTabs.h"

#include "Interop.h"
#include "Log.h"
#include "Shared.h"

namespace npx
{
    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        namespace muxp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;
        namespace muxa = winrt::Microsoft::UI::Xaml::Media::Animation;
        namespace muxi = winrt::Microsoft::UI::Xaml::Input;
        namespace wfc = winrt::Windows::Foundation::Collections;
        using winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority;
        using winrt::Windows::Foundation::IInspectable;
        using winrt::Windows::Foundation::Point;
        using winrt::Windows::System::VirtualKey;

        // Pannello: intestazione (comprimi/espandi), elenco schede con il "+" subito sotto
        // l'ultima scheda, separatore e divisore trascinabile. I pennelli {ThemeResource} seguono il tema
        // della radice (impostato come quello del Blocco note).
        // Lo sfondo DEVE essere opaco: l'editor del Blocco note disegna nella superficie della finestra
        // principale e, quando lo spostiamo, sotto il pannello resterebbe un'immagine fantasma del testo.
        // Di norma lo sfondo è la Mica Alt dell'isola (ApplyBackdrop, opaca come quella della barra del
        // titolo); il pennello del XAML è il ripiego, cioè il colore pieno della Mica (#202020 / #F3F3F3).
        constexpr wchar_t kPanelXaml[] = LR"xaml(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" %BACKGROUND%>
  <Grid.RowDefinitions>
    <RowDefinition Height="Auto"/>
    <RowDefinition Height="*"/>
  </Grid.RowDefinitions>
  <Grid x:Name="Header" Grid.Row="0" Margin="6,6,6,2">
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="Auto"/>
      <ColumnDefinition Width="*"/>
    </Grid.ColumnDefinitions>
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto"/>
    </Grid.RowDefinitions>
    <Button x:Name="Toggle" Width="40" Height="34" Padding="0" Background="Transparent" BorderThickness="0" TabIndex="3"
            AutomationProperties.Name="Comprimi schede">
      <FontIcon x:Name="ToggleIcon" Glyph="&#xEA49;" FontSize="16"/>
    </Button>
  </Grid>
  <!-- Corpo: l'elenco è alto quanto le sue righe (fino allo spazio disponibile, poi scorre: MaxHeight
       impostato dal codice) e il "+" lo segue subito sotto; con molte schede resta visibile in fondo. -->
  <Grid x:Name="Body" Grid.Row="1">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto"/>
      <RowDefinition Height="Auto"/>
      <RowDefinition Height="*"/>
    </Grid.RowDefinitions>
  <ListView x:Name="List" Grid.Row="0" VerticalAlignment="Top" SelectionMode="Single" IsItemClickEnabled="True" Padding="0,2,0,2"
            TabIndex="0" MaxHeight="0" AutomationProperties.Name="Schede"
            ScrollViewer.HorizontalScrollMode="Disabled" ScrollViewer.HorizontalScrollBarVisibility="Disabled">
    <!-- La scheda attiva la disegna la riga (fondo, bordino e stanghetta nel colore della scheda): fondo e
         stanghetta di selezione predefiniti spenti. Il passaggio del mouse resta quello di sistema. -->
    <ListView.Resources>
      <SolidColorBrush x:Key="ListViewItemBackgroundSelected" Color="Transparent"/>
      <SolidColorBrush x:Key="ListViewItemBackgroundSelectedPointerOver" Color="Transparent"/>
      <SolidColorBrush x:Key="ListViewItemBackgroundSelectedPressed" Color="Transparent"/>
      <SolidColorBrush x:Key="ListViewItemSelectionIndicatorBrush" Color="Transparent"/>
      <SolidColorBrush x:Key="ListViewItemSelectionIndicatorPointerOverBrush" Color="Transparent"/>
      <SolidColorBrush x:Key="ListViewItemSelectionIndicatorPressedBrush" Color="Transparent"/>
    </ListView.Resources>
    <!-- Solo animazione di riposizionamento: quelle di comparsa/rimozione predefinite tengono la riga
         rimossa agganciata all'elenco mentre sfuma, e le righe (che fanno da contenitore di sé stesse)
         devono poter essere reinserite subito, ad esempio durante il trascinamento. -->
    <ListView.ItemContainerTransitions>
      <TransitionCollection>
        <RepositionThemeTransition IsStaggeringEnabled="False"/>
      </TransitionCollection>
    </ListView.ItemContainerTransitions>
    <ListView.ItemContainerStyle>
      <Style TargetType="ListViewItem" %BASEDON%>
        <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
        <Setter Property="MinHeight" Value="36"/>
        <Setter Property="MinWidth" Value="0"/>
        <Setter Property="Padding" Value="0"/>
      </Style>
    </ListView.ItemContainerStyle>
  </ListView>
  <Button x:Name="NewTab" Grid.Row="1" Height="32" VerticalAlignment="Top" HorizontalAlignment="Stretch"
          HorizontalContentAlignment="Center" Padding="0" Background="Transparent" BorderThickness="0" TabIndex="2"
          ToolTipService.ToolTip="Nuova scheda" AutomationProperties.Name="Nuova scheda">
    <FontIcon Glyph="&#xE710;" FontSize="14"/>
  </Button>
  </Grid>
  <Border x:Name="Separator" Grid.RowSpan="2" Width="1" HorizontalAlignment="Right"
          Background="{ThemeResource DividerStrokeColorDefaultBrush}"/>
  <Border x:Name="SeparatorActive" Grid.RowSpan="2" Width="2" HorizontalAlignment="Right" Visibility="Collapsed"
          Background="{ThemeResource AccentFillColorDefaultBrush}"/>
  <Border x:Name="Splitter" Grid.RowSpan="2" Width="6" HorizontalAlignment="Right" Background="Transparent"
          AutomationProperties.Name="Larghezza pannello schede"/>
</Grid>)xaml";

        // Una riga = un ListViewItem che fa da contenitore di sé stesso (niente riciclo di
        // contenitori: lo stesso elemento può essere tolto e rimesso nell'elenco, ad esempio trascinandolo).
        // L'icona a sinistra (iniziale del titolo, nel colore assegnato alla scheda) fa le veci della
        // favicon delle schede del browser e compare solo a pannello compresso, dove è l'unica parte visibile.
        // Sotto il contenuto c'è il fondo della riga, una "card" tinta dal colore della scheda: è
        // semitrasparente, così il passaggio del mouse (disegnato sotto) resta visibile. Il fondo si
        // estende sotto il bordino, che lo schiarisce (come un velo) solo nella scheda attiva.
        constexpr wchar_t kRowXaml[] = LR"xaml(<ListViewItem xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Grid Height="36">
  <Border x:Name="RowTint" Margin="4,2,4,2" CornerRadius="4" BorderThickness="1" BackgroundSizing="OuterBorderEdge"/>
  <Border x:Name="PillNeutral" Width="3" Height="16" Margin="5,0,0,0" CornerRadius="1.5" HorizontalAlignment="Left"
          VerticalAlignment="Center" Background="{ThemeResource ControlStrongFillColorDefaultBrush}" Visibility="Collapsed"/>
  <Border x:Name="PillColor" Width="3" Height="16" Margin="5,0,0,0" CornerRadius="1.5" HorizontalAlignment="Left"
          VerticalAlignment="Center" Visibility="Collapsed"/>
  <Grid x:Name="RowGrid" Margin="10,0,4,0" Background="Transparent" ColumnSpacing="10">
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="Auto"/>
      <ColumnDefinition Width="*"/>
      <ColumnDefinition Width="28"/>
    </Grid.ColumnDefinitions>
    <Grid x:Name="Avatar" Width="24" Height="24" VerticalAlignment="Center">
      <Border x:Name="AvatarNeutral" CornerRadius="6" BorderThickness="1"
              Background="{ThemeResource ControlFillColorDefaultBrush}" BorderBrush="{ThemeResource ControlStrokeColorDefaultBrush}">
        <TextBlock x:Name="AvatarNeutralText" FontSize="12" FontWeight="SemiBold" HorizontalAlignment="Center" VerticalAlignment="Center"
                   Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
      </Border>
      <Border x:Name="AvatarColor" CornerRadius="6" Visibility="Collapsed">
        <TextBlock x:Name="AvatarColorText" FontSize="12" FontWeight="SemiBold" HorizontalAlignment="Center" VerticalAlignment="Center"/>
      </Border>
      <Ellipse x:Name="Badge" Width="8" Height="8" HorizontalAlignment="Right" VerticalAlignment="Top" Margin="0,-3,-3,0"
               Fill="{ThemeResource TextFillColorSecondaryBrush}" Visibility="Collapsed"/>
    </Grid>
    <TextBlock x:Name="Title" Grid.Column="1" VerticalAlignment="Center" TextTrimming="CharacterEllipsis" TextWrapping="NoWrap"/>
    <Ellipse x:Name="Dot" Grid.Column="2" Width="8" Height="8" HorizontalAlignment="Center" VerticalAlignment="Center"
             Fill="{ThemeResource TextFillColorSecondaryBrush}" Visibility="Collapsed"/>
    <Button x:Name="Close" Grid.Column="2" Width="28" Height="28" Padding="0" Background="Transparent" BorderThickness="0"
            Visibility="Collapsed" ToolTipService.ToolTip="Chiudi scheda" AutomationProperties.Name="Chiudi scheda">
      <FontIcon Glyph="&#xE711;" FontSize="10"/>
    </Button>
  </Grid>
  </Grid>
</ListViewItem>)xaml";

        // "+" sotto l'ultima scheda: occupa lo spazio di una riga (alto quanto lo sfondo di una riga, con
        // lo stesso rientro), più un margine in fondo; tutto questo si sottrae allo spazio dell'elenco.
        constexpr double kNewTabHeight = 32.0;
        constexpr double kNewTabTopMargin = 2.0;
        constexpr double kNewTabBottomMargin = 6.0;
        // Rientro orizzontale (DIP) degli sfondi delle righe della ListView (misurato: 4 DIP ai lati,
        // 2 DIP sopra e sotto): il "+" e il fondo delle righe vi si allineano.
        constexpr double kRowInset = 4.0;

        // Fondo delle righe (opacità 0-255 sopra lo sfondo del pannello), sull'esempio scelto dall'utente:
        // il colore della scheda è una tinta tenue (tema scuro: tono chiaro; tema chiaro: tono saturo);
        // senza colore, un velo neutro appena visibile (bianco nel tema scuro, nero nel chiaro). La
        // scheda attiva ha la tinta più decisa e un bordino dello stesso velo neutro.
        struct CardStyle
        {
            uint8_t colored;
            uint8_t coloredSelected;
            uint8_t neutral;
            uint8_t neutralSelected;
            uint8_t border;
        };
        constexpr CardStyle kCardDark{ 0x26, 0x45, 0x0B, 0x1A, 0x22 };
        constexpr CardStyle kCardLight{ 0x1F, 0x38, 0x08, 0x0F, 0x1F };
        // Stanghetta della scheda attiva nel tema scuro: il colore della scheda appena più profondo.
        constexpr double kPillShadeDark = 0.9;
        // Aspetto dei titoli riletto dalle schede native ogni N controlli periodici (da 500 ms).
        constexpr uint32_t kTitleStyleEveryTicks = 4;

        constexpr double kSplitterGap = 6.0;     // spazio lasciato libero dall'elenco per il divisore
        // Pulsanti dell'intestazione del pannello: altezza normale e margine (sopra e sotto) nella fascia del titolo.
        constexpr double kHeaderButtonHeight = 34.0;
        constexpr double kCaptionButtonInset = 2.0;
        constexpr double kDragThreshold = 6.0;   // DIP di movimento prima che una pressione diventi trascinamento
        constexpr double kAutoScrollEdge = 28.0; // fascia (DIP) ai bordi dell'elenco che lo fa scorrere
        constexpr double kAutoScrollMaxStep = 18.0;

        // Alza un flag per la durata di un blocco, anche se nel blocco viene lanciata un'eccezione.
        class FlagScope
        {
        public:
            explicit FlagScope(bool& flag) : m_flag(flag), m_previous(flag) { m_flag = true; }
            ~FlagScope() { m_flag = m_previous; }
            FlagScope(FlagScope const&) = delete;
            FlagScope& operator=(FlagScope const&) = delete;

        private:
            bool& m_flag;
            bool m_previous;
        };

        mux::FrameworkElement LoadXaml(std::wstring const& xaml)
        {
            return mux::Markup::XamlReader::Load(xaml).as<mux::FrameworkElement>();
        }

        template <typename T>
        T FindPart(mux::FrameworkElement const& root, wchar_t const* name)
        {
            return root.FindName(name).try_as<T>();
        }

        // Ricerca in profondità nell'albero visuale (per raggiungere i controlli nativi).
        template <typename T>
        T FindDescendant(mux::DependencyObject const& root, std::wstring_view name = {})
        {
            const int count = muxm::VisualTreeHelper::GetChildrenCount(root);
            for (int i = 0; i < count; ++i)
            {
                const auto child = muxm::VisualTreeHelper::GetChild(root, i);
                if (const auto typed = child.try_as<T>())
                {
                    if (name.empty())
                    {
                        return typed;
                    }
                    if (const auto element = child.try_as<mux::FrameworkElement>(); element && element.Name() == name)
                    {
                        return typed;
                    }
                }
                if (const auto found = FindDescendant<T>(child, name))
                {
                    return found;
                }
            }
            return nullptr;
        }

        // Primo TextBlock (in profondità) con quel testo.
        muxc::TextBlock FindTextBlock(mux::DependencyObject const& root, winrt::hstring const& text)
        {
            const int count = muxm::VisualTreeHelper::GetChildrenCount(root);
            for (int i = 0; i < count; ++i)
            {
                const auto child = muxm::VisualTreeHelper::GetChild(root, i);
                if (const auto block = child.try_as<muxc::TextBlock>(); block && block.Text() == text)
                {
                    return block;
                }
                if (const auto found = FindTextBlock(child, text))
                {
                    return found;
                }
            }
            return nullptr;
        }

        // Gruppo di stati visivi "CommonStates" del modello di un controllo (sta sulla radice del modello).
        mux::VisualStateGroup CommonStates(muxc::Control const& control)
        {
            if (muxm::VisualTreeHelper::GetChildrenCount(control) == 0)
            {
                return nullptr;
            }
            const auto root = muxm::VisualTreeHelper::GetChild(control, 0).try_as<mux::FrameworkElement>();
            if (!root)
            {
                return nullptr;
            }
            for (auto const& group : mux::VisualStateManager::GetVisualStateGroups(root))
            {
                if (group.Name() == L"CommonStates")
                {
                    return group;
                }
            }
            return nullptr;
        }

        bool SameBrush(muxm::Brush const& a, muxm::Brush const& b)
        {
            if (a == b)
            {
                return true;
            }
            const auto solidA = a ? a.try_as<muxm::SolidColorBrush>() : nullptr;
            const auto solidB = b ? b.try_as<muxm::SolidColorBrush>() : nullptr;
            return solidA && solidB && solidA.Color() == solidB.Color() && solidA.Opacity() == solidB.Opacity();
        }

        // Elementi dal testo di una scheda nativa fino alla scheda stessa (esclusa).
        std::vector<mux::DependencyObject> ChainToContainer(muxc::TextBlock const& text, muxc::Control const& container)
        {
            std::vector<mux::DependencyObject> chain;
            for (mux::DependencyObject node = text; node && node != container; node = muxm::VisualTreeHelper::GetParent(node))
            {
                chain.push_back(node);
            }
            return chain;
        }

        // Colore effettivo del testo: il primo Foreground impostato risalendo dal testo alla scheda (il
        // testo creato da un ContentPresenter lo eredita, e il suo Foreground può risultare vuoto).
        muxm::Brush EffectiveForeground(muxc::TextBlock const& text, muxc::Control const& container)
        {
            for (auto const& node : ChainToContainer(text, container))
            {
                muxm::Brush brush{ nullptr };
                if (const auto block = node.try_as<muxc::TextBlock>())
                {
                    brush = block.Foreground();
                }
                else if (const auto presenter = node.try_as<muxc::ContentPresenter>())
                {
                    brush = presenter.Foreground();
                }
                else if (const auto control = node.try_as<muxc::Control>())
                {
                    brush = control.Foreground();
                }
                if (brush)
                {
                    return brush;
                }
            }
            return container.Foreground();
        }

        // Colore che il modello di una scheda nativa dà al titolo al passaggio del mouse. I Setter degli
        // stati visivi dei modelli di sistema non si lasciano leggere (errore), quindi la scheda, che
        // nella striscia nascosta è invisibile, viene portata nello stato "PointerOver", letta e subito
        // riportata allo stato di prima, tutto nello stesso passaggio (nulla viene disegnato nel mezzo).
        // Null se lo stato non esiste o non cambia il colore.
        muxm::Brush TitleBrushOnHover(muxc::TabViewItem const& container, muxc::TextBlock const& text, winrt::hstring const& currentState)
        {
            const muxm::Brush normal = EffectiveForeground(text, container);
            if (!mux::VisualStateManager::GoToState(container, L"PointerOver", false))
            {
                return nullptr;
            }
            muxm::Brush hover{ nullptr };
            try
            {
                hover = EffectiveForeground(text, container);
            }
            catch (...)
            {
            }
            mux::VisualStateManager::GoToState(container, currentState.empty() ? winrt::hstring{ L"Normal" } : currentState, false);
            return SameBrush(hover, normal) ? nullptr : hover;
        }

        // Colore di un pennello per il log (#AARRGGBB), o il suo tipo.
        std::wstring BrushText(muxm::Brush const& brush)
        {
            if (!brush)
            {
                return L"-";
            }
            if (const auto solid = brush.try_as<muxm::SolidColorBrush>())
            {
                const auto c = solid.Color();
                wchar_t text[32]{};
                swprintf_s(text, L"#%02X%02X%02X%02X", c.A, c.R, c.G, c.B);
                return text;
            }
            return std::wstring{ winrt::get_class_name(brush) };
        }

        // Iniziale (maiuscola) del titolo per l'icona della scheda: la prima lettera o cifra.
        winrt::hstring InitialOf(std::wstring_view title)
        {
            for (const wchar_t c : title)
            {
                if (IsCharAlphaNumericW(c))
                {
                    wchar_t upper[2]{ c, L'\0' };
                    CharUpperBuffW(upper, 1);
                    return winrt::hstring{ upper, 1 };
                }
            }
            return L"•";
        }

        // Primo elemento (in profondità) che ha un menu contestuale assegnato.
        mux::UIElement FindContextFlyoutOwner(mux::DependencyObject const& root)
        {
            const int count = muxm::VisualTreeHelper::GetChildrenCount(root);
            for (int i = 0; i < count; ++i)
            {
                const auto child = muxm::VisualTreeHelper::GetChild(root, i);
                if (const auto element = child.try_as<mux::UIElement>(); element && element.ContextFlyout())
                {
                    return element;
                }
                if (const auto found = FindContextFlyoutOwner(child))
                {
                    return found;
                }
            }
            return nullptr;
        }

        // "Preme" un pulsante tramite UI Automation: scatena la stessa logica di un clic dell'utente.
        bool InvokeButton(muxc::Button const& button)
        {
            if (!button)
            {
                return false;
            }
            try
            {
                winrt::Microsoft::UI::Xaml::Automation::Peers::ButtonAutomationPeer peer{ button };
                peer.Invoke();
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        // Isola XAML nativa della barra schede: la figlia della finestra con le dimensioni della TabsBar.
        HWND FindNativeTabsIsland(HWND main, winrt::NotepadXamlUI::TabsBar const& tabsBar)
        {
            double scale = 1.0;
            if (const auto xamlRoot = tabsBar.XamlRoot())
            {
                scale = xamlRoot.RasterizationScale();
            }
            const int width = static_cast<int>(std::lround(tabsBar.ActualWidth() * scale));
            const int height = static_cast<int>(std::lround(tabsBar.ActualHeight() * scale));

            HWND best = nullptr;
            int bestScore = INT_MAX;
            for (HWND child = GetWindow(main, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                if (interop::ClassOf(child) != L"Microsoft.UI.Content.DesktopChildSiteBridge" || interop::IsOverlay(child))
                {
                    continue;
                }
                const RECT r = interop::RectInClientOf(child, main);
                const int score = std::abs((r.right - r.left) - width) + std::abs((r.bottom - r.top) - height);
                if (score < bestScore)
                {
                    bestScore = score;
                    best = child;
                }
            }
            return bestScore <= 48 ? best : nullptr;
        }

        UINT DpiOf(HWND hwnd)
        {
            const UINT dpi = GetDpiForWindow(hwnd);
            return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
        }

        winrt::Windows::UI::Color WithAlpha(winrt::Windows::UI::Color color, uint8_t alpha)
        {
            color.A = alpha;
            return color;
        }

        winrt::Windows::UI::Color Shade(winrt::Windows::UI::Color color, double factor)
        {
            const auto scale = [factor](uint8_t channel) { return static_cast<uint8_t>(std::lround(channel * factor)); };
            return { color.A, scale(color.R), scale(color.G), scale(color.B) };
        }

        // Sfondo di un elemento che ne ha uno (pannelli, bordi, controlli, presentatori di contenuto).
        muxm::Brush BackgroundOf(mux::DependencyObject const& node)
        {
            if (const auto panel = node.try_as<muxc::Panel>())
            {
                return panel.Background();
            }
            if (const auto border = node.try_as<muxc::Border>())
            {
                return border.Background();
            }
            if (const auto control = node.try_as<muxc::Control>())
            {
                return control.Background();
            }
            if (const auto presenter = node.try_as<muxc::ContentPresenter>())
            {
                return presenter.Background();
            }
            return nullptr;
        }

        // true se il pennello disegna qualcosa (un colore pieno trasparente no).
        bool IsPainted(muxm::Brush const& brush)
        {
            if (!brush || brush.Opacity() <= 0)
            {
                return false;
            }
            if (const auto solid = brush.try_as<muxm::SolidColorBrush>())
            {
                return solid.Color().A != 0;
            }
            return true; // acrilico, sfumature...
        }

        // Zone vuote della barra dei menu, in px della sua isola: gli spazi tra i comandi (menu, pulsanti, caselle; anche la
        // barra di formattazione o il pulsante dell'account, quando ci sono) larghi almeno kMenuBarMinGapDip, a
        // kMenuBarHolePadDip dai comandi (il bordo del focus da tastiera ne esce un po'), alti quanto l'isola.
        constexpr double kMenuBarHolePadDip = 4.0;
        constexpr double kMenuBarMinGapDip = 16.0;

        bool IsMenuBarCommand(mux::FrameworkElement const& element, double barWidth)
        {
            if (element.try_as<muxc::MenuBar>() || element.try_as<muxc::MenuBarItem>() || element.try_as<muxp::ButtonBase>() ||
                element.try_as<muxc::TextBox>() || element.try_as<muxc::ComboBox>() || element.try_as<muxc::AutoSuggestBox>())
            {
                return true;
            }
            // Altri controlli raggiungibili con il tabulatore (non i contenitori larghi quanto la barra)
            const auto control = element.try_as<muxc::Control>();
            return control && control.IsTabStop() && element.ActualWidth() < barWidth / 2;
        }

        std::vector<RECT> MenuBarHoles(mux::FrameworkElement const& menuBar, SIZE& islandSize)
        {
            std::vector<RECT> holes;
            islandSize = {};
            const auto xamlRoot = menuBar.XamlRoot();
            const auto root = xamlRoot ? xamlRoot.Content().try_as<mux::FrameworkElement>() : nullptr;
            if (!root || root.ActualWidth() <= 0 || root.ActualHeight() <= 0)
            {
                return holes;
            }
            const double scale = xamlRoot.RasterizationScale();
            const double width = root.ActualWidth();
            islandSize = { std::lround(width * scale), std::lround(root.ActualHeight() * scale) };

            std::vector<std::pair<double, double>> spans; // DIP, intervalli orizzontali occupati dai comandi
            std::function<void(mux::DependencyObject const&)> visit = [&](mux::DependencyObject const& node) {
                if (const auto element = node.try_as<mux::FrameworkElement>())
                {
                    if (element.Visibility() != mux::Visibility::Visible || !element.IsHitTestVisible() || element.ActualWidth() <= 0 ||
                        element.ActualHeight() <= 0)
                    {
                        return; // nascosto o trasparente al mouse, figli compresi
                    }
                    if (IsMenuBarCommand(element, width))
                    {
                        const auto bounds = element.TransformToVisual(root).TransformBounds(
                            winrt::Windows::Foundation::Rect{ 0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
                        spans.emplace_back(bounds.X, bounds.X + bounds.Width);
                        return; // i suoi figli stanno dentro
                    }
                }
                const int32_t count = muxm::VisualTreeHelper::GetChildrenCount(node);
                for (int32_t i = 0; i < count; ++i)
                {
                    visit(muxm::VisualTreeHelper::GetChild(node, i));
                }
            };
            visit(root);
            if (spans.empty())
            {
                return holes; // nessun comando riconosciuto: meglio lasciare la barra com'è
            }
            std::sort(spans.begin(), spans.end());
            // In fondo alla barra c'è una linea sottile (#1D1D1D): il buco si ferma 1 DIP sopra, così resta intera (la
            // regione taglia anche il disegno dell'isola).
            const LONG bottom = islandSize.cy - std::max(1L, static_cast<LONG>(std::ceil(scale)));
            const auto addHole = [&](double left, double right) {
                left += kMenuBarHolePadDip;
                right -= kMenuBarHolePadDip;
                if (right - left >= kMenuBarMinGapDip && bottom > 0)
                {
                    holes.push_back(RECT{ static_cast<LONG>(std::ceil(left * scale)), 0, static_cast<LONG>(std::floor(right * scale)), bottom });
                }
            };
            double occupied = 0; // fin dove la barra è occupata (da sinistra)
            for (auto const& [left, right] : spans)
            {
                addHole(occupied, left);
                occupied = std::max(occupied, right);
            }
            addHole(occupied, width);
            return holes;
        }

        // Tipo, nome, dimensioni e colore di sfondo di un elemento (per il log).
        std::wstring Describe(mux::DependencyObject const& node)
        {
            std::wstring text{ winrt::get_class_name(node) };
            if (const auto element = node.try_as<mux::FrameworkElement>())
            {
                if (!element.Name().empty())
                {
                    text += L" #" + std::wstring{ element.Name() };
                }
                wchar_t size[64]{};
                swprintf_s(size, L" [%.0fx%.0f]", element.ActualWidth(), element.ActualHeight());
                text += size;
            }
            if (const auto brush = BackgroundOf(node))
            {
                if (const auto solid = brush.try_as<muxm::SolidColorBrush>())
                {
                    const auto c = solid.Color();
                    wchar_t color[32]{};
                    swprintf_s(color, L" #%02X%02X%02X%02X", c.A, c.R, c.G, c.B);
                    text += color;
                }
                else
                {
                    text += L" " + std::wstring{ winrt::get_class_name(brush) };
                }
            }
            return text;
        }
    }

    VerticalTabs::VerticalTabs(winrt::NotepadXamlUI::TabsBar const& tabsBar) :
        m_tabsBar(winrt::make_weak(tabsBar))
    {
    }

    VerticalTabs::~VerticalTabs()
    {
        Detach(false);
    }

    // ---------------------------------------------------------------------------------------------
    // Ciclo di vita
    // ---------------------------------------------------------------------------------------------

    void VerticalTabs::Apply(Settings const& notified)
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return;
        }
        if (!m_attached && !Attach(tabsBar))
        {
            return;
        }

        // Le modifiche a settings.ini arrivano a raffiche e la lettura del thread di controllo può essere
        // già superata da una scelta fatta qui nel frattempo (es. comprimi e subito espandi): vale il
        // contenuto del file adesso, altrimenti un valore intermedio verrebbe riapplicato per ultimo.
        Settings settings = Settings::Load();
        settings.dumpTreeOnAttach = notified.dumpTreeOnAttach;

        const bool geometryChanged = settings.paneWidth != m_settings.paneWidth || settings.collapsed != m_settings.collapsed;
        m_settings = settings;
        ApplyAppearance();

        // Gate per-finestra (sviluppo): se esiste dev-windows.txt agiamo solo sulle finestre elencate.
        const bool wanted = settings.verticalTabs && WindowApplyAllowed(m_main);
        if (wanted && !m_enabled)
        {
            Enable();
        }
        else if (!wanted && m_enabled)
        {
            Disable();
        }
        else if (wanted && geometryChanged)
        {
            m_layout.SetActive(true, LayoutSettings());
        }
    }

    Settings VerticalTabs::LayoutSettings() const
    {
        Settings layout = m_settings;
        if (layout.collapsed)
        {
            layout.paneWidth = Settings::kCollapsedPaneWidth;
        }
        return layout;
    }

    bool VerticalTabs::ReadNativeTitleStyle()
    {
        const auto tabsBar = m_tabsBar.get();
        const auto tabView = NativeTabView();
        if (!tabsBar || !tabView || !m_items)
        {
            return false;
        }
        // Schede già realizzate dalla striscia (niente scorrimenti per realizzarne altre); il titolo è il
        // TextBlock con lo stesso testo. Valgono solo schede "a riposo" (stato Normal o Selected): il
        // passaggio del mouse o la pressione, ad esempio col menu contestuale aperto, ne cambiano il colore.
        TitleStyle style = m_titleStyle;
        bool haveNormal = false;
        bool haveSelected = false;
        const int selectedIndex = tabsBar.SelectedIndex();
        for (uint32_t i = 0; i < m_items.Size() && !(haveNormal && haveSelected); ++i)
        {
            const bool isSelected = static_cast<int>(i) == selectedIndex;
            if (isSelected ? haveSelected : haveNormal)
            {
                continue;
            }
            try
            {
                const winrt::hstring title = m_items.GetAt(i).Title();
                const auto container = tabView.ContainerFromIndex(static_cast<int32_t>(i)).try_as<muxc::TabViewItem>();
                const auto text = container && !title.empty() ? FindTextBlock(container, title) : nullptr;
                if (!text)
                {
                    continue;
                }
                const auto states = CommonStates(container);
                const auto current = states ? states.CurrentState() : nullptr;
                const winrt::hstring state = current ? current.Name() : winrt::hstring{};
                if (!state.empty() && state != (isSelected ? L"Selected" : L"Normal"))
                {
                    continue;
                }
                const auto brush = EffectiveForeground(text, container);
                if (isSelected)
                {
                    style.selected = brush;
                    style.selectedWeight = text.FontWeight();
                    haveSelected = true;
                    continue;
                }
                // Il colore al passaggio del mouse si rilegge solo se è cambiato quello normale (tema).
                if (!m_titleStyleRead || !SameBrush(brush, m_titleStyle.normal))
                {
                    style.hover = TitleBrushOnHover(container, text, state);
                }
                style.normal = brush;
                style.normalWeight = text.FontWeight();
                style.size = text.FontSize();
                style.family = text.FontFamily();
                haveNormal = true;
            }
            catch (...)
            {
                // Una scheda non leggibile non deve fermare nulla: si prova con la successiva.
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true))
                {
                    NPX_LOG(L"VerticalTabs: scheda nativa %u non leggibile (0x%08X).", i, static_cast<unsigned>(winrt::to_hresult()));
                }
            }
        }
        if (!haveNormal && !haveSelected)
        {
            return false;
        }
        m_titleStyleRead = m_titleStyleRead || (haveNormal && haveSelected);

        const auto& old = m_titleStyle;
        const bool changed = style.size != old.size || style.normalWeight.Weight != old.normalWeight.Weight ||
                             style.selectedWeight.Weight != old.selectedWeight.Weight || !SameBrush(style.normal, old.normal) ||
                             !SameBrush(style.hover, old.hover) || !SameBrush(style.selected, old.selected) ||
                             (style.family ? style.family.Source() : winrt::hstring{}) != (old.family ? old.family.Source() : winrt::hstring{});
        if (!changed)
        {
            return false;
        }
        m_titleStyle = style;
        NPX_LOG(L"VerticalTabs: titoli come le schede native: %.1f DIP %s; normale peso %u %s, al passaggio del mouse %s, attiva peso %u %s.",
                style.size, style.family ? style.family.Source().c_str() : L"(predefinito)", static_cast<unsigned>(style.normalWeight.Weight),
                BrushText(style.normal).c_str(), style.hover ? BrushText(style.hover).c_str() : L"invariato",
                static_cast<unsigned>(style.selectedWeight.Weight), BrushText(style.selected).c_str());
        for (auto const& row : m_rows)
        {
            UpdateRow(*row);
        }
        return true;
    }

    void VerticalTabs::OnTimer()
    {
        if (m_enabled)
        {
            m_layout.Check();
            // Aspetto dei titoli: finché non è letto (schede native non ancora realizzate) e poi ogni tanto,
            // così segue anche i cambi di tema.
            if (!m_titleStyleRead || ++m_titleStyleTicks % kTitleStyleEveryTicks == 0)
            {
                try
                {
                    ReadNativeTitleStyle();
                }
                catch (...)
                {
                    NPX_VLOG(L"VerticalTabs: aspetto dei titoli non letto (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                }
            }
            try
            {
                UpdateMenuBarTint(); // la barra dei menu può nascere dopo il pannello o essere ricreata
            }
            catch (...)
            {
                NPX_LOG(L"VerticalTabs: controllo della barra dei menu non riuscito (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            }
            for (const auto& fit : { m_vScrollFit.get(), m_hScrollFit.get() })
            {
                if (fit)
                {
                    fit->Refresh(); // barra ricreata o misura ricalcolata dal Blocco note
                }
            }
        }
    }

    void VerticalTabs::Detach(bool restore)
    {
        if (m_enabled)
        {
            if (restore)
            {
                Disable();
            }
            m_enabled = false;
        }
        if (m_autoScrollTimer)
        {
            m_autoScrollTimer.Stop();
            m_autoScrollTimer = nullptr;
        }
        m_dragging = false;
        m_dragPending = false;
        m_flyoutClosedRevoker.revoke();
        RemoveColorMenu();
        // (con restore=true l'ha già fermata Disable; altrimenti la finestra si sta chiudendo)
        m_menuBarStoryboard = nullptr;
        m_menuBar = nullptr;
        m_menuBarLayoutRevoker.revoke();
        m_layout.onPanelGeometry = nullptr;
        m_layout.onScrollBarsChange = nullptr;
        for (auto* fit : { &m_vScrollFit, &m_hScrollFit })
        {
            if (*fit)
            {
                // Funzione spenta: torna la misura del Blocco note e si stacca la callback; a finestra in chiusura niente XAML.
                if (restore)
                {
                    (*fit)->Reset();
                }
                else
                {
                    (*fit)->Abandon();
                }
                fit->reset();
            }
        }
        m_layout.Detach();
        m_captionPx = 0;
        m_contentTopPx = 0;
        m_regionCaptionPx = 0;
        m_regionKeep.clear();
        m_nativeSelectionRevoker.revoke();
        m_themeRevoker.revoke();
        ClearRows();
        if (m_items && m_itemsToken)
        {
            try
            {
                m_items.VectorChanged(m_itemsToken);
            }
            catch (...)
            {
            }
        }
        m_items = nullptr;
        m_itemsToken = {};
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
        m_attached = false;
    }

    bool VerticalTabs::Attach(winrt::NotepadXamlUI::TabsBar const& tabsBar)
    {
        // 1) Finestra top-level a cui appartiene questa TabsBar (mappatura esatta, anche con più finestre).
        HWND main = nullptr;
        try
        {
            if (const auto xamlRoot = tabsBar.XamlRoot())
            {
                if (const auto environment = xamlRoot.ContentIslandEnvironment())
                {
                    main = interop::HwndFromWindowId(environment.AppWindowId());
                }
            }
        }
        catch (...)
        {
        }
        if (main)
        {
            main = GetAncestor(main, GA_ROOT);
        }
        if (!main || !IsWindow(main))
        {
            NPX_LOG(L"VerticalTabs: finestra principale non trovata, riprovo al prossimo aggiornamento.");
            return false;
        }
        m_main = main;

        // 2) Subclass della finestra: serve anche ad accorgersi in modo affidabile della sua chiusura
        //    (la notifica XAML di rimozione non sempre arriva prima che il thread della finestra termini).
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        m_layout.onMainDestroyed = [weak] {
            if (const auto self = weak.lock())
            {
                self->Detach(false);
                if (const auto callback = self->onWindowClosed)
                {
                    callback();
                }
            }
        };
        // Barre di scorrimento dell'editor adattate alle loro isole spostate dal layout (vedi ScrollBarFit.h): la verticale
        // allungata con l'editor salito, l'orizzontale accorciata della larghezza della colonna.
        m_vScrollFit = std::make_unique<ScrollBarFit>(m_main, ScrollBarFit::Axis::Vertical);
        m_vScrollFit->scrollViewersProvider = scrollViewersProvider;
        m_hScrollFit = std::make_unique<ScrollBarFit>(m_main, ScrollBarFit::Axis::Horizontal);
        m_hScrollFit->scrollViewersProvider = scrollViewersProvider;
        m_layout.onScrollBarsChange = [weak](int raisePx, int shrinkPx) {
            if (const auto self = weak.lock())
            {
                if (self->m_vScrollFit)
                {
                    self->m_vScrollFit->SetDelta(raisePx);
                }
                if (self->m_hScrollFit)
                {
                    self->m_hScrollFit->SetDelta(-shrinkPx);
                }
            }
        };
        m_layout.Attach(m_main);

        // 3) Eventi nativi: selezione (Ctrl+Tab, apertura file...) e tema.
        m_nativeSelectionRevoker = tabsBar.SelectionChanged(winrt::auto_revoke, [weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->SyncSelectionFromNative();
            }
        });
        m_themeRevoker = tabsBar.ActualThemeChanged(winrt::auto_revoke, [weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->ApplyAppearance();
                self->m_layout.Buttons().ThemeChanged(); // pulsanti della finestra nel tema nuovo del Blocco note
                // I colori dei titoli si rileggono dalle schede native quando queste hanno preso il nuovo tema.
                if (const auto bar = self->m_tabsBar.get())
                {
                    bar.DispatcherQueue().TryEnqueue(DispatcherQueuePriority::Low, [weak] {
                        if (const auto later = weak.lock(); later && later->m_enabled)
                        {
                            try
                            {
                                later->ReadNativeTitleStyle();
                            }
                            catch (...)
                            {
                            }
                        }
                    });
                }
            }
        });

        m_attached = true;
        return true;
    }

    // La nostra isola XAML si crea solo alla prima attivazione: con la feature spenta la mod non
    // aggiunge nulla all'interfaccia del Blocco note.
    bool VerticalTabs::EnsurePanel()
    {
        if (m_source)
        {
            return true;
        }
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar || !m_main || !IsWindow(m_main))
        {
            return false;
        }

        // Isola nativa della barra schede (prima di creare la nostra, che altrimenti la confonderebbe).
        const HWND nativeTabs = FindNativeTabsIsland(m_main, tabsBar);

        try
        {
            BuildPanel();
            m_source = winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource();
            m_source.Initialize(interop::WindowIdFromHwnd(m_main));
            m_source.Content(m_root);
            ApplyBackdrop();
            const auto bridge = m_source.SiteBridge();
            m_panelHwnd = interop::HwndFromWindowId(bridge.WindowId());
            bridge.Hide();
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: impossibile creare l'isola XAML del pannello (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
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
            return false;
        }

        // Geometria: il layout decide quando il pannello è visibile.
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        m_layout.onPanelVisibility = [weak](bool shown) {
            const auto self = weak.lock();
            // Isola già distrutta (finestra in chiusura): Show/Hide su un ponte chiuso è un accesso non valido.
            if (!self || !self->m_source || !self->m_panelHwnd || !IsWindow(self->m_panelHwnd))
            {
                return;
            }
            try
            {
                const auto bridge = self->m_source.SiteBridge();
                shown ? bridge.Show() : bridge.Hide();
            }
            catch (...)
            {
            }
        };
        m_layout.onPanelGeometry = [weak](int captionPx, int contentTopPx) {
            if (const auto self = weak.lock())
            {
                try
                {
                    self->OnPanelGeometry(captionPx, contentTopPx);
                }
                catch (...)
                {
                    NPX_LOG(L"VerticalTabs: geometria del pannello non applicata (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                }
            }
        };
        // Fascia del titolo in cima alla colonna: la regione dell'isola si aggiorna dopo ogni layout (pulsanti
        // spostati, compresso/espanso), solo se cambia.
        m_root.LayoutUpdated([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                try
                {
                    self->UpdateCaptionRegion();
                }
                catch (...)
                {
                }
            }
        });
        m_layout.SetManagedWindows(nativeTabs, m_panelHwnd);
        ApplyAppearance();

        NPX_LOG(L"VerticalTabs: pannello creato (finestra %llx, isola schede nativa %llx, pannello %llx).",
                reinterpret_cast<uintptr_t>(m_main), reinterpret_cast<uintptr_t>(nativeTabs), reinterpret_cast<uintptr_t>(m_panelHwnd));
        return true;
    }

    void VerticalTabs::BuildPanel()
    {
        // Stile dei contenitori basato su quello Fluent di WinUI e sfondo opaco del tema; se una chiave
        // non esistesse (versioni diverse delle risorse) si ripiega per gradi.
        const auto build = [](std::wstring_view basedOn, std::wstring_view background) {
            std::wstring xaml = kPanelXaml;
            const auto replace = [&xaml](std::wstring_view token, std::wstring_view value) {
                xaml.replace(xaml.find(token), token.size(), value);
            };
            replace(L"%BACKGROUND%", background);
            replace(L"%BASEDON%", basedOn);
            return xaml;
        };
        const std::wstring_view basedOn = LR"(BasedOn="{StaticResource DefaultListViewItemStyle}")";
        const std::wstring_view solidBase = LR"(Background="{ThemeResource SolidBackgroundFillColorBaseBrush}")";
        const std::wstring_view tertiary = LR"(Background="{ThemeResource SolidBackgroundFillColorTertiaryBrush}")";
        for (auto const& [style, background] : { std::pair{ basedOn, solidBase }, std::pair{ std::wstring_view{}, solidBase },
                                                 std::pair{ std::wstring_view{}, tertiary } })
        {
            try
            {
                m_root = LoadXaml(build(style, background)).as<muxc::Grid>();
                break;
            }
            catch (...)
            {
            }
        }
        if (!m_root)
        {
            throw winrt::hresult_error(E_FAIL, L"XAML del pannello non caricabile");
        }

        m_header = FindPart<muxc::Grid>(m_root, L"Header");
        m_toggle = FindPart<muxc::Button>(m_root, L"Toggle");
        m_toggleIcon = FindPart<muxc::FontIcon>(m_root, L"ToggleIcon");
        m_body = FindPart<muxc::Grid>(m_root, L"Body");
        m_list = FindPart<muxc::ListView>(m_root, L"List");
        m_newTab = FindPart<muxc::Button>(m_root, L"NewTab");
        m_separator = FindPart<muxc::Border>(m_root, L"Separator");
        m_separatorActive = FindPart<muxc::Border>(m_root, L"SeparatorActive");
        m_splitter = FindPart<muxc::Border>(m_root, L"Splitter");

        m_visible = winrt::single_threaded_observable_vector<IInspectable>();
        m_list.ItemsSource(m_visible);

        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        m_list.SelectionChanged([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->OnListSelectionChanged();
            }
        });
        m_list.ItemClick([weak](auto&&, auto&&) {
            // Clic col mouse: dopo il cambio di scheda si torna a scrivere nell'editor.
            if (const auto self = weak.lock())
            {
                self->FocusEditor();
            }
        });
        m_list.KeyDown([weak](auto&&, muxi::KeyRoutedEventArgs const& args) {
            const auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (args.Key() == VirtualKey::Escape && self->m_dragging)
            {
                self->EndDrag(false); // annulla il trascinamento
                args.Handled(true);
            }
            else if (args.Key() == VirtualKey::Enter)
            {
                self->FocusEditor();
                args.Handled(true);
            }
            else if (args.Key() == VirtualKey::Delete)
            {
                if (const Row* row = self->FindRow(self->m_list.SelectedItem()))
                {
                    self->CloseTab(row->tabId);
                    args.Handled(true);
                }
            }
        });

        // Riordino con trascinamento (come nella barra orizzontale). I gestori ricevono anche gli eventi
        // già gestiti dalle righe: la ListView continua a occuparsi di clic e selezione.
        const auto addPointerHandler = [this, weak](mux::RoutedEvent const& routedEvent, auto handler) {
            m_list.AddHandler(routedEvent, winrt::box_value(muxi::PointerEventHandler([weak, handler](IInspectable const&, muxi::PointerRoutedEventArgs const& args) {
                                  if (const auto self = weak.lock())
                                  {
                                      handler(*self, args);
                                  }
                              })),
                              true);
        };
        addPointerHandler(mux::UIElement::PointerPressedEvent(),
                          [](VerticalTabs& self, muxi::PointerRoutedEventArgs const& args) { self.OnListPointerPressed(args); });
        addPointerHandler(mux::UIElement::PointerMovedEvent(),
                          [](VerticalTabs& self, muxi::PointerRoutedEventArgs const& args) { self.OnListPointerMoved(args); });
        addPointerHandler(mux::UIElement::PointerReleasedEvent(),
                          [](VerticalTabs& self, muxi::PointerRoutedEventArgs const& args) { self.OnListPointerReleased(args); });
        addPointerHandler(mux::UIElement::PointerCaptureLostEvent(), [](VerticalTabs& self, muxi::PointerRoutedEventArgs const&) {
            // Cattura persa senza rilascio (es. cambio di finestra): il trascinamento si annulla.
            if (self.m_dragging)
            {
                self.EndDrag(false);
            }
        });

        m_toggle.Click([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->SetCollapsed(!self->m_settings.collapsed);
            }
        });

        m_newTab.Click([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->NewTab();
            }
        });
        m_body.SizeChanged([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->UpdateListHeight();
            }
        });
        // Il modello della ListView (con il suo ScrollViewer) esiste solo una volta caricata.
        m_list.Loaded([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->BlockHorizontalWheel();
            }
        });

        // Divisore: cursore ↔ e trascinamento per cambiare la larghezza. ProtectedCursor è "protected"
        // in WinUI, ma è raggiungibile tramite l'interfaccia IUIElementProtected di ogni UIElement.
        try
        {
            if (const auto protectedSplitter = m_splitter.try_as<mux::IUIElementProtected>())
            {
                protectedSplitter.ProtectedCursor(
                    winrt::Microsoft::UI::Input::InputSystemCursor::Create(winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast));
            }
            else
            {
                NPX_LOG(L"VerticalTabs: cursore del divisore non impostabile (IUIElementProtected non disponibile).");
            }
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: cursore del divisore non impostabile (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }
        m_splitter.PointerEntered([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->m_splitterHovered = true;
                self->SetSplitterHighlight(true);
            }
        });
        m_splitter.PointerExited([weak](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->m_splitterHovered = false;
                if (!self->m_resizing)
                {
                    self->SetSplitterHighlight(false);
                }
            }
        });
        m_splitter.PointerPressed([weak](auto&&, muxi::PointerRoutedEventArgs const& args) {
            const auto self = weak.lock();
            if (!self || !args.GetCurrentPoint(self->m_splitter).Properties().IsLeftButtonPressed())
            {
                return;
            }
            // Larghezza e punto afferrato al momento della presa: il bordo segue il mouse senza scatti.
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(self->m_main, &cursor);
            const RECT panel = interop::RectInClientOf(self->m_panelHwnd, self->m_main);
            const int widthPx = panel.right - panel.left;
            self->m_grabOffsetPx = widthPx - cursor.x; // il bordo destro della colonna segue il cursore
            self->m_resizing = self->m_splitter.CapturePointer(args.Pointer());
            self->SetSplitterHighlight(true);
            args.Handled(true);
        });
        m_splitter.PointerMoved([weak](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (const auto self = weak.lock(); self && self->m_resizing)
            {
                self->ResizeFromCursor();
                args.Handled(true);
            }
        });
        m_splitter.PointerReleased([weak](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (const auto self = weak.lock(); self && self->m_resizing)
            {
                self->m_resizing = false;
                self->m_splitter.ReleasePointerCapture(args.Pointer());
                self->PersistPaneWidth();
                self->SetSplitterHighlight(self->m_splitterHovered);
                args.Handled(true);
            }
        });
        m_splitter.PointerCaptureLost([weak](auto&&, auto&&) {
            if (const auto self = weak.lock(); self && self->m_resizing)
            {
                self->m_resizing = false;
                self->PersistPaneWidth();
                self->SetSplitterHighlight(self->m_splitterHovered);
            }
        });
        m_splitter.DoubleTapped([weak](auto&&, muxi::DoubleTappedRoutedEventArgs const& args) {
            if (const auto self = weak.lock())
            {
                // Il secondo clic ha già avviato un ridimensionamento: lo si chiude, altrimenti il movimento
                // generato dal pannello che si restringe sotto il cursore ripristinerebbe la larghezza.
                self->m_resizing = false;
                self->m_splitter.ReleasePointerCaptures();
                self->ApplyPaneWidth(Settings{}.paneWidth);
                self->PersistPaneWidth();
                self->SetSplitterHighlight(self->m_splitterHovered);
                args.Handled(true);
            }
        });
    }

    void VerticalTabs::ApplyBackdrop()
    {
        // Lo stesso materiale della barra del titolo del Blocco note (Mica Alt, "finestra a schede"),
        // reso dentro la nostra isola: è opaco (niente immagini fantasma dell'editor) e, come quello di
        // sistema, diventa il colore pieno di riserva quando la trasparenza di Windows è spenta.
        // Se non è disponibile resta il pennello del XAML, che è proprio quel colore pieno.
        try
        {
            muxm::MicaBackdrop mica;
            mica.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
            m_source.SystemBackdrop(mica);
            m_root.Background(muxm::SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent()));
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: Mica non disponibile per il pannello (0x%08X): colore pieno.", static_cast<unsigned>(winrt::to_hresult()));
        }
    }

    void VerticalTabs::ApplyAppearance()
    {
        if (!m_root)
        {
            return;
        }
        if (const auto tabsBar = m_tabsBar.get())
        {
            m_root.RequestedTheme(tabsBar.ActualTheme());
        }
        const bool compact = m_settings.collapsed;

        // Intestazione: il pulsante a sinistra da espanso, centrato da compresso. La sua icona è un pannello che
        // si chiude (o si apre) verso il bordo della finestra.
        m_header.HorizontalAlignment(compact ? mux::HorizontalAlignment::Center : mux::HorizontalAlignment::Stretch);
        ApplyHeaderLayout(compact);
        m_toggleIcon.Glyph(compact ? L"\xEA5B" : L"\xEA49"); // Segoe Fluent Icons: pannello con la barra a sinistra, che si apre o si chiude
        const winrt::hstring toggleName = compact ? L"Espandi schede" : L"Comprimi schede";
        mux::Automation::AutomationProperties::SetName(m_toggle, toggleName);
        muxc::ToolTipService::SetToolTip(m_toggle, winrt::box_value(toggleName));

        // Separatore e divisore sul bordo destro, verso l'editor; l'elenco lascia libera la fascia del divisore
        // (così la sua barra di scorrimento non si sovrappone alla zona di trascinamento). Da compresso
        // la larghezza è fissa: niente divisore.
        m_splitter.Visibility(compact ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        if (compact)
        {
            SetSplitterHighlight(false);
        }
        const double gap = compact ? 0.0 : kSplitterGap;
        m_list.Margin(mux::Thickness{ 0, 0, gap, 0 });
        // Il "+" (solo icona, centrato) è allineato agli sfondi delle righe.
        m_newTab.Margin(mux::Thickness{ kRowInset, kNewTabTopMargin, kRowInset + gap, kNewTabBottomMargin });

        // Righe: aspetto compresso/espanso e colori dell'icona (che dipendono dal tema).
        m_compactVisual = compact;
        for (auto const& row : m_rows)
        {
            UpdateRow(*row);
        }
    }

    double VerticalTabs::PanelScale() const
    {
        if (m_root)
        {
            if (const auto root = m_root.XamlRoot())
            {
                return root.RasterizationScale();
            }
        }
        const UINT dpi = m_main ? GetDpiForWindow(m_main) : 0;
        return (dpi ? dpi : 96) / 96.0;
    }

    void VerticalTabs::ApplyHeaderLayout(bool compact)
    {
        if (!m_header || !m_root)
        {
            return;
        }
        const double scale = PanelScale();
        const double caption = m_captionPx / scale;
        const double contentTop = m_contentTopPx / scale;
        const auto rows = m_header.RowDefinitions();
        if (caption > 0 && rows.Size() > 0)
        {
            // In cima alla finestra: l'intestazione è la fascia del titolo, con il pulsante centrato accanto a quelli
            // della finestra.
            const double button = std::clamp(caption - 2 * kCaptionButtonInset, 24.0, kHeaderButtonHeight);
            rows.GetAt(0).Height(mux::GridLength{ caption, mux::GridUnitType::Pixel });
            m_header.Margin(compact ? mux::Thickness{ 0, 0, 0, 2 } : mux::Thickness{ 6, 0, 6, 0 });
            m_toggle.Height(button);
        }
        else
        {
            if (rows.Size() > 0)
            {
                rows.GetAt(0).Height(mux::GridLengthHelper::Auto());
            }
            m_header.Margin(compact ? mux::Thickness{ 0, 6, 0, 2 } : mux::Thickness{ 6, 6, 6, 2 });
            m_toggle.Height(kHeaderButtonHeight);
        }
        // Separatore e divisore solo accanto all'editor: sopra, colonna e barra dei menu sono la stessa superficie
        // (la Mica della barra del titolo).
        const mux::Thickness lines{ 0, contentTop, 0, 0 };
        m_separator.Margin(lines);
        m_separatorActive.Margin(lines);
        m_splitter.Margin(lines);
    }

    void VerticalTabs::OnPanelGeometry(int captionPx, int contentTopPx)
    {
        m_captionPx = std::max(0, captionPx);
        m_contentTopPx = std::max(0, contentTopPx);
        ApplyHeaderLayout(m_settings.collapsed);
        UpdateCaptionRegion(); // subito (es. barra dei menu non trovata); dopo il layout la ricalcola LayoutUpdated
    }

    void VerticalTabs::UpdateCaptionRegion()
    {
        if (!m_panelHwnd || !IsWindow(m_panelHwnd) || !m_root)
        {
            return;
        }
        // Il mouse delle isole XAML non passa dall'hit test di Windows (WM_NCHITTEST): per lasciare la fascia del
        // titolo alla finestra principale l'isola non deve proprio esserci. La sua regione esclude la fascia, salvo
        // il pulsante comprimi/espandi; sotto c'è la finestra principale, che lì risponde come la sua barra del
        // titolo (WindowLayout). Si vede lo stesso materiale (la Mica della finestra).
        std::vector<RECT> keep;
        if (m_captionPx > 0)
        {
            const double scale = PanelScale();
            const mux::FrameworkElement interactive[] = { m_toggle };
            for (auto const& element : interactive)
            {
                if (!element || element.Visibility() != mux::Visibility::Visible || element.ActualWidth() <= 0 || element.ActualHeight() <= 0)
                {
                    continue;
                }
                const auto bounds = element.TransformToVisual(m_root).TransformBounds(
                    winrt::Windows::Foundation::Rect{ 0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
                RECT rect{ static_cast<LONG>(std::floor(bounds.X * scale)), static_cast<LONG>(std::floor(bounds.Y * scale)),
                           static_cast<LONG>(std::ceil((bounds.X + bounds.Width) * scale)), static_cast<LONG>(std::ceil((bounds.Y + bounds.Height) * scale)) };
                // Fino al fondo della fascia: sotto il pulsante "comprimi" il Blocco note disegna la sua icona, che
                // dal buco della regione si vedrebbe spuntare (la regione taglia anche ciò che l'isola disegna).
                rect.bottom = static_cast<LONG>(m_captionPx);
                if (rect.right > rect.left && rect.bottom > rect.top)
                {
                    keep.push_back(rect);
                }
            }
        }
        const bool same = m_regionCaptionPx == m_captionPx && keep.size() == m_regionKeep.size() &&
                          std::equal(keep.begin(), keep.end(), m_regionKeep.begin(), [](RECT const& a, RECT const& b) { return EqualRect(&a, &b) != FALSE; });
        if (same)
        {
            return;
        }
        m_regionCaptionPx = m_captionPx;
        m_regionKeep = keep;
        if (m_captionPx <= 0)
        {
            SetWindowRgn(m_panelHwnd, nullptr, TRUE);
            return;
        }
        constexpr int kFar = 32767; // a destra e in basso abbastanza per ogni larghezza e altezza futura
        HRGN region = CreateRectRgn(0, m_captionPx, kFar, kFar);
        for (RECT const& rect : keep)
        {
            if (HRGN button = CreateRectRgn(rect.left, rect.top, rect.right, rect.bottom))
            {
                CombineRgn(region, region, button, RGN_OR);
                DeleteObject(button);
            }
        }
        if (!region || !SetWindowRgn(m_panelHwnd, region, TRUE))
        {
            if (region)
            {
                DeleteObject(region);
            }
            m_regionKeep.clear();
            m_regionCaptionPx = -1; // si riprova al prossimo layout
        }
    }

    void VerticalTabs::BlockHorizontalWheel()
    {
        if (m_horizontalWheelBlocked || !m_list)
        {
            return;
        }
        // Lo ScrollViewer di WinUI, se non può scorrere in orizzontale, converte la rotella orizzontale
        // (anche lo scorrimento a due dita del touchpad, che arriva così) in scorrimento verticale.
        // L'elenco deve scorrere solo in verticale: l'evento si ferma sul contenitore delle righe,
        // prima di raggiungere lo ScrollViewer. La rotella verticale passa come sempre.
        const auto presenter = FindDescendant<muxc::ItemsPresenter>(m_list);
        if (!presenter)
        {
            NPX_LOG(L"VerticalTabs: contenitore delle righe non trovato, rotella orizzontale non filtrata.");
            return;
        }
        presenter.AddHandler(mux::UIElement::PointerWheelChangedEvent(),
                             winrt::box_value(muxi::PointerEventHandler([](IInspectable const& sender, muxi::PointerRoutedEventArgs const& args) {
                                 const auto element = sender.try_as<mux::UIElement>();
                                 if (element && args.GetCurrentPoint(element).Properties().IsHorizontalMouseWheel())
                                 {
                                     args.Handled(true);
                                 }
                             })),
                             false);
        m_horizontalWheelBlocked = true;
    }

    void VerticalTabs::UpdateListHeight()
    {
        if (!m_body || !m_list)
        {
            return;
        }
        // Finché le righe ci stanno l'elenco è alto quanto loro e il "+" le segue; oltre, l'elenco
        // scorre e il "+" resta visibile in fondo al pannello.
        m_list.MaxHeight(std::max(0.0, m_body.ActualHeight() - kNewTabTopMargin - kNewTabHeight - kNewTabBottomMargin));
    }

    mux::FrameworkElement VerticalTabs::FindMenuBar() const
    {
        if (!menuBarsProvider || !m_main)
        {
            return nullptr;
        }
        // Ogni isola sa a quale finestra top-level appartiene (come la TabsBar in Attach).
        for (auto const& candidate : menuBarsProvider())
        {
            try
            {
                const auto xamlRoot = candidate.XamlRoot();
                if (!xamlRoot)
                {
                    continue;
                }
                const auto environment = xamlRoot.ContentIslandEnvironment();
                if (!environment)
                {
                    continue;
                }
                const HWND window = interop::HwndFromWindowId(environment.AppWindowId());
                if (window && GetAncestor(window, GA_ROOT) == m_main)
                {
                    return candidate;
                }
            }
            catch (...)
            {
            }
        }
        return nullptr;
    }

    void VerticalTabs::UpdateMenuBarTint()
    {
        if (!m_enabled)
        {
            RemoveMenuBarTint();
            return;
        }
        if (const auto current = m_menuBar.get(); current && m_menuBarStoryboard && current.XamlRoot())
        {
            // Già trasparente (e zone vuote seguite da LayoutUpdated): se qualcosa ha fermato l'animazione, si riprende.
            try
            {
                if (m_menuBarStoryboard.GetCurrentState() == muxa::ClockState::Stopped)
                {
                    m_menuBarStoryboard.Begin();
                }
            }
            catch (...)
            {
            }
            return;
        }
        RemoveMenuBarTint(); // barra sostituita (o mai trovata finora): si riparte da capo

        const auto menuBar = FindMenuBar();
        if (!menuBar)
        {
            if (!m_menuBarMissingLogged)
            {
                NPX_VLOG(L"VerticalTabs: barra dei menu della finestra non ancora trovata.");
                m_menuBarMissingLogged = true;
            }
            return;
        }

        // A dare alla riga dei menu il colore dell'editor sono gli sfondi lungo la catena che va dalla
        // barra dei menu vera e propria (MenuBar di WinUI) alla radice dell'isola. Gli elementi laterali
        // (barra di formattazione, pulsanti, avatar) non ne fanno parte e restano come sono.
        mux::DependencyObject start = FindDescendant<muxc::MenuBar>(menuBar);
        if (!start)
        {
            start = menuBar;
        }
        std::vector<mux::DependencyObject> targets;
        for (auto node = start; node; node = muxm::VisualTreeHelper::GetParent(node))
        {
            if (IsPainted(BackgroundOf(node)))
            {
                targets.push_back(node);
            }
        }

        muxa::Storyboard storyboard;
        try
        {
            const muxm::SolidColorBrush clear{ winrt::Microsoft::UI::Colors::Transparent() };
            for (auto const& target : targets)
            {
                NPX_LOG(L"VerticalTabs: barra dei menu, sfondo reso trasparente: %s", Describe(target).c_str());
                muxa::DiscreteObjectKeyFrame frame;
                frame.Value(clear);
                muxa::ObjectAnimationUsingKeyFrames animation;
                animation.KeyFrames().Append(frame);
                muxa::Storyboard::SetTarget(animation, target);
                muxa::Storyboard::SetTargetProperty(animation, L"Background");
                storyboard.Children().Append(animation);
            }
            if (targets.empty())
            {
                NPX_LOG(L"VerticalTabs: barra dei menu senza sfondi da togliere.");
            }
            storyboard.Begin();
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: sfondo della barra dei menu non modificabile (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }
        // Anche se non è riuscita: niente nuovi tentativi a ogni controllo, e al ripristino si ferma.
        m_menuBarStoryboard = storyboard;
        m_menuBar = winrt::make_weak(menuBar);

        // Zone vuote della barra (trascinabili con la riga del titolo nascosta): si ricalcolano dopo ogni layout della sua
        // isola (finestra ridimensionata, pannello compresso, barra di formattazione che compare); WindowLayout applica
        // la regione solo se cambiano.
        try
        {
            const auto xamlRoot = menuBar.XamlRoot();
            if (const auto root = xamlRoot ? xamlRoot.Content().try_as<mux::FrameworkElement>() : nullptr)
            {
                const std::weak_ptr<VerticalTabs> weak = weak_from_this();
                m_menuBarLayoutRevoker = root.LayoutUpdated(winrt::auto_revoke, [weak](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        try
                        {
                            self->UpdateMenuBarHoles();
                        }
                        catch (...)
                        {
                        }
                    }
                });
            }
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: layout della barra dei menu non seguito (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }
        UpdateMenuBarHoles();
    }

    void VerticalTabs::UpdateMenuBarHoles()
    {
        std::vector<RECT> holes;
        SIZE islandSize{};
        if (m_enabled)
        {
            if (const auto menuBar = m_menuBar.get())
            {
                holes = MenuBarHoles(menuBar, islandSize);
            }
        }
        m_layout.SetMenuBarHoles(std::move(holes), islandSize);
    }

    void VerticalTabs::RemoveMenuBarTint()
    {
        m_menuBarLayoutRevoker.revoke();
        m_layout.SetMenuBarHoles({}, {}); // barra dei menu di nuovo intera
        if (m_menuBarStoryboard)
        {
            try
            {
                m_menuBarStoryboard.Stop(); // torna lo sfondo del Blocco note
            }
            catch (...)
            {
            }
            m_menuBarStoryboard = nullptr;
            NPX_LOG(L"VerticalTabs: barra dei menu di nuovo con il suo sfondo.");
        }
        m_menuBar = nullptr;
    }

    void VerticalTabs::SetCollapsed(bool collapsed)
    {
        if (m_settings.collapsed == collapsed)
        {
            return;
        }
        m_settings.collapsed = collapsed;
        ApplyAppearance();
        if (m_enabled)
        {
            m_layout.SetActive(true, LayoutSettings());
        }
        SyncSelectionFromNative(); // la scheda attiva resta in vista

        // Salvato in settings.ini: le altre finestre del Blocco note si adeguano tramite l'hot-reload.
        Settings stored = Settings::Load();
        if (stored.collapsed != collapsed)
        {
            stored.collapsed = collapsed;
            stored.Save();
        }
        NPX_LOG(L"VerticalTabs: pannello %s.", collapsed ? L"compresso" : L"espanso");
    }

    void VerticalTabs::Enable()
    {
        if (!EnsurePanel())
        {
            return;
        }
        RebuildRows();
        try
        {
            ReadNativeTitleStyle(); // solo aspetto: un errore qui non deve impedire l'attivazione
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: aspetto dei titoli non letto (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }
        SetNativeStripHidden(true);
        m_enabled = true;
        m_layout.SetActive(true, LayoutSettings());
        UpdateMenuBarTint();
        SyncSelectionFromNative();
        // Comparendo, l'isola del pannello non deve sottrarre la tastiera a chi sta scrivendo.
        if (GetForegroundWindow() == m_main)
        {
            FocusEditor();
        }
        NPX_LOG(L"VerticalTabs: schede verticali attive (%zu schede).", m_rows.size());
    }

    void VerticalTabs::Disable()
    {
        if (m_autoScrollTimer)
        {
            m_autoScrollTimer.Stop();
        }
        m_dragging = false;
        m_dragPending = false;
        m_flyoutClosedRevoker.revoke();
        RemoveColorMenu();
        RemoveMenuBarTint();
        m_layout.SetActive(false, m_settings);
        SetNativeStripHidden(false);
        ScrollNativeStripToSelection();
        ClearRows();
        if (m_items && m_itemsToken)
        {
            try
            {
                m_items.VectorChanged(m_itemsToken);
            }
            catch (...)
            {
            }
        }
        m_items = nullptr;
        m_itemsToken = {};
        m_enabled = false;
        NPX_LOG(L"VerticalTabs: schede verticali disattivate.");
    }

    // ---------------------------------------------------------------------------------------------
    // Dati: una riga per ogni scheda nativa, nello stesso ordine di TabItems()
    // ---------------------------------------------------------------------------------------------

    void VerticalTabs::RebuildRows()
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return;
        }
        if (m_items && m_itemsToken)
        {
            try
            {
                m_items.VectorChanged(m_itemsToken);
            }
            catch (...)
            {
            }
        }
        ClearRows();

        m_items = tabsBar.TabItems();
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        m_itemsToken = m_items.VectorChanged([weak](auto&&, wfc::IVectorChangedEventArgs const& args) {
            if (const auto self = weak.lock())
            {
                self->OnItemsChanged(args);
            }
        });

        for (uint32_t i = 0; i < m_items.Size(); ++i)
        {
            InsertRow(i);
        }
        RefreshVisibleRows();
    }

    void VerticalTabs::OnItemsChanged(wfc::IVectorChangedEventArgs const& args)
    {
        try
        {
            const uint32_t index = args.Index();
            NPX_VLOG(L"VerticalTabs: TabItems modificato (tipo %d, indice %u, righe %zu).", static_cast<int>(args.CollectionChange()), index,
                     m_rows.size());
            switch (args.CollectionChange())
            {
            case wfc::CollectionChange::ItemInserted:
                InsertRow(index);
                break;
            case wfc::CollectionChange::ItemRemoved:
                StashRow(index);
                break;
            case wfc::CollectionChange::ItemChanged:
                StashRow(index);
                InsertRow(index);
                break;
            default: // Reset
                while (!m_rows.empty())
                {
                    StashRow(static_cast<uint32_t>(m_rows.size() - 1));
                }
                for (uint32_t i = 0; m_items && i < m_items.Size(); ++i)
                {
                    InsertRow(i);
                }
                break;
            }

            // Se per qualunque motivo non siamo più allineati, si ricostruisce tutto (riusando le righe).
            if (m_items && m_rows.size() != m_items.Size())
            {
                while (!m_rows.empty())
                {
                    StashRow(static_cast<uint32_t>(m_rows.size() - 1));
                }
                for (uint32_t i = 0; i < m_items.Size(); ++i)
                {
                    InsertRow(i);
                }
            }

            // Un riordino arriva come "rimossa" + "inserita": l'elenco visibile si aggiorna una volta sola,
            // a fine raffica, così la riga spostata viene riusata e non sfarfalla.
            ScheduleRefresh();
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: errore aggiornando l'elenco (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }
    }

    void VerticalTabs::ScheduleRefresh()
    {
        if (m_refreshQueued)
        {
            return;
        }
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return;
        }
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        m_refreshQueued = tabsBar.DispatcherQueue().TryEnqueue(DispatcherQueuePriority::Normal, [weak] {
            if (const auto self = weak.lock())
            {
                self->m_refreshQueued = false;
                try
                {
                    self->FlushStash();
                    self->RefreshVisibleRows();
                    self->SyncSelectionFromNative();
                }
                catch (...)
                {
                    NPX_LOG(L"VerticalTabs: aggiornamento dell'elenco non riuscito (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                }
            }
        });
    }

    std::unique_ptr<VerticalTabs::Row> VerticalTabs::CreateRow(winrt::NotepadXamlUI::TabItemViewModel const& item)
    {
        auto row = std::make_unique<Row>();
        row->item = item;
        row->tabId = item.TabId();
        BuildRowElement(*row);

        const winrt::guid tabId = row->tabId;
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        row->propertyToken = row->item.PropertyChanged([weak, tabId](auto&&, auto&&) {
            const auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (Row* r = self->FindRow(tabId))
            {
                self->UpdateRow(*r);
            }
        });
        return row;
    }

    void VerticalTabs::BuildRowElement(Row& row)
    {
        row.element = LoadXaml(kRowXaml).as<muxc::ListViewItem>();
        row.tint = FindPart<muxc::Border>(row.element, L"RowTint");
        row.pillColor = FindPart<muxc::Border>(row.element, L"PillColor");
        row.pillNeutral = FindPart<muxc::Border>(row.element, L"PillNeutral");
        row.grid = FindPart<muxc::Grid>(row.element, L"RowGrid");
        row.title = FindPart<muxc::TextBlock>(row.element, L"Title");
        row.dot = FindPart<winrt::Microsoft::UI::Xaml::Shapes::Ellipse>(row.element, L"Dot");
        row.close = FindPart<muxc::Button>(row.element, L"Close");
        row.avatar = FindPart<muxc::Grid>(row.element, L"Avatar");
        row.avatarNeutral = FindPart<muxc::Border>(row.element, L"AvatarNeutral");
        row.avatarNeutralText = FindPart<muxc::TextBlock>(row.element, L"AvatarNeutralText");
        row.avatarColor = FindPart<muxc::Border>(row.element, L"AvatarColor");
        row.avatarColorText = FindPart<muxc::TextBlock>(row.element, L"AvatarColorText");
        row.badge = FindPart<winrt::Microsoft::UI::Xaml::Shapes::Ellipse>(row.element, L"Badge");
        row.hovered = false;

        // Gli handler identificano la riga per TabId (mai per puntatore): sicuri anche se la riga sparisce.
        const winrt::guid tabId = row.tabId;
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        row.close.Click([weak, tabId](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                self->CloseTab(tabId);
            }
        });
        row.element.PointerEntered([weak, tabId](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                if (Row* r = self->FindRow(tabId))
                {
                    r->hovered = true;
                    self->UpdateRow(*r);
                }
            }
        });
        row.element.PointerExited([weak, tabId](auto&&, auto&&) {
            if (const auto self = weak.lock())
            {
                if (Row* r = self->FindRow(tabId))
                {
                    r->hovered = false;
                    self->UpdateRow(*r);
                }
            }
        });
        // Clic col tasto centrale = chiudi (anche se il ListViewItem ha già gestito l'evento).
        row.element.AddHandler(mux::UIElement::PointerReleasedEvent(),
                                winrt::box_value(muxi::PointerEventHandler([weak, tabId](IInspectable const& sender,
                                                                                         muxi::PointerRoutedEventArgs const& args) {
                                    const auto self = weak.lock();
                                    const auto element = sender.try_as<mux::UIElement>();
                                    if (self && element &&
                                        args.GetCurrentPoint(element).Properties().PointerUpdateKind() ==
                                            winrt::Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased)
                                    {
                                        self->CloseTab(tabId);
                                        args.Handled(true);
                                    }
                                })),
                                true);
        // Clic destro (o Maiusc+F10 / tasto Menu): il menu contestuale del Blocco note per questa scheda.
        row.element.ContextRequested([weak, tabId](IInspectable const& sender, muxi::ContextRequestedEventArgs const& args) {
            const auto self = weak.lock();
            const auto element = sender.try_as<mux::UIElement>();
            if (!self || !element)
            {
                return;
            }
            Point point{};
            if (!args.TryGetPosition(element, point))
            {
                point = Point{ 24.0f, 18.0f }; // da tastiera: vicino all'inizio della riga
            }
            self->ShowNativeContextMenu(tabId, element, point);
            args.Handled(true);
        });
    }

    void VerticalTabs::ReleaseRow(Row& row)
    {
        try
        {
            row.item.PropertyChanged(row.propertyToken);
        }
        catch (...)
        {
        }
        row.propertyToken = {};
    }

    void VerticalTabs::InsertRow(uint32_t index)
    {
        if (!m_items || index >= m_items.Size() || index > m_rows.size())
        {
            return;
        }
        const auto item = m_items.GetAt(index);
        const winrt::guid tabId = item.TabId();

        // Riordino: la stessa scheda era appena stata tolta. Si riusa la sua riga (elemento compreso).
        std::unique_ptr<Row> row;
        for (auto it = m_stash.begin(); it != m_stash.end(); ++it)
        {
            if ((*it)->tabId == tabId)
            {
                row = std::move(*it);
                m_stash.erase(it);
                break;
            }
        }
        if (row && !(row->item == item))
        {
            // Stesso id ma oggetto diverso: la riga non è riutilizzabile.
            ReleaseRow(*row);
            m_stash.push_back(std::move(row)); // l'elemento verrà tolto dall'elenco da FlushStash
        }
        NPX_VLOG(L"VerticalTabs: riga %s per l'indice %u (TabId %s).", row ? L"riusata" : L"creata", index, winrt::to_hstring(tabId).c_str());
        if (!row)
        {
            row = CreateRow(item);
        }

        UpdateRow(*row);
        m_rows.insert(m_rows.begin() + index, std::move(row));
    }

    void VerticalTabs::StashRow(uint32_t index)
    {
        if (index >= m_rows.size())
        {
            return;
        }
        m_stash.push_back(std::move(m_rows[index]));
        m_rows.erase(m_rows.begin() + index);
    }

    void VerticalTabs::FlushStash()
    {
        // Le righe rimaste da parte appartengono a schede davvero chiuse: le si toglie dall'elenco.
        const FlagScope syncing{ m_syncing };
        bool failed = false;
        for (auto const& row : m_stash)
        {
            ReleaseRow(*row);
            uint32_t position = 0;
            try
            {
                if (m_visible && m_visible.IndexOf(row->element, position))
                {
                    m_visible.RemoveAt(position);
                }
            }
            catch (...)
            {
                NPX_LOG(L"VerticalTabs: rimozione di una riga rifiutata dall'elenco (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                failed = true;
            }
        }
        m_stash.clear();
        if (failed)
        {
            ResetVisibleList();
        }
    }

    void VerticalTabs::ClearRows()
    {
        for (auto const& row : m_rows)
        {
            ReleaseRow(*row);
        }
        for (auto const& row : m_stash)
        {
            ReleaseRow(*row);
        }
        m_rows.clear();
        m_stash.clear();
        if (m_visible)
        {
            const FlagScope syncing{ m_syncing };
            try
            {
                m_visible.Clear();
            }
            catch (...)
            {
            }
        }
    }

    void VerticalTabs::UpdateRow(Row& row)
    {
        const winrt::hstring title = row.item.Title();
        row.title.Text(title);
        muxc::ToolTipService::SetToolTip(row.element, winrt::box_value(row.item.Tooltip()));
        mux::Automation::AutomationProperties::SetName(row.element, row.item.AutomationPropertiesName());

        // Icona: iniziale del titolo sul colore assegnato alla scheda (toni per il tema chiaro o scuro),
        // altrimenti su fondo neutro. Il colore è esposto anche a UI Automation.
        const winrt::hstring initial = InitialOf(title);
        const tabcolors::Swatch* swatch = tabcolors::Find(tabcolors::Get(row.tabId));
        const bool dark = m_root && m_root.ActualTheme() == mux::ElementTheme::Dark;
        if (swatch)
        {
            row.avatarColor.Background(muxm::SolidColorBrush(dark ? swatch->fillDark : swatch->fillLight));
            row.avatarColorText.Foreground(muxm::SolidColorBrush(dark ? swatch->textDark : swatch->textLight));
            row.avatarColorText.Text(initial);
        }
        else
        {
            row.avatarNeutralText.Text(initial);
        }
        row.avatarColor.Visibility(swatch ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        row.avatarNeutral.Visibility(swatch ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        mux::Automation::AutomationProperties::SetItemStatus(row.element, swatch ? winrt::hstring{ L"Colore: " } + swatch->displayName : winrt::hstring{});

        // Fondo della riga: tinta tenue del colore della scheda (o velo neutro), semitrasparente sotto il
        // contenuto, così il passaggio del mouse disegnato sotto resta visibile. La scheda attiva ha la
        // tinta più decisa, il bordino, la stanghetta a sinistra nel suo colore (grigio neutro se non ne
        // ha uno) e il titolo più spesso (peso e colori dei titoli sono quelli delle schede native).
        const bool selected = m_list && m_list.SelectedItem() == row.element;
        const CardStyle& style = dark ? kCardDark : kCardLight;
        const winrt::Windows::UI::Color veil = dark ? winrt::Microsoft::UI::Colors::White() : winrt::Microsoft::UI::Colors::Black();
        const auto card = swatch ? WithAlpha(dark ? swatch->fillDark : swatch->fillLight, selected ? style.coloredSelected : style.colored)
                                 : WithAlpha(veil, selected ? style.neutralSelected : style.neutral);
        row.tint.Background(muxm::SolidColorBrush(card));
        row.tint.BorderBrush(selected ? muxm::SolidColorBrush(WithAlpha(veil, style.border)) : nullptr);
        if (selected && swatch)
        {
            row.pillColor.Background(muxm::SolidColorBrush(dark ? Shade(swatch->fillDark, kPillShadeDark) : swatch->fillLight));
        }
        row.pillColor.Visibility(selected && swatch ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        row.pillNeutral.Visibility(selected && !swatch ? mux::Visibility::Visible : mux::Visibility::Collapsed);

        // Titolo come nelle schede orizzontali: stesso carattere, peso e colore (normale, al passaggio del
        // mouse, attiva). Finché non sono stati letti il colore resta quello ereditato.
        const TitleStyle& titleStyle = m_titleStyle;
        row.title.FontSize(titleStyle.size);
        if (titleStyle.family)
        {
            row.title.FontFamily(titleStyle.family);
        }
        row.title.FontWeight(selected ? titleStyle.selectedWeight : titleStyle.normalWeight);
        const muxm::Brush& titleBrush = selected ? titleStyle.selected : (row.hovered && titleStyle.hover ? titleStyle.hover : titleStyle.normal);
        if (titleBrush)
        {
            row.title.Foreground(titleBrush);
        }
        else
        {
            row.title.ClearValue(muxc::TextBlock::ForegroundProperty());
        }

        // Come nella barra originale: pallino se modificata, X al passaggio del mouse o se attiva.
        // L'icona c'è solo a pannello compresso, dove è l'unica parte visibile (centrata, con il pallino
        // "modificata" sull'angolo); da espanso resta il titolo e il colore è nel fondo della riga.
        const bool compact = m_compactVisual;
        const bool icon = compact;
        const bool modified = row.item.IsModified();
        const bool showClose = !compact && (row.hovered || selected);
        row.close.Visibility(showClose ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        row.dot.Visibility(!compact && !showClose && modified ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        row.badge.Visibility(compact && modified ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        row.title.Visibility(compact ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        row.avatar.Visibility(icon ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        const auto columns = row.grid.ColumnDefinitions();
        columns.GetAt(0).Width(icon ? mux::GridLength{ 1, mux::GridUnitType::Auto } : mux::GridLength{ 0, mux::GridUnitType::Pixel });
        columns.GetAt(1).Width(compact ? mux::GridLength{ 0, mux::GridUnitType::Pixel } : mux::GridLength{ 1, mux::GridUnitType::Star });
        columns.GetAt(2).Width(mux::GridLength{ compact ? 0.0 : 28.0, mux::GridUnitType::Pixel });
        // Senza icona niente spaziatura dopo la colonna vuota: il titolo conserva lo stacco dalla X.
        row.grid.ColumnSpacing(compact || !icon ? 0.0 : 10.0);
        row.title.Margin(!compact && !icon ? mux::Thickness{ 0, 0, 10, 0 } : mux::Thickness{ 0, 0, 0, 0 });
        row.grid.Margin(compact ? mux::Thickness{ 0, 0, 0, 0 } : mux::Thickness{ icon ? 10.0 : 14.0, 0, 4, 0 });
        row.grid.HorizontalAlignment(compact ? mux::HorizontalAlignment::Center : mux::HorizontalAlignment::Stretch);
    }

    void VerticalTabs::RefreshVisibleRows()
    {
        if (!m_visible)
        {
            return;
        }
        std::vector<IInspectable> desired;
        desired.reserve(m_rows.size());
        for (auto const& row : m_rows)
        {
            desired.push_back(row->element);
        }

        // Allinea l'elenco visibile con inserimenti/rimozioni minimi (niente Reset: la ListView
        // conserva scorrimento e selezione). Dopo un trascinamento è già allineato: nessuna modifica.
        const FlagScope syncing{ m_syncing };
        try
        {
            for (uint32_t i = 0; i < desired.size(); ++i)
            {
                if (i < m_visible.Size() && m_visible.GetAt(i) == desired[i])
                {
                    continue;
                }
                uint32_t found = UINT32_MAX;
                for (uint32_t j = i + 1; j < m_visible.Size(); ++j)
                {
                    if (m_visible.GetAt(j) == desired[i])
                    {
                        found = j;
                        break;
                    }
                }
                if (found != UINT32_MAX)
                {
                    for (uint32_t k = i; k < found; ++k)
                    {
                        m_visible.RemoveAt(i);
                    }
                }
                else
                {
                    m_visible.InsertAt(i, desired[i]);
                }
            }
            while (m_visible.Size() > desired.size())
            {
                m_visible.RemoveAt(m_visible.Size() - 1);
            }
        }
        catch (...)
        {
            // La ListView ha rifiutato la modifica (es. elemento non ancora staccato): elenco da ricostruire.
            NPX_LOG(L"VerticalTabs: modifica dell'elenco rifiutata (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            ResetVisibleList();
        }
    }

    void VerticalTabs::ResetVisibleList()
    {
        // Ricostruzione completa, con una raccolta nuova ed elementi nuovi per ogni riga: rimedio per il
        // caso in cui la ListView non sia più allineata alla raccolta.
        if (!m_list)
        {
            return;
        }
        const FlagScope syncing{ m_syncing };
        try
        {
            m_list.ItemsSource(nullptr);
            m_visible = winrt::single_threaded_observable_vector<IInspectable>();
            for (auto const& row : m_rows)
            {
                BuildRowElement(*row);
                UpdateRow(*row);
                m_visible.Append(row->element);
            }
            m_list.ItemsSource(m_visible);
            NPX_LOG(L"VerticalTabs: elenco ricostruito (%u righe).", m_visible.Size());
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: ricostruzione dell'elenco non riuscita (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
        }
    }

    VerticalTabs::Row* VerticalTabs::FindRow(winrt::guid const& tabId)
    {
        for (auto const& row : m_rows)
        {
            if (row->tabId == tabId)
            {
                return row.get();
            }
        }
        return nullptr;
    }

    VerticalTabs::Row* VerticalTabs::FindRow(IInspectable const& element)
    {
        if (!element)
        {
            return nullptr;
        }
        for (auto const& row : m_rows)
        {
            if (row->element == element)
            {
                return row.get();
            }
        }
        return nullptr;
    }

    int VerticalTabs::IndexOfTab(winrt::guid const& tabId) const
    {
        if (!m_items)
        {
            return -1;
        }
        for (uint32_t i = 0; i < m_items.Size(); ++i)
        {
            if (m_items.GetAt(i).TabId() == tabId)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // ---------------------------------------------------------------------------------------------
    // Selezione
    // ---------------------------------------------------------------------------------------------

    void VerticalTabs::SyncSelectionFromNative()
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar || !m_list || !m_enabled || m_dragging)
        {
            return;
        }
        const int index = tabsBar.SelectedIndex();
        Row* selected = index >= 0 && index < static_cast<int>(m_rows.size()) ? m_rows[static_cast<size_t>(index)].get() : nullptr;

        {
            const FlagScope syncing{ m_syncing };
            try
            {
                uint32_t position = 0;
                if (selected && m_visible.IndexOf(selected->element, position))
                {
                    m_list.SelectedIndex(static_cast<int>(position));
                    m_list.ScrollIntoView(selected->element);
                }
                else
                {
                    m_list.SelectedIndex(-1); // scheda attiva non (ancora) nell'elenco
                }
            }
            catch (...)
            {
                NPX_LOG(L"VerticalTabs: selezione non sincronizzata (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            }
        }

        for (auto const& row : m_rows)
        {
            UpdateRow(*row);
        }
    }

    void VerticalTabs::OnListSelectionChanged()
    {
        // Durante il trascinamento la ListView sposta l'elemento e la selezione cambia solo in apparenza.
        if (m_syncing || m_dragging)
        {
            return;
        }
        if (const auto tabsBar = m_tabsBar.get())
        {
            if (const Row* row = FindRow(m_list.SelectedItem()))
            {
                try
                {
                    tabsBar.SelectTabByTabId(row->tabId);
                }
                catch (...)
                {
                }
            }
        }
        for (auto const& row : m_rows)
        {
            UpdateRow(*row);
        }
    }

    void VerticalTabs::FocusEditor()
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return;
        }
        // Dopo che il Blocco note ha gestito il cambio di scheda: il focus torna all'editor.
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        tabsBar.DispatcherQueue().TryEnqueue(DispatcherQueuePriority::Low, [weak] {
            const auto self = weak.lock();
            if (!self)
            {
                return;
            }
            const HWND target = self->m_layout.EditorFocusTarget();
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

    // ---------------------------------------------------------------------------------------------
    // Riordino con trascinamento
    // ---------------------------------------------------------------------------------------------

    VerticalTabs::Row* VerticalTabs::RowFromSource(IInspectable const& source, bool& onCloseButton)
    {
        // Dall'elemento colpito si risale fino alla riga, annotando se si è passati dal pulsante X.
        onCloseButton = false;
        for (auto current = source.try_as<mux::DependencyObject>(); current; current = muxm::VisualTreeHelper::GetParent(current))
        {
            if (Row* row = FindRow(current))
            {
                return row;
            }
            if (const auto button = current.try_as<muxc::Button>(); button && button.Name() == L"Close")
            {
                onCloseButton = true;
            }
        }
        return nullptr;
    }

    void VerticalTabs::OnListPointerPressed(muxi::PointerRoutedEventArgs const& args)
    {
        if (m_dragging || m_dragPending)
        {
            return;
        }
        const auto point = args.GetCurrentPoint(m_list);
        // Al tocco la ListView scorre; il riordino è per mouse e penna, col tasto principale.
        if (point.PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Touch || !point.Properties().IsLeftButtonPressed())
        {
            return;
        }
        bool onCloseButton = false;
        const Row* row = RowFromSource(args.OriginalSource(), onCloseButton);
        if (!row || onCloseButton)
        {
            return;
        }
        m_dragPending = true;
        m_dragPointerId = args.Pointer().PointerId();
        m_dragTabId = row->tabId;
        m_dragStart = point.Position();
    }

    void VerticalTabs::OnListPointerMoved(muxi::PointerRoutedEventArgs const& args)
    {
        if ((!m_dragPending && !m_dragging) || args.Pointer().PointerId() != m_dragPointerId)
        {
            return;
        }
        const auto point = args.GetCurrentPoint(m_list);
        if (!point.Properties().IsLeftButtonPressed())
        {
            // Rilascio non ricevuto (es. avvenuto fuori dalla finestra senza cattura).
            m_dragging ? EndDrag(true) : void(m_dragPending = false);
            return;
        }
        const Point position = point.Position();
        if (m_dragPending)
        {
            if (std::abs(position.Y - m_dragStart.Y) < kDragThreshold && std::abs(position.X - m_dragStart.X) < kDragThreshold)
            {
                return;
            }
            BeginDrag(args.Pointer());
            if (!m_dragging)
            {
                return;
            }
        }
        m_dragY = position.Y;
        UpdateDrag();
        args.Handled(true);
    }

    void VerticalTabs::OnListPointerReleased(muxi::PointerRoutedEventArgs const& args)
    {
        if (args.Pointer().PointerId() != m_dragPointerId)
        {
            return;
        }
        m_dragPending = false; // semplice clic: ci pensa la ListView
        if (m_dragging)
        {
            EndDrag(true);
            args.Handled(true);
        }
    }

    void VerticalTabs::BeginDrag(muxi::Pointer const& pointer)
    {
        m_dragPending = false;
        m_dragFromIndex = IndexOfTab(m_dragTabId);
        // La cattura passa dalla riga all'elenco: la riga non riceverà il rilascio (niente clic) e
        // l'elenco continua a ricevere il puntatore anche mentre le righe vengono spostate.
        if (m_dragFromIndex < 0 || !m_list.CapturePointer(pointer))
        {
            m_dragFromIndex = -1;
            return;
        }
        m_dragging = true;

        // Scorrimento automatico quando il puntatore resta vicino ai bordi dell'elenco.
        if (!m_autoScrollTimer)
        {
            if (const auto tabsBar = m_tabsBar.get())
            {
                m_autoScrollTimer = tabsBar.DispatcherQueue().CreateTimer();
                m_autoScrollTimer.Interval(std::chrono::milliseconds(30));
                m_autoScrollTimer.IsRepeating(true);
                const std::weak_ptr<VerticalTabs> weak = weak_from_this();
                m_autoScrollTimer.Tick([weak](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        self->AutoScrollStep();
                    }
                });
            }
        }
        if (m_autoScrollTimer)
        {
            m_autoScrollTimer.Start();
        }
    }

    void VerticalTabs::UpdateDrag()
    {
        Row* row = FindRow(m_dragTabId);
        uint32_t current = 0;
        if (!row || !m_visible.IndexOf(row->element, current))
        {
            EndDrag(false); // la scheda è sparita durante il trascinamento
            return;
        }

        // La riga trascinata segue il puntatore: si sposta di tante posizioni quante sono le righe
        // superate per più di metà (stesso passo per tutte le righe).
        m_list.UpdateLayout();
        const auto slot = muxp::LayoutInformation::GetLayoutSlot(row->element);
        const double pitch = slot.Height;
        if (pitch <= 1.0)
        {
            return;
        }
        double middle = 0;
        try
        {
            middle = row->element.TransformToVisual(m_list).TransformPoint(Point{ 0, static_cast<float>(row->element.ActualHeight() / 2) }).Y;
        }
        catch (...)
        {
            return;
        }
        const int count = static_cast<int>(m_visible.Size());
        const int target = std::clamp(static_cast<int>(current) + static_cast<int>(std::lround((m_dragY - middle) / pitch)), 0, count - 1);
        if (target == static_cast<int>(current))
        {
            return;
        }
        const bool selected = m_list.SelectedItem() == row->element;
        const FlagScope syncing{ m_syncing };
        try
        {
            m_visible.RemoveAt(current);
            m_visible.InsertAt(static_cast<uint32_t>(target), row->element);
            if (selected)
            {
                m_list.SelectedItem(row->element); // la scheda attiva resta evidenziata mentre si sposta
            }
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: spostamento della riga rifiutato (0x%08X): trascinamento annullato.",
                    static_cast<unsigned>(winrt::to_hresult()));
            ResetVisibleList();
            EndDrag(false);
        }
    }

    void VerticalTabs::AutoScrollStep()
    {
        if (!m_dragging || !m_list)
        {
            return;
        }
        const double height = m_list.ActualHeight();
        double delta = 0;
        if (m_dragY < kAutoScrollEdge)
        {
            delta = -std::min(kAutoScrollMaxStep, (kAutoScrollEdge - m_dragY) * 0.6);
        }
        else if (m_dragY > height - kAutoScrollEdge)
        {
            delta = std::min(kAutoScrollMaxStep, (m_dragY - (height - kAutoScrollEdge)) * 0.6);
        }
        const auto scroller = delta != 0 ? FindDescendant<muxc::ScrollViewer>(m_list) : nullptr;
        if (!scroller)
        {
            return;
        }
        const double offset = std::clamp(scroller.VerticalOffset() + delta, 0.0, scroller.ScrollableHeight());
        if (std::abs(offset - scroller.VerticalOffset()) < 0.5)
        {
            return;
        }
        scroller.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{ offset }, nullptr, true);
        UpdateDrag();
    }

    void VerticalTabs::EndDrag(bool commit)
    {
        m_dragPending = false;
        if (!m_dragging)
        {
            return;
        }
        if (m_autoScrollTimer)
        {
            m_autoScrollTimer.Stop();
        }

        // L'elenco mostra già il nuovo ordine: lo stesso spostamento si applica alle schede vere con il
        // metodo della TabsBar (ReorderTab). Fino alla fine le notifiche transitorie di selezione del
        // Blocco note vengono ignorate (m_dragging): la sincronizzazione avviene a riordino concluso.
        const auto tabsBar = m_tabsBar.get();
        if (commit && tabsBar && m_dragFromIndex >= 0)
        {
            uint32_t to = 0;
            const Row* row = FindRow(m_dragTabId);
            if (row && m_visible.IndexOf(row->element, to) && static_cast<int>(to) != m_dragFromIndex)
            {
                try
                {
                    tabsBar.ReorderTab(m_dragFromIndex, static_cast<int>(to));
                    NPX_LOG(L"VerticalTabs: scheda spostata da %d a %u.", m_dragFromIndex, to);
                }
                catch (...)
                {
                    NPX_LOG(L"VerticalTabs: riordino non riuscito (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
                }
            }
        }
        m_dragging = false;
        m_dragFromIndex = -1;
        try
        {
            m_list.ReleasePointerCaptures();
        }
        catch (...)
        {
        }
        // In ogni caso l'elenco si riallinea all'ordine reale del Blocco note (annullamento compreso).
        ScheduleRefresh();
        FocusEditor();
    }

    // ---------------------------------------------------------------------------------------------
    // Menu contestuale nativo
    // ---------------------------------------------------------------------------------------------

    void VerticalTabs::ShowNativeContextMenu(winrt::guid const& tabId, mux::UIElement const& element, Point const& localPoint)
    {
        const auto tabsBar = m_tabsBar.get();
        const auto container = NativeContainer(IndexOfTab(tabId));
        if (!tabsBar || !container || !m_panelHwnd)
        {
            NPX_LOG(L"VerticalTabs: menu contestuale non disponibile (scheda nativa non trovata).");
            return;
        }

        // Il menu è quello del Blocco note (TabsBarItemContextFlyout), aperto sulla sua vera scheda come
        // farebbe il framework al clic destro: le voci (nuova scheda, chiudi, chiudi altre, chiudi a
        // destra) agiscono su quella scheda, con la logica del Blocco note.
        mux::UIElement owner = container;
        muxp::FlyoutBase flyout = container.ContextFlyout();
        if (!flyout)
        {
            // Menu assegnato a un elemento del modello della scheda.
            if (const auto inner = FindContextFlyoutOwner(container))
            {
                owner = inner;
                flyout = inner.ContextFlyout();
            }
        }
        if (!flyout)
        {
            const auto key = winrt::box_value(winrt::hstring{ L"TabsBarItemContextFlyout" });
            if (tabsBar.Resources().HasKey(key))
            {
                flyout = tabsBar.Resources().Lookup(key).try_as<muxp::FlyoutBase>();
            }
        }
        if (!flyout)
        {
            NPX_LOG(L"VerticalTabs: menu contestuale nativo non trovato.");
            return;
        }

        // Punto del clic nelle coordinate client della finestra principale.
        const auto inPanel = element.TransformToVisual(nullptr).TransformPoint(localPoint);
        const double panelScale = element.XamlRoot().RasterizationScale();
        const RECT panelRect = interop::RectInClientOf(m_panelHwnd, m_main);
        const POINT click{ panelRect.left + std::lround(inPanel.X * panelScale), panelRect.top + std::lround(inPanel.Y * panelScale) };

        // La striscia nativa (invisibile) viene portata con la scheda sotto il puntatore: così il menu
        // si apre esattamente lì e sul monitor giusto. Alla chiusura torna fuori dall'area visibile.
        const auto origin = owner.TransformToVisual(nullptr).TransformPoint(Point{ 0, 0 });
        const double nativeScale = owner.XamlRoot().RasterizationScale();
        m_layout.PinNativeTabs(POINT{ click.x - std::lround(origin.X * nativeScale), click.y - std::lround(origin.Y * nativeScale) });

        // Apertura al giro successivo del dispatcher: l'isola deve aver recepito la nuova posizione.
        const std::weak_ptr<VerticalTabs> weak = weak_from_this();
        const bool queued = tabsBar.DispatcherQueue().TryEnqueue(DispatcherQueuePriority::Normal, [weak, flyout, owner, tabId] {
            const auto self = weak.lock();
            if (!self)
            {
                return;
            }
            try
            {
                self->m_flyoutClosedRevoker = flyout.Closed(winrt::auto_revoke, [weak](auto&&, auto&&) {
                    if (const auto s = weak.lock())
                    {
                        s->RemoveColorMenu(); // il menu nativo torna com'era
                        s->m_layout.PinNativeTabs(std::nullopt);
                        s->m_flyoutClosedRevoker.revoke();
                    }
                });
                // In fondo al menu del Blocco note: "Colore scheda" (solo quando lo apre il pannello).
                if (const auto menu = flyout.try_as<muxc::MenuFlyout>())
                {
                    self->AddColorMenu(menu, tabId);
                }
                muxp::FlyoutShowOptions options;
                options.Position(winrt::Windows::Foundation::IReference<Point>{ Point{ 0, 0 } });
                options.ShowMode(muxp::FlyoutShowMode::Standard);
                flyout.ShowAt(owner, options);
            }
            catch (...)
            {
                self->m_flyoutClosedRevoker.revoke();
                self->RemoveColorMenu();
                self->m_layout.PinNativeTabs(std::nullopt);
                NPX_LOG(L"VerticalTabs: apertura del menu contestuale non riuscita (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            }
        });
        if (!queued)
        {
            m_layout.PinNativeTabs(std::nullopt);
        }
    }

    void VerticalTabs::AddColorMenu(muxc::MenuFlyout const& menu, winrt::guid const& tabId)
    {
        RemoveColorMenu();
        try
        {
            const bool dark = m_root && m_root.ActualTheme() == mux::ElementTheme::Dark;
            const auto current = tabcolors::Get(tabId);
            const std::weak_ptr<VerticalTabs> weak = weak_from_this();

            muxc::MenuFlyoutSubItem colors;
            colors.Text(L"Colore scheda");
            muxc::FontIcon colorsIcon;
            colorsIcon.Glyph(L""); // tavolozza
            colors.Icon(colorsIcon);

            const auto addItem = [&](tabcolors::TabColor color, wchar_t const* text, std::optional<winrt::Windows::UI::Color> fill) {
                muxc::RadioMenuFlyoutItem item;
                item.GroupName(L"VTabsNotepadTabColor");
                item.Text(text);
                item.IsChecked(color == current);
                muxc::FontIcon icon;
                icon.Glyph(fill ? L"" : L""); // cerchio pieno del colore / cerchio vuoto
                if (fill)
                {
                    icon.Foreground(muxm::SolidColorBrush(*fill));
                }
                item.Icon(icon);
                item.Click([weak, tabId, color](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        self->SetTabColor(tabId, color);
                    }
                });
                colors.Items().Append(item);
            };
            addItem(tabcolors::TabColor::None, L"Nessuno", std::nullopt);
            for (auto const& swatch : tabcolors::Palette())
            {
                addItem(swatch.color, swatch.displayName, dark ? swatch.fillDark : swatch.fillLight);
            }

            muxc::MenuFlyoutSeparator separator;
            menu.Items().Append(separator);
            menu.Items().Append(colors);
            m_extendedMenu = menu;
            m_menuExtras = { separator, colors };
        }
        catch (...)
        {
            NPX_LOG(L"VerticalTabs: voce \"Colore scheda\" non aggiunta al menu (0x%08X).", static_cast<unsigned>(winrt::to_hresult()));
            RemoveColorMenu();
        }
    }

    void VerticalTabs::RemoveColorMenu()
    {
        if (m_extendedMenu)
        {
            try
            {
                const auto items = m_extendedMenu.Items();
                for (auto const& extra : m_menuExtras)
                {
                    uint32_t index = 0;
                    if (items.IndexOf(extra, index))
                    {
                        items.RemoveAt(index);
                    }
                }
            }
            catch (...)
            {
            }
        }
        m_extendedMenu = nullptr;
        m_menuExtras.clear();
    }

    void VerticalTabs::SetTabColor(winrt::guid const& tabId, tabcolors::TabColor color)
    {
        tabcolors::Set(tabId, color);
        if (Row* row = FindRow(tabId))
        {
            UpdateRow(*row);
        }
        const tabcolors::Swatch* swatch = tabcolors::Find(color);
        NPX_LOG(L"VerticalTabs: colore della scheda %d: %s.", IndexOfTab(tabId), swatch ? swatch->key : L"nessuno");
    }

    // ---------------------------------------------------------------------------------------------
    // Divisore trascinabile
    // ---------------------------------------------------------------------------------------------

    void VerticalTabs::SetSplitterHighlight(bool on)
    {
        if (m_separatorActive)
        {
            m_separatorActive.Visibility(on ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        }
    }

    void VerticalTabs::ResizeFromCursor()
    {
        // Il bordo destro della colonna segue il cursore.
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(m_main, &cursor);
        const int px = m_grabOffsetPx + cursor.x;
        ApplyPaneWidth(MulDiv(px, USER_DEFAULT_SCREEN_DPI, static_cast<int>(DpiOf(m_main))));
    }

    void VerticalTabs::ApplyPaneWidth(int widthDip)
    {
        // Limiti delle impostazioni; WindowLayout garantisce in più una larghezza minima all'editor.
        widthDip = std::clamp(widthDip, Settings::kMinPaneWidth, Settings::kMaxPaneWidth);
        if (widthDip == m_settings.paneWidth)
        {
            return;
        }
        m_settings.paneWidth = widthDip;
        if (m_enabled)
        {
            m_layout.SetActive(true, LayoutSettings());
        }
    }

    void VerticalTabs::PersistPaneWidth()
    {
        // Salvata in settings.ini: le altre finestre del Blocco note la adottano tramite l'hot-reload.
        Settings stored = Settings::Load();
        if (stored.paneWidth != m_settings.paneWidth)
        {
            stored.paneWidth = m_settings.paneWidth;
            stored.Save();
            NPX_LOG(L"VerticalTabs: larghezza del pannello impostata a %d.", m_settings.paneWidth);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Comandi eseguiti tramite i controlli nativi
    // ---------------------------------------------------------------------------------------------

    muxc::TabView VerticalTabs::NativeTabView()
    {
        if (const auto cached = m_nativeTabView.get())
        {
            return cached;
        }
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return nullptr;
        }
        const auto tabView = FindDescendant<muxc::TabView>(tabsBar);
        if (tabView)
        {
            m_nativeTabView = winrt::make_weak(tabView);
        }
        return tabView;
    }

    muxc::TabViewItem VerticalTabs::NativeContainer(int index)
    {
        const auto tabView = NativeTabView();
        if (!tabView || index < 0)
        {
            return nullptr;
        }
        auto container = tabView.ContainerFromIndex(index).try_as<muxc::TabViewItem>();
        if (!container)
        {
            // Contenitore non ancora realizzato: lo si fa realizzare nella striscia (invisibile).
            if (const auto listView = FindDescendant<muxc::ListView>(tabView, L"TabListView"))
            {
                if (static_cast<uint32_t>(index) < listView.Items().Size())
                {
                    listView.ScrollIntoView(listView.Items().GetAt(static_cast<uint32_t>(index)));
                    listView.UpdateLayout();
                }
            }
            container = tabView.ContainerFromIndex(index).try_as<muxc::TabViewItem>();
        }
        return container;
    }

    void VerticalTabs::CloseTab(winrt::guid const& tabId)
    {
        // Si preme la X della scheda nativa: stessa logica del Blocco note, compreso "Salvare?".
        const int index = IndexOfTab(tabId);
        const auto container = NativeContainer(index);
        const auto button = container ? FindDescendant<muxc::Button>(container, L"CloseButton") : nullptr;
        if (!InvokeButton(button))
        {
            NPX_LOG(L"VerticalTabs: pulsante di chiusura nativo non disponibile per la scheda %d.", index);
        }
    }

    void VerticalTabs::NewTab()
    {
        const auto tabView = NativeTabView();
        const auto button = tabView ? FindDescendant<muxc::Button>(tabView, L"AddButton") : nullptr;
        if (InvokeButton(button))
        {
            FocusEditor();
        }
        else
        {
            NPX_LOG(L"VerticalTabs: pulsante \"nuova scheda\" nativo non trovato.");
        }
    }

    void VerticalTabs::ScrollNativeStripToSelection()
    {
        // Tornando in orizzontale, la striscia nativa deve mostrare la scheda attiva.
        try
        {
            if (const auto tabView = NativeTabView())
            {
                if (const auto listView = FindDescendant<muxc::ListView>(tabView, L"TabListView"))
                {
                    if (const auto selected = listView.SelectedItem())
                    {
                        listView.ScrollIntoView(selected);
                    }
                }
            }
        }
        catch (...)
        {
        }
    }

    void VerticalTabs::SetNativeStripHidden(bool hidden)
    {
        const auto tabsBar = m_tabsBar.get();
        if (!tabsBar)
        {
            return;
        }
        // Solo proprietà visive: niente riorganizzazione dell'albero (né Unloaded/Loaded), e la
        // striscia continua a fare layout, quindi i suoi pulsanti e il suo menu restano utilizzabili.
        if (hidden && !m_nativeHidden)
        {
            m_nativeOpacity = tabsBar.Opacity();
            m_nativeHitTest = tabsBar.IsHitTestVisible();
            tabsBar.Opacity(0.0);
            tabsBar.IsHitTestVisible(false);
            m_nativeHidden = true;
        }
        else if (!hidden && m_nativeHidden)
        {
            tabsBar.Opacity(m_nativeOpacity);
            tabsBar.IsHitTestVisible(m_nativeHitTest);
            m_nativeHidden = false;
        }
    }
}
