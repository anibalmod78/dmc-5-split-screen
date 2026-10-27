#pragma once
#include "common.hpp"

// Vigila (sin modificar nada) como el PROPIO juego usa las funciones de carga
// y registro de jugadores. Salida: dmc5_ss_trace.log
namespace ss::trace {

bool install();            // idempotente; reintentar hasta que devuelva true
void set_enabled(bool on);
void summary();            // cuantas veces se disparo cada hook

// Aviso en el PRE de cada CharacterFactory.create(id, costume, parent, mode, finishEvent).
// Permite grabar la llamada real del juego para reproducirla despues.
using CreateListener = void (*)(int32_t id, void* costume, void* parent, int32_t mode, void* finish_event);
void set_create_listener(CreateListener fn);

// Aviso en el POST de CharacterFactory.create/5 con el RequestData devuelto (mismo hilo que el PRE).
using RequestListener = void (*)(void* request_data);
void set_create_result_listener(RequestListener fn);
// Aviso en el PRE de CharacterFactory.unload(request): ese RequestData deja de ser valido.
void set_unload_listener(RequestListener fn);
// Aviso en el POST de CharacterFactory.create(prefab, parent, isInGame) con el GameObject instanciado.
void set_instance_listener(RequestListener fn);
// Aviso (siempre) en cada app.PadInput::update con el objeto PadInput (this).
void set_pad_listener(RequestListener fn);

} // namespace ss::trace
