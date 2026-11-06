#include "pch.h"
#include "TreeDump.h"

#include "Log.h"
#include "Shared.h"

namespace npx::TreeDump
{
    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;

        std::wstring Num(double value)
        {
            if (std::isnan(value))
            {
                return L"NaN";
            }
            if (std::isinf(value))
            {
                return L"Inf";
            }
            wchar_t buffer[32];
            swprintf_s(buffer, L"%g", value);
            return buffer;
        }

        std::wstring Thickness(mux::Thickness const& t)
        {
            return Num(t.Left) + L"," + Num(t.Top) + L"," + Num(t.Right) + L"," + Num(t.Bottom);
        }

        bool IsZero(mux::Thickness const& t)
        {
            return t.Left == 0 && t.Top == 0 && t.Right == 0 && t.Bottom == 0;
        }

        std::wstring GridLength(mux::GridLength const& length)
        {
            switch (length.GridUnitType)
            {
            case mux::GridUnitType::Auto:
                return L"Auto";
            case mux::GridUnitType::Star:
                return Num(length.Value) + L"*";
            default:
                return Num(length.Value);
            }
        }

        const wchar_t* Alignment(mux::HorizontalAlignment value)
        {
            switch (value)
            {
            case mux::HorizontalAlignment::Left: return L"Left";
            case mux::HorizontalAlignment::Center: return L"Center";
            case mux::HorizontalAlignment::Right: return L"Right";
            default: return L"Stretch";
            }
        }

        const wchar_t* Alignment(mux::VerticalAlignment value)
        {
            switch (value)
            {
            case mux::VerticalAlignment::Top: return L"Top";
            case mux::VerticalAlignment::Center: return L"Center";
            case mux::VerticalAlignment::Bottom: return L"Bottom";
            default: return L"Stretch";
            }
        }

        std::wstring Quote(winrt::hstring const& text)
        {
            std::wstring value{ text };
            for (auto& ch : value)
            {
                if (ch == L'\r' || ch == L'\n' || ch == L'\t')
                {
                    ch = L' ';
                }
            }
            if (value.size() > 80)
            {
                value = value.substr(0, 80) + L"…";
            }
            return L"\"" + value + L"\"";
        }

        std::wstring BrushInfo(muxm::Brush const& brush)
        {
            if (!brush)
            {
                return {};
            }
            if (const auto solid = brush.try_as<muxm::SolidColorBrush>())
            {
                const auto c = solid.Color();
                wchar_t buffer[32];
                swprintf_s(buffer, L"#%02X%02X%02X%02X", c.A, c.R, c.G, c.B);
                return buffer;
            }
            return std::wstring{ winrt::get_class_name(brush) };
        }

        void DumpNode(std::wstring& out, mux::DependencyObject const& node, mux::UIElement const& root, int depth)
        {
            std::wstring line(static_cast<size_t>(depth) * 2, L' ');
            line += winrt::get_class_name(node);

            if (const auto fe = node.try_as<mux::FrameworkElement>())
            {
                if (!fe.Name().empty())
                {
                    line += L" #" + std::wstring{ fe.Name() };
                }
                line += L" [" + Num(fe.ActualWidth()) + L"x" + Num(fe.ActualHeight()) + L"]";
                try
                {
                    const auto origin = fe.TransformToVisual(root).TransformPoint({ 0, 0 });
                    line += L" @" + Num(std::round(origin.X)) + L"," + Num(std::round(origin.Y));
                }
                catch (...)
                {
                }

                if (fe.Visibility() == mux::Visibility::Collapsed)
                {
                    line += L" COLLAPSED";
                }
                if (fe.Opacity() != 1.0)
                {
                    line += L" opacity=" + Num(fe.Opacity());
                }
                if (!std::isnan(fe.Width()))
                {
                    line += L" W=" + Num(fe.Width());
                }
                if (!std::isnan(fe.Height()))
                {
                    line += L" H=" + Num(fe.Height());
                }
                if (fe.MinWidth() != 0)
                {
                    line += L" minW=" + Num(fe.MinWidth());
                }
                if (!std::isinf(fe.MaxWidth()))
                {
                    line += L" maxW=" + Num(fe.MaxWidth());
                }
                if (fe.MinHeight() != 0)
                {
                    line += L" minH=" + Num(fe.MinHeight());
                }
                if (!std::isinf(fe.MaxHeight()))
                {
                    line += L" maxH=" + Num(fe.MaxHeight());
                }
                if (!IsZero(fe.Margin()))
                {
                    line += L" margin=" + Thickness(fe.Margin());
                }
                if (fe.HorizontalAlignment() != mux::HorizontalAlignment::Stretch)
                {
                    line += L" hAlign=" + std::wstring{ Alignment(fe.HorizontalAlignment()) };
                }
                if (fe.VerticalAlignment() != mux::VerticalAlignment::Stretch)
                {
                    line += L" vAlign=" + std::wstring{ Alignment(fe.VerticalAlignment()) };
                }

                const int row = muxc::Grid::GetRow(fe);
                const int column = muxc::Grid::GetColumn(fe);
                const int rowSpan = muxc::Grid::GetRowSpan(fe);
                const int columnSpan = muxc::Grid::GetColumnSpan(fe);
                if (row || column || rowSpan != 1 || columnSpan != 1)
                {
                    line += L" grid(r" + std::to_wstring(row) + L",c" + std::to_wstring(column);
                    if (rowSpan != 1)
                    {
                        line += L",rs" + std::to_wstring(rowSpan);
                    }
                    if (columnSpan != 1)
                    {
                        line += L",cs" + std::to_wstring(columnSpan);
                    }
                    line += L")";
                }

                const auto automationId = mux::Automation::AutomationProperties::GetAutomationId(fe);
                if (!automationId.empty())
                {
                    line += L" aid=" + Quote(automationId);
                }
                const auto automationName = mux::Automation::AutomationProperties::GetName(fe);
                if (!automationName.empty())
                {
                    line += L" aname=" + Quote(automationName);
                }
            }

            if (const auto control = node.try_as<muxc::Control>())
            {
                if (!IsZero(control.Padding()))
                {
                    line += L" padding=" + Thickness(control.Padding());
                }
                if (const auto background = BrushInfo(control.Background()); !background.empty())
                {
                    line += L" bg=" + background;
                }
            }
            else if (const auto panel = node.try_as<muxc::Panel>())
            {
                if (const auto background = BrushInfo(panel.Background()); !background.empty())
                {
                    line += L" bg=" + background;
                }
            }
            else if (const auto border = node.try_as<muxc::Border>())
            {
                if (const auto background = BrushInfo(border.Background()); !background.empty())
                {
                    line += L" bg=" + background;
                }
                if (!IsZero(border.BorderThickness()))
                {
                    line += L" border=" + Thickness(border.BorderThickness()) + L" " + BrushInfo(border.BorderBrush());
                }
            }

            if (const auto grid = node.try_as<muxc::Grid>())
            {
                if (grid.RowDefinitions().Size() > 0)
                {
                    line += L" rows={";
                    for (auto const& definition : grid.RowDefinitions())
                    {
                        line += GridLength(definition.Height()) + L";";
                    }
                    line += L"}";
                }
                if (grid.ColumnDefinitions().Size() > 0)
                {
                    line += L" cols={";
                    for (auto const& definition : grid.ColumnDefinitions())
                    {
                        line += GridLength(definition.Width()) + L";";
                    }
                    line += L"}";
                }
            }
            if (const auto stack = node.try_as<muxc::StackPanel>())
            {
                line += stack.Orientation() == muxc::Orientation::Horizontal ? L" horizontal" : L" vertical";
            }
            if (const auto textBlock = node.try_as<muxc::TextBlock>())
            {
                line += L" text=" + Quote(textBlock.Text());
            }
            if (const auto contentControl = node.try_as<muxc::ContentControl>())
            {
                const auto content = contentControl.Content();
                if (const auto text = winrt::unbox_value_or<winrt::hstring>(content, L""); !text.empty())
                {
                    line += L" content=" + Quote(text);
                }
                else if (content && !content.try_as<mux::UIElement>())
                {
                    line += L" content:" + std::wstring{ winrt::get_class_name(content) };
                }
            }

            out += line;
            out += L"\n";

            const int count = muxm::VisualTreeHelper::GetChildrenCount(node);
            for (int i = 0; i < count; ++i)
            {
                DumpNode(out, muxm::VisualTreeHelper::GetChild(node, i), root, depth + 1);
            }
        }

        void DumpTabsBar(std::wstring& out, winrt::NotepadXamlUI::TabsBar const& tabsBar)
        {
            out += L"\n=== TabsBar: SelectedIndex=" + std::to_wstring(tabsBar.SelectedIndex()) + L"\n";
            const auto items = tabsBar.TabItems();
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                const auto item = items.GetAt(i);
                out += L"  [" + std::to_wstring(i) + L"] title=" + Quote(item.Title()) + L" tooltip=" + Quote(item.Tooltip()) +
                       L" modified=" + (item.IsModified() ? L"1" : L"0") + L" activated=" + (item.IsActivated() ? L"1" : L"0") +
                       L" closeShown=" + (item.IsCloseButtonShown() ? L"1" : L"0") + L"\n";
            }
        }
    }

    std::wstring DumpWindow(mux::UIElement const& element, std::wstring_view reason)
    {
        static std::atomic<int> counter{ 0 };
        std::wstring path;
        try
        {
            // Risale fino alla radice reale (sopra Window.Content ci sono gli elementi interni di WinUI).
            mux::DependencyObject top = element;
            while (const auto parent = muxm::VisualTreeHelper::GetParent(top))
            {
                top = parent;
            }
            const auto root = top.as<mux::UIElement>();

            std::wstring out = L"VTabsNotepad - albero visuale (" + std::wstring{ reason } + L")\n\n";
            DumpNode(out, top, root, 0);

            if (const auto xamlRoot = element.XamlRoot())
            {
                const auto popups = muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(xamlRoot);
                for (auto const& popup : popups)
                {
                    out += L"\n=== Popup aperto\n";
                    DumpNode(out, popup, root, 1);
                }
            }

            if (const auto tabsBar = element.try_as<winrt::NotepadXamlUI::TabsBar>())
            {
                DumpTabsBar(out, tabsBar);
            }

            path = LogDirectory() + L"\\tree-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter) + L".txt";
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), static_cast<int>(out.size()), nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<size_t>(bytes), '\0');
                WideCharToMultiByte(CP_UTF8, 0, out.c_str(), static_cast<int>(out.size()), utf8.data(), bytes, nullptr, nullptr);
                DWORD written = 0;
                WriteFile(file, "\xEF\xBB\xBF", 3, &written, nullptr);
                WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
                CloseHandle(file);
            }
            NPX_LOG(L"Albero visuale esportato in %s", path.c_str());
        }
        catch (...)
        {
            NPX_LOG(L"DumpWindow: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
        }
        return path;
    }
}
