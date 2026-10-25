// Aislamiento de entradas (capa XInput).
//
// Hipotesis A: el juego llama a XInputGetState DENTRO del update de cada
// personaje -> basta con elegir el mando segun g_active_player.
// Hipotesis B: lo lee UNA vez en UpdateHID y reparte un estado cacheado
// (via.hid.GamePad). Los contadores de take_stats() deciden cual es la cierta:
//   en_UpdateHID ~ total  y  dentro_personaje ~ 0   => hipotesis B
//
// Datos del volcado real que sustentan la alternativa B:
//   * via.hid.GamePadDevice tiene set_Button/set_AxisL/set_AxisR (se puede escribir)
//   * via.hid.GamePad.getDevice(UserIndex) devuelve el dispositivo de cada usuario
//   * app.Player tiene get_padInput/set_padInput: un app.PadInput POR jugador
#include "input_isolation.hpp"
#include <MinHook.h>

namespace ss::input {

namespace {
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_original = nullptr;
void* g_target = nullptr;
int g_pad_for_player[2] = {0, 1};

std::atomic<unsigned> n_total{0}, n_hid{0}, n_mgr{0}, n_char{0}, n_p2{0}, n_other{0};

DWORD WINAPI hk_XInputGetState(DWORD user_index, XINPUT_STATE* state) {
    if (user_index == 0) {
        const int player = g_active_player.load(std::memory_order_relaxed) & 1;
        n_total.fetch_add(1, std::memory_order_relaxed);
        if (g_in_hid.load(std::memory_order_relaxed))       n_hid.fetch_add(1, std::memory_order_relaxed);
        if (g_in_manager.load(std::memory_order_relaxed))   n_mgr.fetch_add(1, std::memory_order_relaxed);
        if (g_in_character.load(std::memory_order_relaxed)) n_char.fetch_add(1, std::memory_order_relaxed);
        if (player == 1) n_p2.fetch_add(1, std::memory_order_relaxed);
        const DWORD main_tid = g_main_tid.load(std::memory_order_relaxed);
        if (main_tid != 0 && GetCurrentThreadId() != main_tid) n_other.fetch_add(1, std::memory_order_relaxed);
        return g_original(static_cast<DWORD>(g_pad_for_player[player]), state);
    }
    return ERROR_DEVICE_NOT_CONNECTED;   // el juego no deberia pedir otros indices
}
} // namespace

bool install() {
    // Si REFramework ya inicializo MinHook, devuelve ALREADY_INITIALIZED (no es error).
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log("MH_Initialize fallo: %d", static_cast<int>(st));
        return false;
    }

    const char* used = "xinput1_4.dll";
    HMODULE mod = GetModuleHandleA(used);
    if (!mod) { used = "xinput1_3.dll"; mod = GetModuleHandleA(used); }
    if (!mod) { used = "xinput1_4.dll"; mod = LoadLibraryA(used); }
    if (!mod) { log("No se encontro ninguna DLL de xinput (error %lu)", GetLastError()); return false; }

    g_target = reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState"));
    if (!g_target) { log("XInputGetState no encontrado en %s", used); return false; }

    const MH_STATUS c = MH_CreateHook(g_target, reinterpret_cast<void*>(&hk_XInputGetState),
                                      reinterpret_cast<void**>(&g_original));
    if (c != MH_OK) { log("MH_CreateHook fallo: %d", static_cast<int>(c)); return false; }
    const MH_STATUS e = MH_EnableHook(g_target);
    if (e != MH_OK) { log("MH_EnableHook fallo: %d", static_cast<int>(e)); return false; }

    log("Hook XInputGetState OK en %s", used);
    return true;
}

void uninstall() { if (g_target) MH_DisableHook(g_target); }

void set_pad_for_player(int player, int xinput_index) {
    if (player >= 0 && player < 2) g_pad_for_player[player] = xinput_index;
}

bool read_pad(int xinput_index, XINPUT_STATE* state) {
    return g_original && g_original(static_cast<DWORD>(xinput_index), state) == ERROR_SUCCESS;
}

Stats take_stats() {
    return { n_total.exchange(0), n_hid.exchange(0), n_mgr.exchange(0), n_char.exchange(0),
             n_p2.exchange(0), n_other.exchange(0) };
}

} // namespace ss::input
