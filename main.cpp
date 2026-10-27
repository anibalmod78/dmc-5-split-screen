// ============================================================================
// main.cpp - punto de entrada del plugin (REFramework) y bucle por frame.
//
// IMPORTANTE: este archivo DEBE contener reframework_plugin_initialize y
// reframework_plugin_required_version. En el repo anterior era una copia de
// common.hpp, por eso F7 nunca hacia nada.
//
// Mandos de control (todos hacen lo mismo por dos vias):
//   1) Teclas, con la ventana del juego en primer plano.
//   2) Un archivo  <juego>\reframework\data\dmc5_ss_cmd.txt  con una orden por
//      linea; el plugin lo lee cada ~0,3 s y lo BORRA. Sirve aunque el teclado
//      no responda y permite lanzar ordenes sin tocar el juego.
//
//   orden       tecla   que hace
//   ping        F5      escribe "pong" (prueba que el canal funciona)
//   state       F6      estado de jugadores, cargador adicional y camara
//   dump        F7      vuelca los tipos de dmc5_ss_dump_request.txt
//   recon       F8      volcado interno completo (enums, cargador, camara...)
//   pad         F11     sonda de mandos
//   --- P2 (RePag/AvPag eligen personaje: Nero, Dante, Vergil, Gilver) ---
//   p2adopt <pj>  F9    adopta un Player ya existente como P2
//   p2load <pj>   F10   pide carga por AdditionalPlayerLoadManager
//   p2warp        F12   teletransporta P2 junto a P1 (Ctrl+F12: variante requestWarp)
//   p2manual      Ctrl+F5   playerControlType = Manual
//   p2auto        Ctrl+F6   playerControlType = Auto (IA)
//   p2pad on 1    Ctrl+F7   P2 lee el mando de UserIndex 1
//   p2pad off     Ctrl+F8   restaura
//   p2pad status  Ctrl+F9   estado del desvio de mando
//   p2activate    Ctrl+F10  activatePlayerResource() del cargador (tras p2load)
//   p2finish      Ctrl+F11  requestFinishPlayerResource
//   (p2spawn queda solo por archivo; esta obsoleto)
//   trace on|off        activa/desactiva el trazado del flujo nativo
//   trace sum           resumen de hooks
// ============================================================================
#include "common.hpp"
#include "camera_manager.hpp"
#include "diagnostics.hpp"
#include "input_isolation.hpp"
#include "player_manager.hpp"
#include "p2_input.hpp"
#include "tracer.hpp"

#include <fstream>
#include <sstream>

namespace ss {
const char* build_tag() { return __DATE__ " " __TIME__; }
} // namespace ss

namespace {

bool g_player_init_done = false;
bool g_trace_done       = false;
bool g_p2input_done = false;

// mods: 0 = sin modificador, 1 = Ctrl, 2 = Shift. Solo dispara con la ventana
// del juego en primer plano y exactamente con ese modificador.
bool key_pressed(int vk, int mods = 0) {
    static bool prev[256 * 3] = {};
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool fg    = pid == GetCurrentProcessId();
    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool mods_ok = (mods == 0 && !ctrl && !shift) || (mods == 1 && ctrl && !shift) || (mods == 2 && shift && !ctrl);
    const bool down = fg && mods_ok && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool& p = prev[(vk & 0xFF) + 256 * mods];
    const bool edge = down && !p;
    p = down;
    return edge;
}

// Personaje elegido para P2 (RePag/AvPag lo cambian). Nombres = enum app.PlayerID.
const char* const kChars[] = {"Nero", "Dante", "Vergil", "Gilver"};
int g_p2_char = 0;

void select_char(int delta) {
    const int n = static_cast<int>(sizeof(kChars) / sizeof(kChars[0]));
    g_p2_char = (g_p2_char + delta + n) % n;
    ss::log("P2: personaje elegido = %s (RePag/AvPag para cambiar; F9 adopta, F10 carga)", kChars[g_p2_char]);
}

void run_command(const std::string& full) {
    ss::log("ORDEN: %s", full.c_str());
    // "orden argumento": el argumento solo se usa en p2spawn
    const auto sp = full.find(' ');
    const std::string cmd = full.substr(0, sp);
    const std::string arg = sp == std::string::npos ? std::string() : full.substr(full.find_first_not_of(' ', sp));
    if      (cmd == "ping")       ss::log("pong (frame %u, hilo %lu)", ss::g_frame.load(), static_cast<unsigned long>(GetCurrentThreadId()));
    else if (cmd == "pad")        ss::input::probe();
    else if (cmd == "p2spawn")    ss::player::p2_spawn(arg);
    else if (cmd == "state")      { ss::player::report_state(); ss::camera::probe(); ss::input::probe(); }
    else if (cmd == "dump")       ss::diag::dump_from_file();
    else if (cmd == "recon")      ss::diag::dump_default();
    else if (cmd == "p2load")     ss::player::p2_load(arg);
    else if (cmd == "p2adopt")    ss::player::p2_adopt(arg);
    else if (cmd == "p2manual")   ss::player::p2_control(true);
    else if (cmd == "p2auto")     ss::player::p2_control(false);
    else if (cmd == "p2warp")     ss::player::p2_warp(arg);
    else if (cmd == "p2pad")      ss::p2input::command(arg);
    else if (cmd == "p2activate") ss::player::p2_activate();
    else if (cmd == "p2finish")   ss::player::p2_finish();
    else if (cmd == "trace" && arg == "on")  ss::trace::set_enabled(true);
    else if (cmd == "trace" && arg == "off") ss::trace::set_enabled(false);
    else if (cmd == "trace" && arg == "sum") ss::trace::summary();
    else ss::log("orden desconocida: '%s'", cmd.c_str());
}

void poll_command_file() {
    const std::string path = ss::data_dir() + "dmc5_ss_cmd.txt";
    std::vector<std::string> cmds;
    {
        std::ifstream in(path);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            const auto a = line.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            line = line.substr(a, line.find_last_not_of(" \t\r\n") - a + 1);
            if (!line.empty() && line[0] != '#') cmds.push_back(line);
        }
    }
    if (!DeleteFileA(path.c_str()))
        ss::log("no pude borrar %s (error %lu); se repetira la orden", path.c_str(), GetLastError());
    for (const auto& c : cmds) run_command(c);
}

// Entrada de motor "UpdateHID" y "UpdateMotionFrame": solo para medir desde
// donde se lee el mando.
void on_pre_hid()   { ss::g_in_hid.store(1, std::memory_order_relaxed); }
void on_after_hid() { ss::g_in_hid.store(0, std::memory_order_relaxed); }

void on_frame() {
    ss::g_main_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const unsigned frame = ss::g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

    if (frame == 1) ss::log("on_frame ACTIVO (UpdateBehavior, hilo %lu)", static_cast<unsigned long>(GetCurrentThreadId()));

    // Inicializacion por etapas: la TDB puede no estar lista en los primeros frames.
    if (!g_player_init_done && frame % 30 == 0) g_player_init_done = ss::player::init();
    if (!g_trace_done && frame % 30 == 0)       g_trace_done = ss::trace::install();
    if (!g_p2input_done && frame % 30 == 0)     g_p2input_done = ss::p2input::install();

    if (key_pressed(VK_F5))  run_command("ping");
    if (key_pressed(VK_F6))  run_command("state");
    if (key_pressed(VK_F7))  run_command("dump");
    if (key_pressed(VK_F8))  run_command("recon");
    // --- ordenes de P2 ---
    if (key_pressed(VK_PRIOR)) select_char(+1);
    if (key_pressed(VK_NEXT))  select_char(-1);
    if (key_pressed(VK_F9))    run_command(std::string("p2adopt ") + kChars[g_p2_char]);
    if (key_pressed(VK_F10))   run_command(std::string("p2load ")  + kChars[g_p2_char]);
    if (key_pressed(VK_F12))   run_command("p2warp");
    if (key_pressed(VK_F5, 1)) run_command("p2manual");
    if (key_pressed(VK_F6, 1)) run_command("p2auto");
    if (key_pressed(VK_F7, 1)) run_command("p2pad on 1");
    if (key_pressed(VK_F8, 1)) run_command("p2pad off");
    if (key_pressed(VK_F9, 1)) run_command("p2pad status");
    if (key_pressed(VK_F10, 1)) run_command("p2activate");
    if (key_pressed(VK_F11, 1)) run_command("p2finish");
    if (key_pressed(VK_F12, 1)) run_command("p2warp request");
    if (key_pressed(VK_F11)) run_command("pad");
    if (frame % 20 == 0) poll_command_file();

    ss::player::on_frame();

    if (frame % 180 == 0) {
        const auto s = ss::input::take_stats();
        if (s.total)
            ss::log("XInput(0) en 3 s: total=%u en_UpdateHID=%u dentro_manager=%u dentro_personaje=%u como_P2=%u otro_hilo=%u",
                    s.total, s.in_hid, s.in_manager, s.in_character, s.as_p2, s.other_thread);
    }
    if (frame % 1800 == 0) ss::log("latido: frame=%u", frame);
}

} // namespace

extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    // Estos valores ya funcionaron con el REFramework instalado ("requires version 1.5.9"
    // aparece en su log y a continuacion "Initializing dmc5_splitscreen...").
    version->major = 1;
    version->minor = 5;
    version->patch = 9;
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    reframework::API::initialize(param);

    // --- Banner: identifica el binario y comprueba la escritura de logs -------------
    ss::g_log_sink.ensure_open();
    ss::log("==== dmc5_splitscreen compilado %s ====", ss::build_tag());
    ss::log("proceso: %s", ss::exe_dir().c_str());
    ss::log("log principal: %s  (abierto=%d, error_apertura=%lu)", ss::g_log_sink.path().c_str(),
            ss::g_log_sink.is_open(), ss::g_log_sink.open_error());
    if (param && param->version)
        ss::log("API de plugins de REFramework (runtime): %d.%d.%d", param->version->major,
                param->version->minor, param->version->patch);
    ss::log("ordenes: archivo dmc5_ss_cmd.txt o teclas F5..F12, Ctrl+F5..F12, RePag/AvPag (ver cabecera de main.cpp)");

    if (!ss::input::install()) ss::log("AVISO: el hook de XInput no se instalo");

    param->functions->on_pre_application_entry("UpdateHID",         reinterpret_cast<REFOnPreApplicationEntryCb>(on_pre_hid));
    param->functions->on_pre_application_entry("UpdateMotionFrame", reinterpret_cast<REFOnPreApplicationEntryCb>(on_after_hid));
    param->functions->on_pre_application_entry("UpdateBehavior",    reinterpret_cast<REFOnPreApplicationEntryCb>(on_frame));
    return true;
}
