#include "pch.h"
#include "ScrollBarHover.h"

#include "Interop.h"
#include "Log.h"
#include "Shared.h"

namespace npx
{
    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;
        namespace muxa = winrt::Microsoft::UI::Xaml::Media::Animation;
        using winrt::Windows::Foundation::IReference;
        using winrt::Windows::Foundation::TimeSpan;

        // Animazioni degli stati "Expanded" ed "ExpandedWithoutAnimation" (gruppo "ConsciousStates") del modello di una
        // barra, ottenute ogni volta fresche dall'albero. false se il modello non è ancora applicato.
        template <typename F>
        bool ForEachExpandTimeline(muxp::ScrollBar const& bar, F&& visit)
        {
            if (muxm::VisualTreeHelper::GetChildrenCount(bar) == 0)
            {
                return false;
            }
            const auto root = muxm::VisualTreeHelper::GetChild(bar, 0).try_as<mux::FrameworkElement>();
            if (!root)
            {
                return true;
            }
            for (auto const& group : mux::VisualStateManager::GetVisualStateGroups(root))
            {
                if (group.Name() != L"ConsciousStates")
                {
                    continue;
                }
                for (auto const& state : group.States())
                {
                    const std::wstring name{ state.Name() };
                    if (name != L"Expanded" && name != L"ExpandedWithoutAnimation")
                    {
                        continue;
                    }
                    const auto storyboard = state.Storyboard();
                    if (!storyboard)
                    {
                        continue;
                    }
                    const auto children = storyboard.Children();
                    const uint32_t count = children.Size();
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        visit(name, index, children.GetAt(index));
                    }
                }
            }
            return true;
        }
    }

    HWND ScrollBarHover::MainWindow()
    {
        if (m_main && IsWindow(m_main))
        {
            return m_main;
        }
        m_main = nullptr;
        try
        {
            if (const auto tabsBar = m_tabsBar.get())
            {
                if (const auto xamlRoot = tabsBar.XamlRoot())
                {
                    if (const auto environment = xamlRoot.ContentIslandEnvironment())
                    {
                        m_main = GetAncestor(interop::HwndFromWindowId(environment.AppWindowId()), GA_ROOT);
                    }
                }
            }
        }
        catch (...)
        {
        }
        return m_main;
    }

    void ScrollBarHover::Apply(Settings const& settings)
    {
        const HWND main = MainWindow();
        if (!main)
        {
            return; // ci riprova il prossimo Apply (la finestra si sta ancora costruendo)
        }
        // Sempre attiva (niente opzione, su richiesta dell'utente), tranne con la tray chiusa con "Esci". Resta il gate
        // per-finestra dello sviluppo: se esiste dev-windows.txt agiamo solo sulle finestre elencate.
        const bool wanted = WindowApplyAllowed(main) && !settings.trayExited;
        if (wanted && !m_enabled)
        {
            m_enabled = true;
            PatchAll();
        }
        else if (!wanted && m_enabled)
        {
            m_enabled = false;
            const int restored = RestoreAll();
            NPX_LOG(L"ScrollBarHover: %s: barre di scorrimento di nuovo con il ritardo di sistema (%d animazioni).",
                    settings.trayExited ? L"tray chiusa" : L"finestra esclusa (modalità sviluppo)", restored);
        }
    }

    void ScrollBarHover::OnTimer()
    {
        if (m_enabled)
        {
            PatchAll();
        }
    }

    void ScrollBarHover::Detach(bool restore)
    {
        if (restore && m_enabled)
        {
            RestoreAll();
        }
        m_enabled = false;
        m_bars.clear();
    }

    void ScrollBarHover::PatchAll()
    {
        if (!scrollBarsProvider)
        {
            return;
        }
        std::erase_if(m_bars, [](Bar const& entry) { return !entry.bar.get(); });
        int bars = 0;
        int timelines = 0;
        for (auto const& bar : scrollBarsProvider())
        {
            Bar* entry = nullptr;
            for (auto& existing : m_bars)
            {
                if (existing.bar.get() == bar)
                {
                    entry = &existing;
                    break;
                }
            }
            if (!entry)
            {
                m_bars.push_back({ winrt::make_weak(bar) });
                entry = &m_bars.back();
            }
            if (entry->done)
            {
                continue;
            }
            const size_t before = entry->timelines.size();
            try
            {
                entry->done = Patch(bar, *entry);
            }
            catch (...)
            {
                // Modello in costruzione o barra appena distrutta: si riprova al prossimo controllo.
            }
            if (entry->timelines.size() > before)
            {
                ++bars;
                timelines += static_cast<int>(entry->timelines.size() - before);
            }
        }
        if (bars == 0)
        {
            return;
        }
        if (!m_loggedFirst)
        {
            m_loggedFirst = true;
            NPX_LOG(L"ScrollBarHover: %d barre di scorrimento si allargano subito sotto il mouse (%d animazioni senza ritardo).", bars, timelines);
        }
        else
        {
            NPX_VLOG(L"ScrollBarHover: altre %d barre adattate (%d animazioni).", bars, timelines);
        }
    }

    bool ScrollBarHover::Patch(muxp::ScrollBar const& bar, Bar& entry)
    {
        bool complete = true;
        const bool templated = ForEachExpandTimeline(bar, [&](std::wstring const& state, uint32_t index, muxa::Timeline const& timeline) {
            const auto begin = timeline.BeginTime();
            if (!begin || begin.Value() <= TimeSpan::zero())
            {
                return; // già senza ritardo (anche adattata da noi)
            }
            try
            {
                const TimeSpan original = begin.Value();
                timeline.BeginTime(IReference<TimeSpan>(TimeSpan::zero()));
                entry.timelines.push_back({ state, index, original });
            }
            catch (...)
            {
                complete = false; // animazione in corso: si riprova
            }
        });
        return templated && complete; // senza modello (barra mai mostrata) si riprova
    }

    int ScrollBarHover::RestoreAll()
    {
        int restored = 0;
        for (auto const& entry : m_bars)
        {
            if (entry.timelines.empty())
            {
                continue;
            }
            const auto bar = entry.bar.get();
            if (!bar)
            {
                continue; // barra distrutta: niente da rimettere
            }
            try
            {
                ForEachExpandTimeline(bar, [&](std::wstring const& state, uint32_t index, muxa::Timeline const& timeline) {
                    for (auto const& patched : entry.timelines)
                    {
                        if (patched.index == index && patched.state == state)
                        {
                            try
                            {
                                timeline.BeginTime(IReference<TimeSpan>(patched.original));
                                ++restored;
                            }
                            catch (...)
                            {
                            }
                            break;
                        }
                    }
                });
            }
            catch (...)
            {
            }
        }
        m_bars.clear();
        return restored;
    }
}
