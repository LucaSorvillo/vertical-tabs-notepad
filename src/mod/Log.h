#pragma once
// Log su file: %LOCALAPPDATA%\VTabsNotepad\logs\mod.log (condiviso tra i processi, con PID e TID).

#include <sal.h>

namespace npx::log
{
    void Init();
    void SetVerbose(bool verbose);
    bool IsVerbose();
    void Write(_Printf_format_string_ const wchar_t* format, ...);
}

#define NPX_LOG(...) ::npx::log::Write(__VA_ARGS__)
#define NPX_VLOG(...)                       \
    do                                      \
    {                                       \
        if (::npx::log::IsVerbose())        \
            ::npx::log::Write(__VA_ARGS__); \
    } while (false)
