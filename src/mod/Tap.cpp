// TAP (Tool Access Point): l'oggetto COM che il runtime XAML crea dentro Notepad dopo
// InitializeXamlDiagnosticsEx. Il runtime chiama IObjectWithSite::SetSite passando
// IXamlDiagnostics, la "porta d'ingresso" all'albero visuale dell'app.

#include "pch.h"

#include "Log.h"
#include "ModCore.h"
#include "Shared.h"

namespace npx
{
    namespace
    {
        struct TapSite : winrt::implements<TapSite, IObjectWithSite, winrt::non_agile>
        {
            HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override
            {
                m_site.copy_from(site);
                if (site)
                {
                    try
                    {
                        ModCore::Instance().OnXamlDiagnosticsReady(m_site.as<IXamlDiagnostics>());
                    }
                    catch (...)
                    {
                        NPX_LOG(L"SetSite: errore 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                    }
                }
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) noexcept override
            {
                if (!site)
                {
                    return E_POINTER;
                }
                *site = nullptr;
                return m_site ? m_site->QueryInterface(riid, site) : E_FAIL;
            }

        private:
            winrt::com_ptr<IUnknown> m_site;
        };

        struct TapClassFactory : winrt::implements<TapClassFactory, IClassFactory, winrt::non_agile>
        {
            HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** result) noexcept override
            {
                if (!result)
                {
                    return E_POINTER;
                }
                *result = nullptr;
                if (outer)
                {
                    return CLASS_E_NOAGGREGATION;
                }
                try
                {
                    return winrt::make_self<TapSite>()->QueryInterface(riid, result);
                }
                catch (...)
                {
                    return winrt::to_hresult();
                }
            }

            HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override
            {
                return S_OK;
            }
        };
    }
}

_Use_decl_annotations_ STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* result)
{
    if (!result)
    {
        return E_POINTER;
    }
    *result = nullptr;
    if (clsid != npx::kTapClsid)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    try
    {
        return winrt::make_self<npx::TapClassFactory>()->QueryInterface(riid, result);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

// La DLL non viene mai scaricata: il runtime XAML conserva riferimenti ai nostri oggetti.
STDAPI DllCanUnloadNow()
{
    return S_FALSE;
}
