#pragma once
// Diagnóstico por reflexión. Todo va a  <juego>\reframework\data\dmc5_ss.log
//
//   dump_core()       -> volcado automático de los tipos clave (mandos, jugadores,
//                        fábrica de personajes, cámaras)
//   dump_from_file()  -> F7: vuelca lo que pidas en
//                        <juego>\reframework\data\dmc5_ss_dump_request.txt
//
// Formato del archivo de petición (una línea por tipo, # = comentario):
//     app.CharacterFactory
//     via.Camera|viewport,rect,aspect      <- solo métodos que contengan alguna clave
#include "reflect_util.hpp"
#include <fstream>
#include <sstream>

namespace ss::diag {

inline void dump_core() {
    log("[diag] volcado automático de tipos clave -> dmc5_ss.log");
    const char* types[] = {
        // Capa de mandos del motor (el juego los usa tras UpdateHID)
        "via.hid.GamePad", "via.hid.GamePadDevice", "via.hid.MergedGamePadDevice",
        "via.hid.XInputGamePadDevice", "via.hid.PadTracker",
        // Jugadores y su creación por el propio juego
        "app.PlayerManager", "app.CharacterFactory", "app.CharacterFactory.RequestData",
        "app.CharacterFactoryData.PlayerDataSet", "app.NetworkCharacterManager",
        "app.NetworkBattleManager", "app.Player",
        // Cámaras
        "app.CameraManager", "app.PlayerCameraController",
        "via.Camera", "via.SceneView", "via.SceneManager",
        // Objetos genéricos del motor
        "via.GameObject", "via.Prefab",
    };
    for (auto t : types) rf::dump_type(t);
    log("[diag] volcado automático terminado");
}

inline void dump_from_file() {
    const std::string path = data_dir() + "dmc5_ss_dump_request.txt";
    std::ifstream in(path);
    if (!in) { log("[diag] No existe %s. Créalo con un tipo por línea.", path.c_str()); return; }

    auto trim = [](std::string s) {
        const char* ws = " \t\r\n";
        const auto a = s.find_first_not_of(ws);
        if (a == std::string::npos) return std::string();
        return s.substr(a, s.find_last_not_of(ws) - a + 1);
    };

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto bar = line.find('|');
        const std::string name = trim(line.substr(0, bar));
        std::vector<std::string> keys;
        if (bar != std::string::npos) {
            std::stringstream kss(line.substr(bar + 1));
            std::string k;
            while (std::getline(kss, k, ',')) { k = trim(k); if (!k.empty()) keys.push_back(k); }
        }
        rf::dump_type(name.c_str(), keys);
    }
    log("[diag] petición de volcado procesada");
}

} // namespace ss::diag
