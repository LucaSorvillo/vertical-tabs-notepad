#include "pch.h"
#include "ScrollBarFit.h"

#include "Interop.h"
#include "Log.h"

namespace npx
{
    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;

        constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

        double ReadLocalDouble(mux::DependencyObject const& element, mux::DependencyProperty const& property)
        {
            const auto local = element.ReadLocalValue(property);
            if (!local || local == mux::DependencyProperty::UnsetValue())
            {
                return kNaN;
            }
            return winrt::unbox_value_or<double>(local, kNaN);
        }

        // Stesso valore (XAML lo conserva come float: qualche decimillesimo di differenza). Falso se uno dei due è NaN.
        bool Same(double a, double b)
        {
            return std::abs(a - b) < 0.005;
        }

        // Valore da scrivere mai intero (vedi ScrollBarFit.h): 1/32 DIP non si vede.
        double NotWhole(double value)
        {
            return std::abs(value - std::round(value)) < 0.01 ? value + 1.0 / 32 : value;
        }
    }

    mux::DependencyProperty ScrollBarFit::SizeProperty() const
    {
        return m_axis == Axis::Vertical ? mux::FrameworkElement::HeightProperty() : mux::FrameworkElement::WidthProperty();
    }

    const wchar_t* ScrollBarFit::AxisName() const
    {
        return m_axis == Axis::Vertical ? L"verticale" : L"orizzontale";
    }

    double ScrollBarFit::Local(mux::FrameworkElement const& element) const
    {
        return ReadLocalDouble(element, SizeProperty());
    }

    muxc::ScrollViewer ScrollBarFit::Find() const
    {
        if (!scrollViewersProvider)
        {
            return nullptr;
        }
        // Le barre dell'editor sono ScrollViewer "ScrollViewer" dentro un controllo del Blocco note (NotepadXamlUI.ScrollBar),
        // ciascuna nella sua isola di questa finestra: stretta e alta la verticale, larga e bassa l'orizzontale. Gli altri
        // (striscia schede, elenchi, il nostro pannello) stanno in controlli di WinUI e in isole d'altra forma.
        for (auto const& candidate : scrollViewersProvider())
        {
            try
            {
                if (!candidate.IsLoaded())
                {
                    continue; // fuori dall'albero (vecchia, o non ancora entrata)
                }
                const auto parent = muxm::VisualTreeHelper::GetParent(candidate);
                if (!parent || !std::wstring_view(winrt::get_class_name(parent)).starts_with(L"NotepadXamlUI."))
                {
                    continue;
                }
                const auto xamlRoot = candidate.XamlRoot();
                const auto environment = xamlRoot ? xamlRoot.ContentIslandEnvironment() : nullptr;
                if (!environment || GetAncestor(interop::HwndFromWindowId(environment.AppWindowId()), GA_ROOT) != m_main)
                {
                    continue;
                }
                const auto size = xamlRoot.Size();
                const bool shape = m_axis == Axis::Vertical ? size.Width > 0 && size.Width <= 40 && size.Height >= 3 * size.Width
                                                            : size.Height > 0 && size.Height <= 40 && size.Width >= 3 * size.Height;
                if (shape)
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

    void ScrollBarFit::SetDelta(int deltaPx)
    {
        m_delta = deltaPx;
        Apply();
    }

    void ScrollBarFit::Refresh()
    {
        Apply();
    }

    void ScrollBarFit::Apply()
    {
        try
        {
            auto scrollViewer = m_scrollViewer.get();
            if (scrollViewer && !scrollViewer.IsLoaded())
            {
                // Uscita dall'albero: il Blocco note l'ha ricreata. Si cerca quella nuova.
                Reset();
                scrollViewer = nullptr;
            }
            if (!scrollViewer)
            {
                if (m_delta == 0)
                {
                    return; // niente da correggere: non serve nemmeno cercarla
                }
                scrollViewer = Find();
                if (!scrollViewer)
                {
                    return; // non ancora creata (o ricreata): ci riprova il prossimo controllo
                }
                m_scrollViewer = winrt::make_weak(scrollViewer);
                Attach(m_view, scrollViewer);
            }
            // Il contenuto (Canvas "ScrollContent") si segue anche se il Blocco note lo sostituisce.
            const auto content = scrollViewer.Content().try_as<mux::FrameworkElement>();
            if (content != m_content.element.get())
            {
                Detach(m_content, /*restore*/ true);
                if (content)
                {
                    Attach(m_content, content);
                }
            }
            Sync();
        }
        catch (...)
        {
            NPX_LOG(L"ScrollBarFit: barra %s non adattata (0x%08X).", AxisName(), static_cast<unsigned>(winrt::to_hresult()));
        }
    }

    void ScrollBarFit::Attach(Slot& slot, mux::FrameworkElement const& element)
    {
        slot = Slot{};
        slot.element = winrt::make_weak(element);
        std::weak_ptr<int> alive = m_alive;
        slot.token = element.RegisterPropertyChangedCallback(SizeProperty(), [this, alive](mux::DependencyObject const&, mux::DependencyProperty const&) {
            if (!alive.expired())
            {
                OnSizeChanged();
            }
        });
    }

    void ScrollBarFit::Detach(Slot& slot, bool restore)
    {
        if (const auto element = slot.element.get())
        {
            try
            {
                if (slot.token)
                {
                    element.UnregisterPropertyChangedCallback(SizeProperty(), slot.token);
                }
                if (restore)
                {
                    Restore(slot, element);
                }
            }
            catch (...)
            {
            }
        }
        slot = Slot{};
    }

    void ScrollBarFit::ReadBase(Slot& slot, mux::FrameworkElement const& element) const
    {
        const double local = Local(element);
        if (!Same(local, slot.ours))
        {
            // Valore del Blocco note: nuovo, o mai toccato da noi.
            slot.base = local;
            slot.ours = kNaN;
        }
    }

    void ScrollBarFit::Restore(Slot& slot, mux::FrameworkElement const& element)
    {
        // Torna il valore del Blocco note, se l'ultimo scritto è ancora il nostro.
        if (!std::isnan(slot.base) && Same(Local(element), slot.ours))
        {
            Write(element, slot.base);
        }
        slot.ours = kNaN;
    }

    void ScrollBarFit::Sync()
    {
        const auto view = m_view.element.get();
        const auto content = m_content.element.get();
        if (!view)
        {
            return;
        }
        ReadBase(m_view, view);
        if (content)
        {
            ReadBase(m_content, content);
        }
        // Vista e contenuto si correggono insieme o per niente: con una sola delle due la corsa del testo non tornerebbe.
        const bool known = m_view.base > 0 && content && m_content.base > 0; // falso anche con NaN
        if (!known)
        {
            if (m_delta != 0 && !m_unknownLogged)
            {
                m_unknownLogged = true;
                NPX_LOG(L"ScrollBarFit: barra %s fatta in modo diverso (vista %g, contenuto %s %g): nessuna correzione.", AxisName(), m_view.base,
                        content ? winrt::get_class_name(content).c_str() : L"assente", m_content.base);
            }
            Restore(m_view, view);
            if (content)
            {
                Restore(m_content, content);
            }
            return;
        }
        const auto xamlRoot = view.XamlRoot();
        const double scale = xamlRoot && xamlRoot.RasterizationScale() > 0 ? xamlRoot.RasterizationScale() : 1.0;
        const double delta = m_delta / scale;
        if (m_delta == 0 || m_view.base + delta < 1 || m_content.base + delta < 1 || GetTickCount64() < m_pausedUntil)
        {
            if (!std::isnan(m_view.ours) || !std::isnan(m_content.ours))
            {
                Restore(m_view, view);
                Restore(m_content, content);
                NPX_VLOG(L"ScrollBarFit: barra %s di nuovo con le misure del Blocco note.", AxisName());
            }
            return;
        }
        const double wantedView = NotWhole(m_view.base + delta);
        const double wantedContent = NotWhole(m_content.base + delta);
        bool changed = false;
        if (!Same(m_view.ours, wantedView))
        {
            Write(view, wantedView);
            m_view.ours = wantedView;
            changed = true;
        }
        if (!Same(m_content.ours, wantedContent))
        {
            Write(content, wantedContent);
            m_content.ours = wantedContent;
            changed = true;
        }
        if (!changed)
        {
            return;
        }
        if (!m_appliedLogged)
        {
            m_appliedLogged = true;
            NPX_LOG(L"ScrollBarFit: barra %s adattata all'isola (%+d px): vista %.1f -> %.1f, contenuto %.1f -> %.1f DIP.", AxisName(), m_delta,
                    m_view.base, wantedView, m_content.base, wantedContent);
        }
        else
        {
            NPX_VLOG(L"ScrollBarFit: barra %s: vista %.1f -> %.1f, contenuto %.1f -> %.1f DIP.", AxisName(), m_view.base, wantedView, m_content.base,
                     wantedContent);
        }
    }

    void ScrollBarFit::Write(mux::FrameworkElement const& element, double value)
    {
        m_writing = true;
        struct Reset
        {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{ m_writing };
        if (m_axis == Axis::Vertical)
        {
            element.Height(value);
        }
        else
        {
            element.Width(value);
        }
    }

    void ScrollBarFit::OnSizeChanged()
    {
        if (m_writing)
        {
            return; // la nostra
        }
        // Valvola di sicurezza: il Blocco note reimposta le misure solo quando ricalcola il layout o cambia il testo;
        // centinaia di volte al secondo vorrebbe dire un rimpallo con le nostre: per 30 s si lasciano le sue.
        const ULONGLONG now = GetTickCount64();
        if (now - m_burstStart >= 1000)
        {
            m_burstStart = now;
            m_burstCount = 0;
        }
        if (++m_burstCount > 500 && now >= m_pausedUntil)
        {
            m_pausedUntil = now + 30000;
            NPX_LOG(L"ScrollBarFit: misure della barra %s reimpostate di continuo: correzione sospesa per 30 s.", AxisName());
        }
        try
        {
            Sync(); // dentro la put_ del Blocco note: la barra non resta mai sbagliata per un fotogramma
        }
        catch (...)
        {
        }
    }

    void ScrollBarFit::Reset()
    {
        Detach(m_view, /*restore*/ true);
        Detach(m_content, /*restore*/ true);
        m_scrollViewer = {};
    }

    void ScrollBarFit::Abandon()
    {
        // Finestra in chiusura: niente XAML. Le callback ancora registrate restano inerti (elementi dimenticati, poi
        // m_alive scaduto).
        m_view = Slot{};
        m_content = Slot{};
        m_scrollViewer = {};
    }
}
