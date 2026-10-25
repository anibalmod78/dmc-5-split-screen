#pragma once
#include "common.hpp"

namespace ss::player {

bool init();              // hook de diagnostico en PlayerManager::doUpdate (una vez)
void on_frame();          // cada frame: singletons, deteccion de jugadores, sondeo de carga
void report_state();      // vuelca el estado real de jugadores y cargador adicional
void p2_load();           // pide al juego cargar un jugador adicional (flujo nativo)
void p2_activate();       // llama al activatePlayerResource() del cargador pedido
void p2_finish();         // requestFinishPlayerResource del jugador pedido
bool has_player2();
reframework::API::ManagedObject* player2();

} // namespace ss::player
