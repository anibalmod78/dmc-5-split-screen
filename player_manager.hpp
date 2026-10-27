#pragma once
#include "common.hpp"
#include <string>

namespace ss::player {

bool init();              // hook de diagnostico en PlayerManager::doUpdate (una vez)
void on_frame();          // cada frame: singletons, deteccion de jugadores, sondeo de carga
void report_state();
void report_pads();       // v7: todos los app.PadInput vistos en update() y su estado      // vuelca el estado real de jugadores y cargador adicional
void p2_spawn(const std::string& arg);   // repite CharacterFactory.create con padre+delegado reales del juego
void p2_load(const std::string& arg);   // pide al juego cargar un jugador adicional (flujo nativo); arg = PlayerID opcional
void p2_adopt(const std::string& arg);  // v8: toma como P2 a un Player YA existente (por PlayerID) para probar control/entrada
void p2_control(bool manual);           // v8: set_playerControlType(Manual/Auto) sobre P2
void p2_warp(const std::string& arg);   // v8: coloca a P2 junto a P1 (arg "request" usa requestWarp)
void p2_activate();       // llama al activatePlayerResource() del cargador pedido
void p2_finish();         // requestFinishPlayerResource del jugador pedido
bool has_player2();
reframework::API::ManagedObject* player2();

} // namespace ss::player
