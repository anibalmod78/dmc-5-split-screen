// Aislamiento de entradas.
//
// Idea: el juego solo lee UN mando (índice 0). Para dar mandos distintos a dos
// personajes hay que saber QUÉ personaje está leyendo en cada momento:
//
//   pre-hook  del update del personaje N  -> g_active_player = N
//   XInputGetState(0, ...)                -> devuelve el mando físico de N
//   post-hook del update del personaje N  -> g_active_player = 0
//
// ATENCIÓN: el log de REFramework muestra una entrada UpdateHID (104) mucho
// antes de UpdateBehavior (126). Es probable que el mando se lea UNA vez ahí y
// los personajes consuman un estado ya cacheado (via.hid.GamePad...). Estos
// contadores lo confirman: si en_UpdateHID ~ total y dentro_personaje ~ 0, este
// truco no basta y hay que intervenir la capa que cachea el estado.
#include "input_isolation.hpp"
#include <MinHook.h>

namespace ss::input {

namespace {
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_original = nullptr;
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
        return g_original(g_pad_for_player[player], state);
    }
    // El juego no debería pedir otros índices; evitamos que se cuele otro mando.
    return ERROR_DEVICE_NOT_CONNECTED;
}
} // namespace

bool install() {
    // Si REFramework ya inicializó MinHook, devuelve ALREADY_INITIALIZED (no es error).
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log("MH_Initialize falló: %d", static_cast<int>(st));
        return false;
    }

    HMODULE mod = GetModuleHandleA("xinput1_4.dll");
    if (!mod) mod = GetModuleHandleA("xinput1_3.dll");
    if (!mod) mod = LoadLibraryA("xinput1_4.dll");
    if (!mod) { log("No se encontró xinput"); return false; }

    auto target = reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState"));
    if (!target) { log("XInputGetState no encontrado"); return false; }

    if (MH_CreateHook(target, &hk_XInputGetState, reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        log("Fallo al hookear XInputGetState");
        return false;
    }
    log("Hook XInputGetState OK");
    return true;
}

void uninstall() { MH_DisableHook(MH_ALL_HOOKS); }

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
