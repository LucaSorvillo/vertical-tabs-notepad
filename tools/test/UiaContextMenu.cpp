// VTabsNotepadUiaTool <hwnd esadecimale> <prefisso nome> [tab]
// Strumento per i test: trova sotto la finestra il primo ListItem (riga del pannello; con "tab" il
// primo TabItem, cioè una scheda nativa) il cui nome inizia col prefisso e invoca
// IUIAutomationElement3::ShowContextMenu (come uno screen reader o il tasto Menu), così il menu
// contestuale si può provare senza mouse. Il wrapper .NET di UI Automation non espone questa funzione.
#include <windows.h>
#include <objbase.h>
#include <uiautomation.h>

#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"uso: VTabsNotepadUiaTool <hwnd> <prefisso> [tab]\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        return 3;
    }
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))))
    {
        wprintf(L"CUIAutomation8 non disponibile\n");
        return 3;
    }
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(argv[1], nullptr, 16)));
    IUIAutomationElement* window = nullptr;
    if (FAILED(uia->ElementFromHandle(hwnd, &window)) || !window)
    {
        wprintf(L"finestra non trovata\n");
        return 4;
    }
    VARIANT type{};
    type.vt = VT_I4;
    type.lVal = argc > 3 && _wcsicmp(argv[3], L"tab") == 0 ? UIA_TabItemControlTypeId : UIA_ListItemControlTypeId;
    IUIAutomationCondition* condition = nullptr;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition);
    IUIAutomationElementArray* items = nullptr;
    window->FindAll(TreeScope_Descendants, condition, &items);
    int count = 0;
    if (items)
    {
        items->get_Length(&count);
    }
    const size_t prefixLength = wcslen(argv[2]);
    for (int i = 0; i < count; ++i)
    {
        IUIAutomationElement* item = nullptr;
        items->GetElement(i, &item);
        BSTR name = nullptr;
        item->get_CurrentName(&name);
        const bool match = name && wcsncmp(name, argv[2], prefixLength) == 0;
        SysFreeString(name);
        if (!match)
        {
            continue;
        }
        IUIAutomationElement3* item3 = nullptr;
        if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item3))))
        {
            wprintf(L"IUIAutomationElement3 non disponibile\n");
            return 5;
        }
        const HRESULT hr = item3->ShowContextMenu();
        wprintf(L"ShowContextMenu: 0x%08X\n", static_cast<unsigned>(hr));
        return SUCCEEDED(hr) ? 0 : 6;
    }
    wprintf(L"elemento non trovato (%d ListItem)\n", count);
    return 1;
}
