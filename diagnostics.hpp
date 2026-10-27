#pragma once
// ============================================================================
// diagnostics.hpp - volcado de tipos por reflexion -> dmc5_ss_dump.log
//
//   dump_type(nombre, claves)  metodos (con firma completa), campos, enums
//   dump_from_file()           lee dmc5_ss_dump_request.txt (una linea por tipo;
//                              "Tipo|clave1,clave2" filtra por nombre)
//   dump_default()             lista interna con todo lo necesario para P2
//
// Los enums se vuelcan con sus VALORES (necesarios para elegir PlayerID,
// CostumeID, SituationID...).
// ============================================================================
#include "reflect_util.hpp"
#include <fstream>
#include <sstream>

namespace ss::diag {

inline std::string method_sig(rf::Method* m) {
    std::string s;
    auto rt = m->get_return_type();
    s += rt ? rf::tname(rt) : std::string("void");
    s += " ";
    s += m->get_name();
    s += "(";
    const auto params = m->get_params();
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        auto pt = reinterpret_cast<rf::TypeDef*>(params[i].t);
        s += rf::tname(pt);
        s += " ";
        s += params[i].name ? params[i].name : "?";
    }
    s += ")";
    if (m->is_static()) s += " [static]";
    return s;
}

inline void dump_enum(rf::TypeDef* t) {
    log_dump("[enum] %s (tamano %u)", rf::tname(t).c_str(), t->get_valuetype_size());
    for (const auto& [name, value] : rf::enum_values(t))
        log_dump("[enum]     %s = %lld", name.c_str(), static_cast<long long>(value));
}

inline bool key_match(const char* name, const std::vector<std::string>& keys) {
    if (keys.empty()) return true;
    for (const auto& k : keys) if (rf::icontains(name, k)) return true;
    return false;
}

inline void dump_type(const char* name, const std::vector<std::string>& keys = {}) {
    rf::TypeDef* t = rf::type(name);
    if (!t) { log_dump("[dump] TIPO INEXISTENTE: %s", name); return; }

    if (t->is_enum()) { dump_enum(t); return; }

    log_dump("[dump] ===== %s =====", name);
    unsigned n_methods = 0, n_fields = 0;
    rf::TypeDef* cur = t;
    for (int depth = 0; cur && depth < 8; ++depth, cur = cur->get_parent_type()) {
        const std::string cn = rf::tname(cur);
        if (depth > 0 && (cn.rfind("System.", 0) == 0 || cn.rfind("via.clr.", 0) == 0)) continue;
        log_dump("[dump] -- declarados en %s", cn.c_str());

        for (auto f : cur->get_fields()) {
            if (!key_match(f->get_name(), keys)) continue;
            auto ft = f->get_type();
            log_dump("[dump]     campo %s : %s%s", f->get_name(), rf::tname(ft).c_str(),
                     f->is_static() ? " [static]" : "");
            ++n_fields;
        }
        for (auto m : cur->get_methods()) {
            if (!key_match(m->get_name(), keys)) continue;
            log_dump("[dump]     %s", method_sig(m).c_str());
            ++n_methods;
        }
    }
    log("dump %s -> %u metodos, %u campos", name, n_methods, n_fields);
}

inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

inline void dump_from_file() {
    const std::string path = data_dir() + "dmc5_ss_dump_request.txt";
    std::ifstream in(path);
    if (!in) { log("[diag] No existe %s", path.c_str()); return; }

    unsigned n = 0;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto bar = line.find('|');
        const std::string name = trim(line.substr(0, bar));
        std::vector<std::string> keys;
        if (bar != std::string::npos) {
            std::stringstream ss(line.substr(bar + 1));
            std::string k;
            while (std::getline(ss, k, ',')) { k = trim(k); if (!k.empty()) keys.push_back(k); }
        }
        dump_type(name.c_str(), keys);
        ++n;
    }
    log("[diag] dump_from_file: %u tipos procesados (ver dmc5_ss_dump.log)", n);
}

// Lo imprescindible para atacar P2, P3 (input) y P6 (camara).
inline void dump_default() {
    static const char* full[] = {
        // Enums: sus valores son lo que falta para pedir un jugador adicional
        "app.PlayerID", "app.PlayerCostumeID", "app.PlayerSituationID",
        "app.CharacterFactory.ModeEnum", "app.CharacterFactory.CostumeID",
        "app.AdditionalPlayerLoadManager.AdditionalPlayerLoader.LoadState",
        // Carga de jugadores adicionales
        "app.AdditionalPlayerLoadManager",
        "app.AdditionalPlayerLoadManager.AdditionalPlayerLoader",
        "app.CharacterFactory", "app.CharacterFactory.RequestData",
        // Ids de fabrica (500, 600... del trazado) y sus datos
        "app.CharacterFactory.FactoryID", "app.CharacterFactory.CostumeData", "app.CharacterFactoryData",
        "app.CharacterFactory.FinishCreationEvent",
        // Usuarios/mandos
        "via.UserIndex", "app.Player.PlayerControlType",
        // Camara / render
        "via.render.layer.Scene", "via.SceneView", "via.SceneManager",
    };
    for (auto t : full) dump_type(t);
    dump_type("app.PadInput", {"update", "enable", "device", "pad", "button", "axis", "assign", "clone"});

    // Tipos enormes: solo lo relacionado con entrada / identidad / camara.
    dump_type("app.Player", {"pad", "input", "playerid", "control", "manual", "camera", "costume", "situation"});
    dump_type("app.PlayerManager");
    dump_type("app.CameraManager");
    dump_type("app.PlayerCameraController", {"target", "player", "camera", "update", "pad", "input"});
    dump_type("via.hid.GamePad", {"device", "virtual", "hijack", "user"});
    log("[diag] dump_default terminado");
}

} // namespace ss::diag
