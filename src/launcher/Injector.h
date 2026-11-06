#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace npx
{
    struct NotepadProcess
    {
        DWORD pid = 0;
        std::wstring imagePath;
    };

    // Processi Notepad (versione Store) in esecuzione.
    std::vector<NotepadProcess> FindNotepadProcesses();

    // true se nel processo è già caricata una VTabsNotepadMod.dll (di qualunque versione).
    bool IsModLoaded(DWORD pid);

    // Copia la DLL della mod in %LOCALAPPDATA%\VTabsNotepad\runtime\<stamp>\ e restituisce il percorso
    // della copia: così la build originale non resta bloccata finché Notepad è aperto.
    std::wstring PrepareModDll(const std::wstring& sourceDll);

    // Inietta la DLL nel processo (CreateRemoteThread + LoadLibraryW).
    // In caso di errore restituisce false e descrive il problema in 'error'.
    bool InjectDll(DWORD pid, const std::wstring& dllPath, std::wstring& error);
}
