#include "pch.h"
#include "TabColors.h"

#include "Log.h"
#include "Shared.h"

#include <appmodel.h>

namespace npx::tabcolors
{
    namespace
    {
        constexpr winrt::Windows::UI::Color Rgb(uint32_t rgb)
        {
            return { 0xFF, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb) };
        }

        constexpr auto kWhite = Rgb(0xFFFFFF);
        constexpr auto kInk = Rgb(0x202124);

        // Toni chiari per il tema scuro e saturi per il tema chiaro, come i gruppi di schede di Chrome.
        constexpr Swatch kPalette[] = {
            { TabColor::Grey, L"grey", L"Grigio", Rgb(0x5F6368), Rgb(0xBDC1C6), kWhite, kInk },
            { TabColor::Blue, L"blue", L"Blu", Rgb(0x1A73E8), Rgb(0x8AB4F8), kWhite, kInk },
            { TabColor::Red, L"red", L"Rosso", Rgb(0xD93025), Rgb(0xF28B82), kWhite, kInk },
            { TabColor::Yellow, L"yellow", L"Giallo", Rgb(0xF9AB00), Rgb(0xFDD663), kInk, kInk },
            { TabColor::Green, L"green", L"Verde", Rgb(0x188038), Rgb(0x81C995), kWhite, kInk },
            { TabColor::Pink, L"pink", L"Rosa", Rgb(0xD01884), Rgb(0xFF8BCB), kWhite, kInk },
            { TabColor::Purple, L"purple", L"Viola", Rgb(0x9334E6), Rgb(0xC58AF9), kWhite, kInk },
            { TabColor::Cyan, L"cyan", L"Azzurro", Rgb(0x007B83), Rgb(0x78D9EC), kWhite, kInk },
            { TabColor::Orange, L"orange", L"Arancione", Rgb(0xFA903E), Rgb(0xFCAD70), kInk, kInk },
        };

        constexpr wchar_t kSection[] = L"Colors";

        std::mutex g_lock;
        bool g_loaded = false;
        std::unordered_map<std::wstring, TabColor> g_colors; // chiave: "{GUID}" maiuscolo

        std::wstring FilePath()
        {
            return DataDirectory() + L"\\tab-colors.ini";
        }

        std::wstring KeyOf(winrt::guid const& id)
        {
            wchar_t buffer[64]{};
            StringFromGUID2(reinterpret_cast<GUID const&>(id), buffer, ARRAYSIZE(buffer));
            return buffer;
        }

        std::wstring Upper(std::wstring text)
        {
            CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
            return text;
        }

        // Cartella TabState della sessione del Blocco note (stessa identità di pacchetto del processo).
        std::wstring TabStateDirectory()
        {
            UINT32 length = 0;
            if (GetCurrentPackageFamilyName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0)
            {
                return {};
            }
            std::wstring family(length, L'\0');
            if (GetCurrentPackageFamilyName(&length, family.data()) != ERROR_SUCCESS)
            {
                return {};
            }
            family.resize(wcslen(family.c_str()));
            wchar_t localAppData[MAX_PATH]{};
            if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData)))
            {
                return {};
            }
            return std::wstring(localAppData) + L"\\Packages\\" + family + L"\\LocalState\\TabState";
        }

        // Le schede chiuse per sempre non hanno più uno stato nella sessione (TabState\<id>.bin): i loro
        // colori si eliminano. Se nessun colore salvato corrisponde a uno stato, per prudenza non si tocca nulla.
        void PruneLocked(std::wstring const& file)
        {
            const std::wstring directory = TabStateDirectory();
            if (g_colors.empty() || directory.empty())
            {
                return;
            }
            std::unordered_set<std::wstring> alive;
            WIN32_FIND_DATAW find{};
            const HANDLE search = FindFirstFileW((directory + L"\\*.bin").c_str(), &find);
            if (search == INVALID_HANDLE_VALUE)
            {
                return;
            }
            do
            {
                const std::wstring name = find.cFileName;
                alive.insert(L"{" + Upper(name.substr(0, name.find(L'.'))) + L"}");
            } while (FindNextFileW(search, &find));
            FindClose(search);

            const bool anyAlive = std::any_of(g_colors.begin(), g_colors.end(), [&](auto const& entry) { return alive.contains(entry.first); });
            if (!anyAlive)
            {
                return;
            }
            for (auto it = g_colors.begin(); it != g_colors.end();)
            {
                if (alive.contains(it->first))
                {
                    ++it;
                    continue;
                }
                WritePrivateProfileStringW(kSection, it->first.c_str(), nullptr, file.c_str());
                it = g_colors.erase(it);
            }
        }

        void LoadLocked()
        {
            g_loaded = true;
            const std::wstring file = FilePath();
            std::vector<wchar_t> buffer(64 * 1024);
            const DWORD length = GetPrivateProfileSectionW(kSection, buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
            for (const wchar_t* entry = buffer.data(); entry < buffer.data() + length && *entry; entry += wcslen(entry) + 1)
            {
                const std::wstring_view line{ entry };
                const size_t equals = line.find(L'=');
                if (equals == std::wstring_view::npos)
                {
                    continue;
                }
                const std::wstring_view value = line.substr(equals + 1);
                for (auto const& swatch : kPalette)
                {
                    if (EqualsIgnoreCase(value, swatch.key))
                    {
                        g_colors[Upper(std::wstring(line.substr(0, equals)))] = swatch.color;
                        break;
                    }
                }
            }
            PruneLocked(file);
            NPX_LOG(L"Colori delle schede: %zu salvati.", g_colors.size());
        }
    }

    std::span<const Swatch> Palette()
    {
        return kPalette;
    }

    Swatch const* Find(TabColor color)
    {
        for (auto const& swatch : kPalette)
        {
            if (swatch.color == color)
            {
                return &swatch;
            }
        }
        return nullptr;
    }

    TabColor Get(winrt::guid const& tabId)
    {
        std::scoped_lock lock(g_lock);
        if (!g_loaded)
        {
            LoadLocked();
        }
        const auto it = g_colors.find(KeyOf(tabId));
        return it == g_colors.end() ? TabColor::None : it->second;
    }

    void Set(winrt::guid const& tabId, TabColor color)
    {
        std::scoped_lock lock(g_lock);
        if (!g_loaded)
        {
            LoadLocked();
        }
        const std::wstring key = KeyOf(tabId);
        const Swatch* swatch = Find(color);
        if (swatch)
        {
            g_colors[key] = color;
        }
        else
        {
            g_colors.erase(key);
        }
        WritePrivateProfileStringW(kSection, key.c_str(), swatch ? swatch->key : nullptr, FilePath().c_str());
    }
}
