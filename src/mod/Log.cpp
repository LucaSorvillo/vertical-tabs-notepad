#include "pch.h"
#include "Log.h"

#include "Shared.h"

#include <cstdarg>
#include <cstdio>

namespace npx::log
{
    namespace
    {
        std::mutex g_lock;
        std::wstring g_path;    // mod.log (vuoto finché Init non è stata chiamata)
        std::wstring g_oldPath; // mod.old.log
        unsigned g_writes = 0;
        std::atomic<bool> g_verbose{ false };
        constexpr LONGLONG kMaxLogSize = 2 * 1024 * 1024;

        // Rotazione semplice: oltre 2 MB il file diventa mod.old.log. Sotto g_lock.
        void RotateIfLarge()
        {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &info))
            {
                const LONGLONG size = (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                if (size > kMaxLogSize)
                {
                    MoveFileExW(g_path.c_str(), g_oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
            }
        }
    }

    void Init()
    {
        std::scoped_lock lock(g_lock);
        if (!g_path.empty())
        {
            return;
        }
        g_path = LogDirectory() + L"\\mod.log";
        g_oldPath = LogDirectory() + L"\\mod.old.log";
        RotateIfLarge();
    }

    void SetVerbose(bool verbose)
    {
        g_verbose = verbose;
    }

    bool IsVerbose()
    {
        return g_verbose;
    }

    void Write(const wchar_t* format, ...)
    {
        wchar_t message[2048];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(message, _TRUNCATE, format, args);
        va_end(args);

        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t line[2200];
        _snwprintf_s(line, _TRUNCATE, L"%02u:%02u:%02u.%03u [%5lu:%5lu] %s\r\n", now.wHour, now.wMinute, now.wSecond,
                     now.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), message);

        OutputDebugStringW(line);

        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1)
        {
            return;
        }
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);

        std::scoped_lock lock(g_lock);
        if (g_path.empty())
        {
            return;
        }
        // Il file si apre a ogni riga: anche le istanze di un attimo del Blocco note (aprendo un file da fuori ne nasce
        // una che passa il file a quella vera e termina) caricano la mod; chi ruota il log (rinominandolo) non deve
        // lasciare il Blocco note vero a scrivere per sempre nel file vecchio, come succedeva tenendo aperto un handle.
        if (++g_writes % 256 == 0)
        {
            RotateIfLarge();
        }
        const HANDLE file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
}
