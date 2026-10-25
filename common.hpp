#pragma once
// ============================================================================
// common.hpp - UNICA definicion del logger y del estado global compartido.
//
// Historia: el repo tenia dos copias distintas de ss::log (una en main.cpp y
// otra aqui). Al ser `inline` con cuerpos distintos es una violacion de ODR y
// el resultado depende del enlazador. Ahora hay una sola, y main.cpp ya no
// define nada de esto.
//
// Salidas (todas en <carpeta del juego>\reframework\data\):
//   dmc5_ss.log        eventos normales (+ copia en el log de REFramework)
//   dmc5_ss_trace.log  llamadas del propio juego a las funciones vigiladas
//   dmc5_ss_dump.log   volcados de tipos/metodos/campos/enums
// Si no se puede abrir el archivo, se intenta %TEMP% y se deja constancia
// del codigo de error de Windows (GetLastError) en la primera linea.
// ============================================================================
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <reframework/API.h>
#include <reframework/API.hpp>

using REF = reframework::API;

namespace ss {

// Definido en main.cpp (fecha/hora de compilacion de ESA unidad).
const char* build_tag();

// ---- Estado global compartido ---------------------------------------------
inline std::atomic<int>      g_active_player{0};   // 0 = P1, 1 = P2 (ver input_isolation)
inline std::atomic<int>      g_in_hid{0};
inline std::atomic<int>      g_in_manager{0};
inline std::atomic<int>      g_in_character{0};
inline std::atomic<DWORD>    g_main_tid{0};
inline std::atomic<unsigned> g_frame{0};

// ---- Rutas ------------------------------------------------------------------
inline std::string exe_dir() {
    char path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(path, n);
    const auto slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : s.substr(0, slash + 1);
}

// <juego>\reframework\data\   (se crea si no existe)
inline const std::string& data_dir() {
    static const std::string dir = [] {
        const std::string base = exe_dir();
        if (base.empty()) return std::string{};
        CreateDirectoryA((base + "reframework").c_str(), nullptr);
        std::string d = base + "reframework\\data\\";
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

// ---- Sink de log -------------------------------------------------------------
class Sink {
public:
    explicit Sink(const char* file_name) : m_name(file_name) {}

    void write(const char* line) {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_tried) open_locked();

        char stamp[48];
        std::snprintf(stamp, sizeof(stamp), "[+%8.3f f%u] ", seconds(), g_frame.load(std::memory_order_relaxed));
        std::string out(stamp);
        out += line;
        out += "\r\n";

        if (m_handle != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!WriteFile(m_handle, out.data(), static_cast<DWORD>(out.size()), &written, nullptr)) {
                m_write_errors++;
                m_last_error = GetLastError();
            }
        }
        OutputDebugStringA(out.c_str());
    }

    // Fuerza la apertura (para conocer la ruta/errores en el banner inicial).
    void ensure_open() { std::lock_guard<std::mutex> lk(m_mtx); if (!m_tried) open_locked(); }
    std::string path()        { std::lock_guard<std::mutex> lk(m_mtx); return m_path; }
    DWORD       open_error()  { std::lock_guard<std::mutex> lk(m_mtx); return m_open_error; }
    bool        is_open()     { std::lock_guard<std::mutex> lk(m_mtx); return m_handle != INVALID_HANDLE_VALUE; }
    unsigned    write_errors(){ std::lock_guard<std::mutex> lk(m_mtx); return m_write_errors; }

private:
    static double seconds() {
        static const ULONGLONG t0 = GetTickCount64();
        return static_cast<double>(GetTickCount64() - t0) / 1000.0;
    }

    HANDLE try_open(const std::string& path) {
        return CreateFileA(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    void open_locked() {
        m_tried = true;
        const std::string primary = data_dir() + m_name;
        m_handle = data_dir().empty() ? INVALID_HANDLE_VALUE : try_open(primary);
        m_path = primary;
        if (m_handle == INVALID_HANDLE_VALUE) {
            m_open_error = GetLastError();
            char tmp[MAX_PATH]{};
            const DWORD n = GetTempPathA(MAX_PATH, tmp);
            if (n > 0 && n < MAX_PATH) {
                const std::string fallback = std::string(tmp) + m_name;
                m_handle = try_open(fallback);
                if (m_handle != INVALID_HANDLE_VALUE) {
                    m_path = fallback;
                    const std::string note = "AVISO: no se pudo abrir '" + primary + "' (error Windows " +
                                             std::to_string(m_open_error) + "). Se usa " + fallback + "\r\n";
                    DWORD w = 0;
                    WriteFile(m_handle, note.data(), static_cast<DWORD>(note.size()), &w, nullptr);
                }
            }
        }
    }

    std::mutex  m_mtx;
    std::string m_name;
    std::string m_path;
    HANDLE      m_handle = INVALID_HANDLE_VALUE;
    bool        m_tried = false;
    DWORD       m_open_error = 0;
    DWORD       m_last_error = 0;
    unsigned    m_write_errors = 0;
};

inline Sink g_log_sink{"dmc5_ss.log"};
inline Sink g_trace_sink{"dmc5_ss_trace.log"};
inline Sink g_dump_sink{"dmc5_ss_dump.log"};

namespace detail {
inline void vformat(char* buf, size_t cap, const char* fmt, va_list va) {
    std::vsnprintf(buf, cap, fmt, va);
    buf[cap - 1] = '\0';
}
} // namespace detail

// Evento normal: dmc5_ss.log + log de REFramework.
inline void log(const char* fmt, ...) {
    char buf[2048];
    va_list va; va_start(va, fmt);
    detail::vformat(buf, sizeof(buf), fmt, va);
    va_end(va);
    g_log_sink.write(buf);
    if (REF::get() != nullptr) REF::get()->log_info("[DMC5-SS] %s", buf);
}

// Solo dmc5_ss_trace.log (alto volumen).
inline void log_trace(const char* fmt, ...) {
    char buf[2048];
    va_list va; va_start(va, fmt);
    detail::vformat(buf, sizeof(buf), fmt, va);
    va_end(va);
    g_trace_sink.write(buf);
}

// Solo dmc5_ss_dump.log (volcados).
inline void log_dump(const char* fmt, ...) {
    char buf[2048];
    va_list va; va_start(va, fmt);
    detail::vformat(buf, sizeof(buf), fmt, va);
    va_end(va);
    g_dump_sink.write(buf);
}

} // namespace ss
