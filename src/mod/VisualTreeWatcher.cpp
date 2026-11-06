#include "pch.h"
#include "VisualTreeWatcher.h"

#include "Log.h"
#include "ModCore.h"

namespace npx
{
    VisualTreeWatcher::VisualTreeWatcher(winrt::com_ptr<IXamlDiagnostics> diagnostics) :
        m_diagnostics(std::move(diagnostics))
    {
    }

    void VisualTreeWatcher::Start()
    {
        // Il thread possiede un riferimento al watcher finché Advise non ritorna.
        winrt::com_ptr<VisualTreeWatcher> self = get_strong();
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](void* parameter) -> DWORD {
                winrt::com_ptr<VisualTreeWatcher> watcher;
                watcher.attach(static_cast<VisualTreeWatcher*>(parameter));

                const HRESULT hr = watcher->m_diagnostics.as<IVisualTreeService3>()->AdviseVisualTreeChange(watcher.get());
                NPX_LOG(L"AdviseVisualTreeChange: 0x%08X", static_cast<unsigned>(hr));
                return 0;
            },
            self.get(), 0, nullptr);

        if (thread)
        {
            self.detach();
            CloseHandle(thread);
        }
        else
        {
            NPX_LOG(L"CreateThread per AdviseVisualTreeChange fallita: %lu", GetLastError());
        }
    }

    winrt::Windows::Foundation::IInspectable VisualTreeWatcher::FromHandle(InstanceHandle handle) const
    {
        winrt::Windows::Foundation::IInspectable object;
        winrt::check_hresult(m_diagnostics->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object))));
        return object;
    }

    HRESULT VisualTreeWatcher::OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                                  VisualMutationType mutationType) noexcept
    {
        try
        {
            if (mutationType == Add)
            {
                ModCore::Instance().OnElementAdded(*this, relation, element);
            }
            else if (mutationType == Remove)
            {
                ModCore::Instance().OnElementRemoved(*this, relation, element);
            }
        }
        catch (...)
        {
            NPX_LOG(L"Eccezione in OnVisualTreeChange: 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
        }
        // Non restituire mai errori al runtime XAML.
        return S_OK;
    }

    HRESULT VisualTreeWatcher::OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept
    {
        return S_OK;
    }
}
