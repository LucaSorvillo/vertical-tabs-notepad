#include "pch.h"
#include "Interop.h"

namespace npx::interop
{
    namespace
    {
        // Firme di Microsoft.UI.Interop.h (Windows App SDK). WindowId è una struct { UINT64 Value; }
        // con lo stesso layout della proiezione C++/WinRT.
        using PFN_GetWindowIdFromWindow = HRESULT(WINAPI*)(HWND, winrt::Microsoft::UI::WindowId*);
        using PFN_GetWindowFromWindowId = HRESULT(WINAPI*)(winrt::Microsoft::UI::WindowId, HWND*);

        template <typename T>
        T UdkExport(const char* name)
        {
            const HMODULE udk = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
            return udk ? reinterpret_cast<T>(GetProcAddress(udk, name)) : nullptr;
        }
    }

    winrt::Microsoft::UI::WindowId WindowIdFromHwnd(HWND hwnd)
    {
        static const auto fn = UdkExport<PFN_GetWindowIdFromWindow>("Windowing_GetWindowIdFromWindow");
        winrt::Microsoft::UI::WindowId id{};
        if (fn && SUCCEEDED(fn(hwnd, &id)))
        {
            return id;
        }
        return winrt::Microsoft::UI::WindowId{ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd)) };
    }

    HWND HwndFromWindowId(winrt::Microsoft::UI::WindowId const& id)
    {
        static const auto fn = UdkExport<PFN_GetWindowFromWindowId>("Windowing_GetWindowFromWindowId");
        HWND hwnd = nullptr;
        if (fn && SUCCEEDED(fn(id, &hwnd)) && hwnd)
        {
            return hwnd;
        }
        return reinterpret_cast<HWND>(static_cast<uintptr_t>(id.Value));
    }

    RECT RectInClientOf(HWND child, HWND reference)
    {
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(HWND_DESKTOP, reference, reinterpret_cast<POINT*>(&rect), 2);
        return rect;
    }

    std::wstring ClassOf(HWND hwnd)
    {
        wchar_t name[256]{};
        GetClassNameW(hwnd, name, ARRAYSIZE(name));
        return name;
    }

    HWND FindDescendantByClass(HWND root, const wchar_t* className, bool requireVisible)
    {
        struct Context
        {
            const wchar_t* className;
            bool requireVisible;
            HWND result;
        } context{ className, requireVisible, nullptr };

        EnumChildWindows(root, [](HWND hwnd, LPARAM param) -> BOOL {
            auto& ctx = *reinterpret_cast<Context*>(param);
            if (ClassOf(hwnd) == ctx.className && (!ctx.requireVisible || IsWindowVisible(hwnd)))
            {
                ctx.result = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.result;
    }
}
