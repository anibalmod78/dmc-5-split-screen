#pragma once
#include "common.hpp"
#include <xinput.h>

namespace ss::input {

bool install();     // hook de XInputGetState (MinHook)
void uninstall();

// Que mando fisico (0..3) controla a cada jugador. Por defecto P1=0, P2=1.
void set_pad_for_player(int player, int xinput_index);

// Lectura directa de un mando saltandose el hook.
bool read_pad(int xinput_index, XINPUT_STATE* state);

// Diagnostico: cuantas veces leyo el juego el mando 0, desde donde y en que hilo.
struct Stats { unsigned total, in_hid, in_manager, in_character, as_p2, other_thread; };
Stats take_stats();   // devuelve y reinicia los contadores

// Sonda: que ve el juego (via.hid.GamePad por usuario) y que ve XInput directamente.
void probe();

} // namespace ss::input
