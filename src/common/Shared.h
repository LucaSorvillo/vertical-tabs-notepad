#pragma once
// Costanti, percorsi e utilità condivise tra il launcher (VTabsNotepad.exe)
// e la mod iniettata nel Blocco note (VTabsNotepadMod.dll).

#include <windows.h>

#include <string>
#include <string_view>

namespace npx
{
    inline constexpr wchar_t kAppName[] = L"VTabsNotepad";
    inline constexpr wchar_t kModDllName[] = L"VTabsNotepadMod.dll";

    // Lingua dei testi dell'app (menu della tray, menu e suggerimenti della mod): l'italiano se la lingua di
    // visualizzazione di Windows dell'utente è l'italiano, altrimenti l'inglese. Si legge una volta per
    // processo: cambiare la lingua di Windows richiede di disconnettersi.
    inline bool UiItalian()
    {
        static const bool italian = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN;
        return italian;
    }
    // Il testo nella lingua dell'app.
    inline const wchar_t* Tr(const wchar_t* english, const wchar_t* italian)
    {
        return UiItalian() ? italian : english;
    }

    // CLSID del TAP (Tool Access Point) che il runtime XAML istanzia dentro Notepad
    // quando chiamiamo InitializeXamlDiagnosticsEx.
    // {FF5B52B0-369E-4523-8ACA-EACA3766DD29}
    inline constexpr GUID kTapClsid{ 0xff5b52b0, 0x369e, 0x4523, { 0x8a, 0xca, 0xea, 0xca, 0x37, 0x66, 0xdd, 0x29 } };

    // %LOCALAPPDATA%\VTabsNotepad (creata se manca).
    std::wstring DataDirectory();
    // %LOCALAPPDATA%\VTabsNotepad\settings.ini
    std::wstring SettingsPath();
    // %LOCALAPPDATA%\VTabsNotepad\logs (creata se manca).
    std::wstring LogDirectory();
    // %LOCALAPPDATA%\VTabsNotepad\tray-exited.txt: c'è mentre la tray è chiusa con "Esci" (o --exit). La mod si spegne in
    // tutte le finestre del Blocco note come con le funzioni spente dal menu (anche quelle sempre attive), senza toccare
    // settings.ini, e si riaccende quando la tray riparte (che lo cancella). --exit --keep-mods (compilazione, prove) esce
    // senza crearlo.
    std::wstring TrayExitedPath();

    // Evento con nome con cui il launcher chiede a un processo Notepad di esportare l'albero XAML.
    std::wstring DumpTreeEventName(DWORD pid);
    // ...e con cui gli chiede di riaprire la scheda chiusa nella finestra attiva (come Ctrl+Maiusc+T).
    std::wstring ReopenTabEventName(DWORD pid);

    // Allowlist di finestre per lo sviluppo: %LOCALAPPDATA%\VTabsNotepad\dev-windows.txt, un valore HWND
    // (decimale) per riga. Serve perché il Blocco note moderno è un solo processo con più finestre:
    // così in sviluppo si agisce su una sola finestra, lasciando intatte le altre.
    std::wstring DevWindowsPath();

    // true se la finestra può essere modificata: se dev-windows.txt non esiste -> sempre true
    // (comportamento normale); se esiste -> true solo se l'HWND è nell'elenco.
    bool WindowApplyAllowed(HWND mainWindow);

    // true se il percorso è il Notepad.exe del pacchetto Store "Microsoft.WindowsNotepad"
    // (e non il vecchio notepad.exe di System32).
    bool IsStoreNotepadPath(std::wstring_view imagePath);

    // Percorso completo dell'eseguibile di un processo ("" se non accessibile).
    std::wstring ProcessImagePath(DWORD pid);

    // Confronto case-insensitive (ordinale) tra stringhe.
    bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);
    bool EndsWithIgnoreCase(std::wstring_view text, std::wstring_view suffix);
    bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part);
}
