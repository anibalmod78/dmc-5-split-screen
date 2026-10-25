// ============================================================================
// player_manager.cpp - jugadores reales del juego y carga de un jugador
// adicional a traves del flujo NATIVO (app.AdditionalPlayerLoadManager).
//
// Todas las firmas usadas aqui salen de los volcados reales del juego:
//   AdditionalPlayerLoadManager:
//     requestPreloadPlayerResource(PlayerID)
//     requestLoadPlayerResource(PlayerID, PlayerCostumeID, PlayerSituationID, bool reload)
//     isReadyPlayerResource(PlayerID, PlayerCostumeID, PlayerSituationID) -> bool
//     requestFinishPlayerResource(PlayerID), isAvailable(), isBusy() ...
//     campos: RequestedPlayerLoaderList / FinishedPlayerLoaderList (List<Loader>)
//   Loader: activatePlayerResource() -> bool, get_CurrentState() -> LoadState,
//     campos RequestedPlayerID / RequestedCostumeID / RequestedSituationID
//   PlayerManager: campo `players` (List<app.Player>), get_manualPlayer(),
//     getCostumeID(PlayerID), getSituationID(PlayerID)
//   Player: get_playerID() -> PlayerID
//
// QUE NO SE SABE TODAVIA (se descubre con dmc5_ss_trace.log y p2state):
//   - si el cargador se avanza solo hasta crear el personaje o si hay que
//     llamar a activatePlayerResource() desde fuera;
//   - si el personaje creado se registra solo en PlayerManager.addPlayer();
//   - los valores de enum validos para el segundo jugador.
// ============================================================================
#include "player_manager.hpp"
#include "reflect_util.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace ss::player {

namespace {
using rf::Obj;

constexpr const char* kApl      = "app.AdditionalPlayerLoadManager";
constexpr const char* kLoadStT  = "app.AdditionalPlayerLoadManager.AdditionalPlayerLoader.LoadState";

Obj* g_pm  = nullptr;                 // app.PlayerManager
Obj* g_apl = nullptr;                 // app.AdditionalPlayerLoadManager
Obj* g_p1  = nullptr;                 // referencia propia (add_ref)
Obj* g_p2  = nullptr;                 // referencia propia (add_ref)
std::atomic<void*> g_pm_from_hook{nullptr};

std::set<rf::Method*> g_hooked;       // metodos de Player ya enganchados
int  g_last_count   = -1;
bool g_expect_p2    = false;
bool g_loading      = false;
int64_t g_req_pid = -1, g_req_cos = 0, g_req_sit = 0;
int  g_ready_state  = -1;             // -1 desconocido, 0 no, 1 si
int  g_baseline     = 0;
unsigned g_frames   = 0;

// ---- Hooks ------------------------------------------------------------------------
int pre_mgr(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_in_manager.store(1, std::memory_order_relaxed);
    if (argc > 1 && argv[1]) g_pm_from_hook.store(argv[1], std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_mgr(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    g_in_manager.store(0, std::memory_order_relaxed);
}

// Anidamiento seguro: si doUpdate de una subclase llama al de la base, ambos
// hooks se disparan; cada post restaura el valor que habia ANTES de su pre.
thread_local int  t_depth = 0;
thread_local int  t_saved[16];

int pre_char(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    void* self = argc > 1 ? argv[1] : nullptr;
    if (t_depth < 16) t_saved[t_depth] = g_active_player.load(std::memory_order_relaxed);
    ++t_depth;
    g_in_character.store(1, std::memory_order_relaxed);
    g_active_player.store((self && self == static_cast<void*>(g_p2)) ? 1 : 0, std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_char(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (t_depth > 0) --t_depth;
    g_active_player.store(t_depth < 16 ? t_saved[t_depth] : 0, std::memory_order_relaxed);
    if (t_depth == 0) g_in_character.store(0, std::memory_order_relaxed);
}

void release(Obj*& o) { if (o) { o->release(); o = nullptr; } }

// Engancha doUpdate/doLateUpdate en TODA la cadena de tipos del jugador.
void hook_player_type(Obj* player) {
    if (!player) return;
    for (rf::TypeDef* t = player->get_type_definition(); t; t = t->get_parent_type()) {
        const std::string n = rf::tname(t);
        if (n.rfind("System.", 0) == 0 || n.rfind("via.", 0) == 0) break;
        for (const char* name : {"doUpdate", "doLateUpdate"}) {
            for (auto m : rf::own_methods_named(t, name)) {
                if (m->get_num_params() != 0 || g_hooked.count(m)) continue;
                m->add_hook(pre_char, post_char, false);
                g_hooked.insert(m);
                log("hook de personaje en %s::%s", n.c_str(), name);
            }
        }
    }
}

// ---- Configuracion (dmc5_ss_config.txt: clave=valor) ---------------------------------
std::map<std::string, std::string> read_config() {
    std::map<std::string, std::string> cfg;
    std::ifstream in(data_dir() + "dmc5_ss_config.txt");
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const char* ws = " \t\r\n";
            const auto a = s.find_first_not_of(ws);
            if (a == std::string::npos) return std::string{};
            return s.substr(a, s.find_last_not_of(ws) - a + 1);
        };
        cfg[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return cfg;
}

bool is_sentinel_name(const std::string& n) {
    for (const char* bad : {"max", "num", "invalid", "none", "count", "unknown", "dummy", "all", "end"})
        if (rf::icontains(n, bad)) return true;
    return false;
}

// ---- Estado de la carga ---------------------------------------------------------------
std::string load_state_str(Obj* loader) {
    int32_t v = 0;
    if (!rf::call_i32(loader, "get_CurrentState", {}, v, false)) return "?";
    return rf::enum_str(rf::type(kLoadStT), v);
}

void log_loader_list(const char* title, Obj* list) {
    const int n = rf::list_count(list);
    log("  %s: %d", title, n);
    for (int i = 0; i < n; ++i) {
        Obj* ld = rf::list_item(list, i);
        if (!ld) continue;
        int32_t pid = -1, cos = -1, sit = -1; bool fin = false, loc = false, rel = false;
        rf::field_i32(ld, "RequestedPlayerID", pid);
        rf::field_i32(ld, "RequestedCostumeID", cos);
        rf::field_i32(ld, "RequestedSituationID", sit);
        rf::field_bool(ld, "isFinishFlow", fin);
        rf::field_bool(ld, "isLocationWaitFlow", loc);
        rf::field_bool(ld, "isReloadFlow", rel);
        log("    loader[%d] %p estado=%s pid=%s costume=%s situation=%s finish=%d locWait=%d reload=%d",
            i, static_cast<void*>(ld), load_state_str(ld).c_str(),
            rf::enum_str(rf::type("app.PlayerID"), pid).c_str(),
            rf::enum_str(rf::type("app.PlayerCostumeID"), cos).c_str(),
            rf::enum_str(rf::type("app.PlayerSituationID"), sit).c_str(),
            fin, loc, rel);
    }
}

// Devuelve el loader (de Requested o Finished) para un PlayerID.
Obj* find_loader(int64_t pid) {
    for (const char* fname : {"RequestedPlayerLoaderList", "FinishedPlayerLoaderList"}) {
        Obj* list = rf::field_obj(g_apl, fname);
        const int n = rf::list_count(list);
        for (int i = 0; i < n; ++i) {
            Obj* ld = rf::list_item(list, i);
            int32_t id = -1;
            if (ld && rf::field_i32(ld, "RequestedPlayerID", id) && id == pid) return ld;
        }
    }
    return nullptr;
}

// ---- Seleccion de IDs para el jugador adicional --------------------------------------
bool choose_ids(int64_t& pid, int64_t& cos, int64_t& sit) {
    rf::TypeDef* tPid = rf::type("app.PlayerID");
    rf::TypeDef* tCos = rf::type("app.PlayerCostumeID");
    rf::TypeDef* tSit = rf::type("app.PlayerSituationID");
    if (!tPid || !tCos || !tSit) { log("No existen los tipos app.PlayerID/CostumeID/SituationID"); return false; }

    const auto cfg = read_config();
    auto get = [&](const char* k) { auto it = cfg.find(k); return it == cfg.end() ? std::string() : it->second; };

    int32_t p1pid = -1;
    if (g_p1) rf::call_i32(g_p1, "get_playerID", {}, p1pid, false);

    if (!rf::parse_enum_or_int(tPid, get("p2_player_id"), pid)) {
        // Autoseleccion: algo distinto de P1, con preferencia por personajes jugables conocidos.
        pid = -1;
        for (const char* pref : {"dante", "nero", "vergil", "v"}) {
            for (const auto& [name, val] : rf::enum_values(tPid)) {
                if (val == p1pid || is_sentinel_name(name)) continue;
                const bool match = std::string(pref) == "v" ? (name.size() == 1 && (name[0] == 'V' || name[0] == 'v'))
                                                            : rf::icontains(name, pref);
                if (match) { pid = val; break; }
            }
            if (pid >= 0) break;
        }
        if (pid < 0) {
            for (const auto& [name, val] : rf::enum_values(tPid))
                if (val != p1pid && !is_sentinel_name(name)) { pid = val; break; }
        }
        if (pid < 0) { log("No pude elegir un PlayerID distinto del de P1 (%d). Pon p2_player_id en dmc5_ss_config.txt", p1pid); return false; }
    }

    int32_t v = 0;
    if (!rf::parse_enum_or_int(tCos, get("p2_costume_id"), cos)) {
        cos = (g_pm && rf::call_i32(g_pm, "getCostumeID", {rf::iv(pid)}, v, false)) ? v : 0;
    }
    if (!rf::parse_enum_or_int(tSit, get("p2_situation_id"), sit)) {
        sit = (g_pm && rf::call_i32(g_pm, "getSituationID", {rf::iv(pid)}, v, false)) ? v : 0;
    }
    log("IDs para P2: player=%s costume=%s situation=%s (P1 player=%s)",
        rf::enum_str(tPid, pid).c_str(), rf::enum_str(tCos, cos).c_str(),
        rf::enum_str(tSit, sit).c_str(), rf::enum_str(tPid, p1pid).c_str());
    return true;
}

// ---- Deteccion de jugadores ------------------------------------------------------------
void describe_player(int idx, Obj* p) {
    int32_t pid = -1;
    rf::call_i32(p, "get_playerID", {}, pid, false);
    rf::V3 pos;
    const bool has_pos = rf::world_position(p, pos);
    log("  player[%d] %p tipo=%s id=%s pos=(%.2f, %.2f, %.2f)%s%s", idx, static_cast<void*>(p),
        rf::obj_type(p).c_str(), rf::enum_str(rf::type("app.PlayerID"), pid).c_str(),
        pos.x, pos.y, pos.z, has_pos ? "" : " [sin pos]",
        p == g_p1 ? " [P1/manual]" : (p == g_p2 ? " [P2]" : ""));
}

void refresh_players() {
    // P1 = jugador controlado manualmente
    Obj* manual = rf::call_obj(g_pm, "get_manualPlayer", {}, false);
    if (manual != g_p1) {
        release(g_p2);
        g_expect_p2 = false;
        release(g_p1);
        g_p1 = manual;
        if (g_p1) {
            g_p1->add_ref();
            log("P1 (manualPlayer) detectado: %s", rf::obj_type(g_p1).c_str());
            hook_player_type(g_p1);
        } else {
            log("P1 desaparecio (manualPlayer == null)");
        }
    }

    Obj* list = rf::field_obj(g_pm, "players");
    const int n = rf::list_count(list);
    if (n != g_last_count) {
        log("PlayerManager.players: %d -> %d", g_last_count, n);
        g_last_count = n;
        for (int i = 0; i < n; ++i) if (Obj* p = rf::list_item(list, i)) describe_player(i, p);
    }

    if (g_expect_p2 && !g_p2) {
        for (int i = 0; i < n; ++i) {
            Obj* p = rf::list_item(list, i);
            if (p && p != g_p1) {
                g_p2 = p;
                g_p2->add_ref();
                log("P2 DETECTADO en PlayerManager.players[%d]: %s", i, rf::obj_type(g_p2).c_str());
                hook_player_type(g_p2);
                break;
            }
        }
    }
}

void poll_loading() {
    if (!g_loading || !g_apl) return;
    bool ready = false;
    if (rf::call_bool(g_apl, "isReadyPlayerResource", {rf::iv(g_req_pid), rf::iv(g_req_cos), rf::iv(g_req_sit)}, ready, false)) {
        const int now = ready ? 1 : 0;
        if (now != g_ready_state) {
            g_ready_state = now;
            log("isReadyPlayerResource -> %s", ready ? "TRUE" : "false");
            log_loader_list("Requested", rf::field_obj(g_apl, "RequestedPlayerLoaderList"));
            log_loader_list("Finished",  rf::field_obj(g_apl, "FinishedPlayerLoaderList"));
        }
    }
}

} // namespace

// ---- API publica -----------------------------------------------------------------------
bool init() {
    auto t = rf::type("app.PlayerManager");
    auto m = t ? rf::find_method(t, "doUpdate", 0) : nullptr;
    if (!m) { log("init: app.PlayerManager::doUpdate no encontrado"); return false; }
    m->add_hook(pre_mgr, post_mgr, false);
    log("init: hook de diagnostico en app.PlayerManager::doUpdate");
    return true;
}

void on_frame() {
    ++g_frames;

    if (!g_pm) {
        g_pm = rf::singleton("app.PlayerManager");
        if (!g_pm) if (void* h = g_pm_from_hook.load()) g_pm = static_cast<Obj*>(h);
        if (g_pm) log("app.PlayerManager disponible (%p)", static_cast<void*>(g_pm));
    }
    if (!g_apl && g_frames % 60 == 0) {
        g_apl = rf::singleton(kApl);
        if (g_apl) log("%s disponible (%p)", kApl, static_cast<void*>(g_apl));
    }

    if (g_pm && g_frames % 30 == 0) { refresh_players(); poll_loading(); }
}

void report_state() {
    log("=========== ESTADO ===========");
    log("PlayerManager=%p  AdditionalPlayerLoadManager=%p", static_cast<void*>(g_pm), static_cast<void*>(g_apl));
    if (g_pm) {
        int32_t num = -1;
        rf::call_i32(g_pm, "getPlayerNum", {}, num, false);
        log("getPlayerNum=%d", num);
        Obj* list = rf::field_obj(g_pm, "players");
        const int n = rf::list_count(list);
        log("players.Count=%d", n);
        for (int i = 0; i < n; ++i) if (Obj* p = rf::list_item(list, i)) describe_player(i, p);
    }
    if (g_apl) {
        bool a = false, b = false, nl = false, ba = false;
        rf::call_bool(g_apl, "isAvailable", {}, a, false);
        rf::call_bool(g_apl, "isBusy", {}, b, false);
        rf::call_bool(g_apl, "isNotLoadPlayerResource", {}, nl, false);
        rf::call_bool(g_apl, "isBusyAllPlayerResource", {}, ba, false);
        log("APL: isAvailable=%d isBusy=%d isNotLoadPlayerResource=%d isBusyAllPlayerResource=%d", a, b, nl, ba);
        log_loader_list("Requested", rf::field_obj(g_apl, "RequestedPlayerLoaderList"));
        log_loader_list("Finished",  rf::field_obj(g_apl, "FinishedPlayerLoaderList"));
    }
    log("peticion P2: loading=%d pid=%lld cos=%lld sit=%lld ready=%d  P2=%p",
        g_loading, static_cast<long long>(g_req_pid), static_cast<long long>(g_req_cos),
        static_cast<long long>(g_req_sit), g_ready_state, static_cast<void*>(g_p2));
}

void p2_load() {
    if (!g_pm || !g_apl) { log("p2load: faltan singletons (PlayerManager=%p APL=%p). Entra primero en una mision.",
                               static_cast<void*>(g_pm), static_cast<void*>(g_apl)); return; }
    if (!g_p1) { log("p2load: todavia no hay P1 (manualPlayer == null)"); return; }

    bool available = false;
    rf::call_bool(g_apl, "isAvailable", {}, available, false);
    const auto cfg = read_config();
    const bool force = cfg.count("force") && cfg.at("force") == "1";
    if (!available && !force) { log("p2load: APL.isAvailable()==false. Aborto (pon force=1 en la config para ignorarlo)."); return; }

    if (!choose_ids(g_req_pid, g_req_cos, g_req_sit)) return;

    g_baseline = rf::list_count(rf::field_obj(g_pm, "players"));
    g_ready_state = -1;

    rf::Ret r{};
    const bool preload = !(cfg.count("p2_preload") && cfg.at("p2_preload") == "0");
    if (preload) {
        const bool ok = rf::invoke(g_apl, "requestPreloadPlayerResource", {rf::iv(g_req_pid)}, r);
        log("requestPreloadPlayerResource(%lld) -> %s", static_cast<long long>(g_req_pid), ok ? "ok" : "FALLO");
    }
    const bool ok = rf::invoke(g_apl, "requestLoadPlayerResource",
                               {rf::iv(g_req_pid), rf::iv(g_req_cos), rf::iv(g_req_sit), rf::iv(0)}, r);
    log("requestLoadPlayerResource -> %s", ok ? "ok" : "FALLO");
    if (!ok) return;

    g_loading = true;
    g_expect_p2 = true;
    log("Carga pedida. Observa dmc5_ss.log / dmc5_ss_trace.log; luego usa p2state y, si hace falta, p2activate.");
}

void p2_activate() {
    if (!g_apl || g_req_pid < 0) { log("p2activate: primero p2load"); return; }
    Obj* ld = find_loader(g_req_pid);
    if (!ld) { log("p2activate: no hay loader para el PlayerID pedido (ver p2state)"); return; }
    log("p2activate: loader %p en estado %s", static_cast<void*>(ld), load_state_str(ld).c_str());
    rf::Ret r{};
    const bool ok = rf::invoke(ld, "activatePlayerResource", {}, r);
    log("activatePlayerResource -> %s, devolvio %d. Estado ahora: %s",
        ok ? "ok" : "FALLO", r.byte, load_state_str(ld).c_str());
    g_expect_p2 = true;
}

void p2_finish() {
    if (!g_apl || g_req_pid < 0) { log("p2finish: primero p2load"); return; }
    rf::Ret r{};
    const bool ok = rf::invoke(g_apl, "requestFinishPlayerResource", {rf::iv(g_req_pid)}, r);
    log("requestFinishPlayerResource(%lld) -> %s", static_cast<long long>(g_req_pid), ok ? "ok" : "FALLO");
    g_loading = false;
}

bool has_player2() { return g_p2 != nullptr; }
rf::Obj* player2() { return g_p2; }

} // namespace ss::player
