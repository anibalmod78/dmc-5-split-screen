
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// API de REFramework: incluye los tipos de los hooks.
#include <reframework/API.h>
#include <reframework/API.hpp>

using REF = reframework::API;

namespace ss {

    inline std::atomic<int> g_active_player{ 0 };
    inline std::atomic<int> g_in_hid{ 0 };
    inline std::atomic<int> g_in_manager{ 0 };
    inline std::atomic<int> g_in_character{ 0 };
    inline std::atomic<DWORD> g_main_tid{ 0 };

    namespace detail {
        inline void write_line(const char* message) {
            char path[MAX_PATH]{};

            DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) {
                OutputDebugStringA(message);
                OutputDebugStringA("\n");
                return;
            }

            std::string file_path(path);
            const auto slash = file_path.find_last_of("\\/");
            if (slash != std::string::npos) {
                file_path.resize(slash + 1);
            }

            file_path += "reframework\\data\\dmc5_ss.log";

            HANDLE file = CreateFileA(
                file_path.c_str(),
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, message, static_cast<DWORD>(strlen(message)),
                    &written, nullptr);
                WriteFile(file, "\r\n", 2, &written, nullptr);
                CloseHandle(file);
            }

            OutputDebugStringA(message);
            OutputDebugStringA("\n");
        }
    }

    inline void log(const char* format, ...) {
        char buffer[2048]{};

        va_list args;
        va_start(args, format);
        vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
        va_end(args);

        if (REF::get() != nullptr) {
            REF::get()->log_info("%s", buffer);
        }

        detail::write_line(buffer);
    }

    inline void log_dump(const char* format, ...) {
        char buffer[2048]{};

        va_list args;
        va_start(args, format);
        vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
        va_end(args);

        detail::write_line(buffer);
    }

}
