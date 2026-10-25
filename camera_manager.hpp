#pragma once
#include "common.hpp"

// ESTADO: SOLO LECTURA. La version anterior intentaba fijar el viewport con
// via.Camera::set_ViewportRect, que NO EXISTE (el volcado real de via.Camera
// solo trae FOV, planos, AspectRatio, LookAt, matrices...). La pantalla
// dividida hay que atacarla en la capa de render/SceneView; primero se mide.
namespace ss::camera {

void probe();   // vuelca camara principal, SceneView(s), tamanos y FOV

} // namespace ss::camera
