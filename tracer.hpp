#pragma once
#include "common.hpp"

// Vigila (sin modificar nada) como el PROPIO juego usa las funciones de carga
// y registro de jugadores. Salida: dmc5_ss_trace.log
namespace ss::trace {

bool install();            // idempotente; reintentar hasta que devuelva true
void set_enabled(bool on);
void summary();            // cuantas veces se disparo cada hook

} // namespace ss::trace
