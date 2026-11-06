#include "Shared.h"

#include <shlobj.h>

#include <cctype>

namespace npx
{
    std::wstring DataDirectory()
    {
        std::wstring result;
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)))
        {
            result = localAppData;
            result += L"\\";
            result += kAppName;
            CreateDirectoryW(result.c_str(), nullptr);
        }
        CoTaskMemFree(localAppData);
        return result;
    }

    std::wstring SettingsPath()
    {
        return DataDirectory() + L"\\settings.ini";
    }

    std::wstring LogDirectory()
    {
        std::wstring dir = DataDirectory() + L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    std::wstring DumpTreeEventName(DWORD pid)
    {
        return L"Local\\VTabsNotepad.DumpTree." + std::to_wstring(pid);
    }

    std::wstring ReopenTabEventName(DWORD pid)
    {
        return L"Local\\VTabsNotepad.ReopenTab." + std::to_wstring(pid);
    }

    std::wstring DevWindowsPath()
    {
        return DataDirectory() + L"\\dev-windows.txt";
    }

    bool WindowApplyAllowed(HWND mainWindow)
    {
        const std::wstring path = DevWindowsPath();
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return true; // nessuna allowlist: comportamento normale (tutte le finestre consentite)
        }

        char buffer[4096];
        DWORD read = 0;
        std::string content;
        while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            content.append(buffer, read);
        }
        CloseHandle(file);

        const unsigned long long target = reinterpret_cast<uintptr_t>(mainWindow);
        size_t pos = 0;
        while (pos < content.size())
        {
            while (pos < content.size() && !std::isdigit(static_cast<unsigned char>(content[pos])))
            {
                ++pos;
            }
            if (pos >= content.size())
            {
                break;
            }
            unsigned long long value = 0;
            while (pos < content.size() && std::isdigit(static_cast<unsigned char>(content[pos])))
            {
                value = value * 10 + static_cast<unsigned long long>(content[pos] - '0');
                ++pos;
            }
            if (value == target)
            {
                return true;
            }
        }
        return false;
    }

    bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
    {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    bool EndsWithIgnoreCase(std::wstring_view text, std::wstring_view suffix)
    {
        return text.size() >= suffix.size() && EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
    }

    bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view part)
    {
        if (part.size() > text.size())
        {
            return false;
        }
        for (size_t i = 0; i + part.size() <= text.size(); ++i)
        {
            if (EqualsIgnoreCase(text.substr(i, part.size()), part))
            {
                return true;
            }
        }
        return false;
    }

    bool IsStoreNotepadPath(std::wstring_view imagePath)
    {
        // Esempio: C:\Program Files\WindowsApps\Microsoft.WindowsNotepad_11.2607.14.0_x64__8wekyb3d8bbwe\Notepad\Notepad.exe
        return ContainsIgnoreCase(imagePath, L"\\Microsoft.WindowsNotepad_") && EndsWithIgnoreCase(imagePath, L"\\Notepad.exe");
    }

    std::wstring ProcessImagePath(DWORD pid)
    {
        std::wstring path;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            wchar_t buffer[MAX_PATH * 2];
            DWORD size = ARRAYSIZE(buffer);
            if (QueryFullProcessImageNameW(process, 0, buffer, &size))
            {
                path.assign(buffer, size);
            }
            CloseHandle(process);
        }
        return path;
    }
}
