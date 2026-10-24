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

#include <reframework/API.h>
#include <reframework/API.hpp>

using REF = reframework::API;

namespace ss {

    inline std::atomic<int> g_active_player{ 0 };
    inline std::atomic<int> g_in_hid{ 0 };
    inline std::atomic<int> g_in_manager{ 0 };
    inline std::atomic<int> g_in_character{ 0 };
    inline std::atomic<DWORD> g_main_tid{ 0 };

    inline std::string data_dir() {
        char path[MAX_PATH]{};

        DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return {};
        }

        std::string result(path);
        const auto slash = result.find_last_of("\\/");
        if (slash == std::string::npos) {
            return {};
        }

        result.resize(slash + 1);
        result += "reframework\\data\\";

        // Crear la carpeta reframework si no existe.
        std::string reframework_dir = result.substr(0, result.find("data\\"));
        if (!reframework_dir.empty()) {
            CreateDirectoryA(reframework_dir.c_str(), nullptr);
        }

        // Crear la carpeta data si no existe.
        CreateDirectoryA(result.c_str(), nullptr);

        return result;
    }

    namespace detail {

        inline void write_line(const char* message) {
            const std::string directory = data_dir();

            if (directory.empty()) {
                OutputDebugStringA(message);
                OutputDebugStringA("\n");
                return;
            }

            const std::string file_path = directory + "dmc5_ss.log";

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
                WriteFile(
                    file,
                    message,
                    static_cast<DWORD>(std::strlen(message)),
                    &written,
                    nullptr
                );
                WriteFile(file, "\r\n", 2, &written, nullptr);
                CloseHandle(file);
            }

            OutputDebugStringA(message);
            OutputDebugStringA("\n");
        }

    } // namespace detail

    inline void log(const char* format, ...) {
        char buffer[2048]{};

        va_list args;
        va_start(args, format);
        vsnprintf_s(
            buffer,
            sizeof(buffer),
            _TRUNCATE,
            format,
            args
        );
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
        vsnprintf_s(
            buffer,
            sizeof(buffer),
            _TRUNCATE,
            format,
            args
        );
        va_end(args);

        detail::write_line(buffer);
    }

} // namespace ss