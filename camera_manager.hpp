#pragma once
#include "common.hpp"

namespace ss::camera {

bool init();                                                      // siempre true; resuelve perezosamente
void update();                                                    // cada frame
void set_player2_target(reframework::API::ManagedObject* go);     // GameObject del P2 (nullptr = quitar)
void request_retry();                                             // reintenta crear la cámara 2 (F10)
void dump_info();                                                 // vuelca métodos candidatos (F8)

} // namespace ss::camera
