#pragma once

namespace npx
{
    // Riceve dal runtime XAML (tramite IVisualTreeService3) le notifiche di aggiunta e
    // rimozione degli elementi dell'albero visuale e le inoltra a ModCore.
    class VisualTreeWatcher : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2, winrt::non_agile>
    {
    public:
        explicit VisualTreeWatcher(winrt::com_ptr<IXamlDiagnostics> diagnostics);

        // Registra il callback su un thread separato: AdviseVisualTreeChange enumera
        // l'albero esistente in modo sincrono sul thread UI, quindi chiamarlo dal thread
        // UI stesso andrebbe in deadlock.
        void Start();

        winrt::Windows::Foundation::IInspectable FromHandle(InstanceHandle handle) const;

        // IVisualTreeServiceCallback
        HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                                     VisualMutationType mutationType) noexcept override;
        // IVisualTreeServiceCallback2
        HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle element, VisualElementState elementState,
                                                        LPCWSTR context) noexcept override;

    private:
        winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
    };
}
