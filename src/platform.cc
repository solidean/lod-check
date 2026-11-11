#include "lod_check.hh"

#include <cstdio>
#include <format>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <sys/utsname.h>

#include <fstream>
#endif

namespace lod_check
{
#if defined(_WIN32)

std::string os_description()
{
    // GetVersionEx lies to unmanifested programs; RtlGetVersion does not.
    using rtl_get_version = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto info = OSVERSIONINFOW{.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW)};
    if (auto const ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto const get = reinterpret_cast<rtl_get_version>(GetProcAddress(ntdll, "RtlGetVersion")))
            get(&info);
    return std::format("Windows {}.{}.{}", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
}

void wait_if_own_console()
{
    // A console started by a double-click holds this process alone; one started from a shell holds the shell too.
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1)
    {
        std::fputs("\nPress Enter to close.", stdout);
        std::getchar();
    }
}

#elif defined(__linux__)

std::string os_description()
{
    auto name = std::string("Linux");
    auto file = std::ifstream("/etc/os-release");
    for (auto line = std::string(); std::getline(file, line);)
        if (line.starts_with("PRETTY_NAME="))
        {
            name = line.substr(12);
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
        }
    auto un = utsname();
    if (uname(&un) == 0)
        name += std::format(", kernel {}", un.release);
    return name;
}

void wait_if_own_console()
{
}

#elif defined(__APPLE__)

// os_description lives in metal.mm, which already speaks Foundation.
void wait_if_own_console()
{
}

#endif
} // namespace lod_check
