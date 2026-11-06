#include "pch.h"

#include "ModCore.h"

HMODULE g_module = nullptr;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);

        // Sotto il loader lock non si può fare quasi nulla: il lavoro vero parte su un thread.
        if (HANDLE thread = CreateThread(nullptr, 0, &npx::ModCore::BootstrapThreadProc, nullptr, 0, nullptr))
        {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
