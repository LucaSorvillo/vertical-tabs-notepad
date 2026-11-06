#include "Settings.h"
#include "Shared.h"

#include <algorithm>
#include <string>

namespace npx
{
    namespace
    {
        bool ReadBool(const std::wstring& file, const wchar_t* section, const wchar_t* key, bool fallback)
        {
            return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, file.c_str()) != 0;
        }

        bool Write(const std::wstring& file, const wchar_t* section, const wchar_t* key, const std::wstring& value)
        {
            return WritePrivateProfileStringW(section, key, value.c_str(), file.c_str()) != FALSE;
        }
    }

    Settings Settings::Load()
    {
        const std::wstring file = SettingsPath();
        Settings s;

        s.verticalTabs = ReadBool(file, L"VerticalTabs", L"Enabled", s.verticalTabs);
        s.paneWidth = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"VerticalTabs", L"Width", s.paneWidth, file.c_str())), kMinPaneWidth, kMaxPaneWidth);
        s.collapsed = ReadBool(file, L"VerticalTabs", L"Collapsed", s.collapsed);

        s.backToTop = ReadBool(file, L"BackToTop", L"Enabled", s.backToTop);
        s.reopenTab = ReadBool(file, L"ReopenTab", L"Enabled", s.reopenTab);
        s.noSmoothScroll = ReadBool(file, L"SmoothScroll", L"Disabled", s.noSmoothScroll);

        s.dumpTreeOnAttach = ReadBool(file, L"Debug", L"DumpTreeOnAttach", s.dumpTreeOnAttach);
        s.verboseLog = ReadBool(file, L"Debug", L"VerboseLog", s.verboseLog);
        return s;
    }

    bool Settings::Save() const
    {
        const std::wstring file = SettingsPath();
        bool ok = true;
        ok &= Write(file, L"VerticalTabs", L"Enabled", verticalTabs ? L"1" : L"0");
        ok &= Write(file, L"VerticalTabs", L"Width", std::to_wstring(paneWidth));
        // Chiavi delle versioni con le opzioni "Pannello a destra" e "Ricerca nelle schede" (tolte).
        WritePrivateProfileStringW(L"VerticalTabs", L"Position", nullptr, file.c_str());
        WritePrivateProfileStringW(L"VerticalTabs", L"SearchBox", nullptr, file.c_str());
        ok &= Write(file, L"VerticalTabs", L"Collapsed", collapsed ? L"1" : L"0");
        ok &= Write(file, L"BackToTop", L"Enabled", backToTop ? L"1" : L"0");
        ok &= Write(file, L"ReopenTab", L"Enabled", reopenTab ? L"1" : L"0");
        ok &= Write(file, L"SmoothScroll", L"Disabled", noSmoothScroll ? L"1" : L"0");
        // Sezione della versione con l'opzione "Barre di scorrimento subito larghe" (tolta: ora sempre attiva).
        WritePrivateProfileStringW(L"ScrollBars", nullptr, nullptr, file.c_str());
        ok &= Write(file, L"Debug", L"DumpTreeOnAttach", dumpTreeOnAttach ? L"1" : L"0");
        ok &= Write(file, L"Debug", L"VerboseLog", verboseLog ? L"1" : L"0");
        return ok;
    }
}
