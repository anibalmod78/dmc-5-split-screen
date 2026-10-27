#pragma once
#include "common.hpp"
#include <string>

// Entrada del jugador 2 (experimental).
// Idea: el PadInput de cada Player lee los mandos de via.hid.GamePad dentro de app.PadInput::update().
// Si el PadInput de P2 lee el dispositivo del usuario 1 (y no el 0), P2 tendra su propio mando con
// todo el mapeo/zonas muertas del juego, sin tocar banderas de botones una a una.
//
//  - install():  engancha app.PadInput::update y via.hid.GamePad::getDevice/getMergedDevice.
//                Siempre registra (solo log) con que UserIndex lee cada PadInput.
//  - set_target(pad): PadInput del P2.
//  - command("on [user]" | "off" | "status")
//  - on_frame(): si el juego no actualiza el PadInput de P2, se llama a su update() aqui.
namespace ss::p2input {

bool install();
void set_target(void* pad);
void command(const std::string& arg);
void on_frame();

} // namespace ss::p2input
