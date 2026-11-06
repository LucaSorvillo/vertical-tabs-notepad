#pragma once

namespace npx::interop
{
    // Conversioni HWND <-> WindowId del Windows App SDK. Usano le funzioni esportate da
    // Microsoft.Internal.FrameworkUdk.dll (le stesse di Microsoft.UI.Interop.h); se non sono
    // disponibili ripiegano sull'equivalenza WindowId.Value == HWND valida su desktop.
    winrt::Microsoft::UI::WindowId WindowIdFromHwnd(HWND hwnd);
    HWND HwndFromWindowId(winrt::Microsoft::UI::WindowId const& id);

    // Rettangolo di 'child' nelle coordinate client di 'reference'.
    RECT RectInClientOf(HWND child, HWND reference);

    // Primo discendente (anche indiretto) con il nome di classe indicato.
    HWND FindDescendantByClass(HWND root, const wchar_t* className, bool requireVisible);

    // Nome della classe di finestra.
    std::wstring ClassOf(HWND hwnd);

    // Le nostre isole sovrapposte all'editor (es. il pulsante "Torna su") hanno questa proprietà di
    // finestra: il layout delle schede verticali non le tratta come finestre del Blocco note.
    inline constexpr wchar_t kOverlayProp[] = L"VTabsNotepad.Overlay";
    inline bool IsOverlay(HWND hwnd)
    {
        return GetPropW(hwnd, kOverlayProp) != nullptr;
    }
}
