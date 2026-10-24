#pragma once
#include "common.hpp"
#include <xinput.h>

namespace ss::input {

bool install();     // hook de XInputGetState (MinHook)
void uninstall();

// Qué mando físico (0..3) controla a cada jugador. Por defecto P1=0, P2=1.
void set_pad_for_player(int player, int xinput_index);

// Lectura directa de un mando saltándose el hook (para la cámara del P2).
bool read_pad(int xinput_index, XINPUT_STATE* state);

// Diagnóstico: cuántas veces leyó el juego el mando 0, desde dónde y en qué hilo.
struct Stats { unsigned total, in_hid, in_manager, in_character, as_p2, other_thread; };
Stats take_stats();   // devuelve y reinicia los contadores

} // namespace ss::input
