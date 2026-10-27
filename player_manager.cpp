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
#include "p2_input.hpp"
#include "reflect_util.hpp"
#include "tracer.hpp"
#include <algorithm>
#include <mutex>
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
std::atomic<unsigned> g_pm_hook_frame{0};   // ultimo frame en que doUpdate del manager se disparo

std::set<rf::Method*> g_hooked;       // metodos de Player ya enganchados
int  g_last_count   = -1;
bool g_expect_p2    = false;
bool g_loading      = false;
int64_t g_req_pid = -1, g_req_cos = 0, g_req_sit = 0;
int  g_ready_state  = -1;             // -1 desconocido, 0 no, 1 si
int  g_baseline     = 0;
unsigned g_frames   = 0;

// ---- Grabacion de CharacterFactory.create(id, costume, parent, mode, finishEvent) -----
// El trazado mostro que el juego crea a sus jugadores asi (mode = InGame(3)):
//   create(500, null, <GameObject padre>, InGame, <FinishCreationEvent>)
// y que el delegado FinishCreationEvent(id, body, face) es quien termina el trabajo
// (registro en PlayerManager). Guardamos padre y delegado REALES del juego para
// poder repetir la llamada con otro id (p2spawn).
struct FactoryRecord { int32_t id; void* parent; int32_t mode; void* finish; unsigned frame; unsigned serial; void* req; };
std::mutex g_rec_mtx;
std::vector<FactoryRecord> g_records;
std::set<void*> g_known_players;      // fotografia de players al pedir el spawn
Obj* g_spawn_req = nullptr;
std::string g_pid_override;           // PlayerID pedido en la orden p2load <id>           // RequestData devuelto por create()
int  g_spawn_finished = -1;

thread_local bool     t_spawning = false;   // true mientras p2spawn llama a create (no se graba a si mismo)
thread_local unsigned t_pending   = 0;       // serial del registro cuyo POST aun no llego (mismo hilo)
unsigned g_serial = 0;

void drop_record_refs(const FactoryRecord& r) {
    static_cast<Obj*>(r.parent)->release();
    static_cast<Obj*>(r.finish)->release();
    if (r.req) static_cast<Obj*>(r.req)->release();
}

void on_factory_create(int32_t id, void*, void* parent, int32_t mode, void* finish) {
    if (t_spawning) return;
    if (mode != 3 && mode != 4) return;          // solo InGame / InGameHigh
    if (!parent || !finish) return;
    std::lock_guard<std::mutex> lk(g_rec_mtx);
    static_cast<Obj*>(parent)->add_ref();
    static_cast<Obj*>(finish)->add_ref();
    g_records.push_back({id, parent, mode, finish, g_frame.load(), ++g_serial, nullptr});
    t_pending = g_serial;
    log("GRABADO CharacterFactory.create(id=%d, mode=%d) padre=%p delegado=%p (%zu registros)",
        id, mode, parent, finish, g_records.size());
}

// POST de create/5: asocia el RequestData devuelto al registro (para olvidarlo cuando el juego lo descargue).
void on_create_result(void* req) {
    if (!t_pending) return;
    const unsigned ser = t_pending; t_pending = 0;
    if (!req) return;
    std::lock_guard<std::mutex> lk(g_rec_mtx);
    for (auto& r : g_records) if (r.serial == ser) { static_cast<Obj*>(req)->add_ref(); r.req = req; return; }
}

// PRE de CharacterFactory.unload(request): el grabado asociado deja de ser valido.
void on_unload(void* req) {
    if (!req) return;
    std::lock_guard<std::mutex> lk(g_rec_mtx);
    for (size_t i = 0; i < g_records.size();) {
        if (g_records[i].req == req) {
            log("grabacion id=%d descartada: el juego descargo su RequestData %p", g_records[i].id, req);
            drop_record_refs(g_records[i]);
            g_records.erase(g_records.begin() + static_cast<long>(i));
        } else ++i;
    }
}

// ---- Hooks ------------------------------------------------------------------------
int pre_mgr(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_in_manager.store(1, std::memory_order_relaxed);
    if (argc > 1 && argv[1]) {
        g_pm_from_hook.store(argv[1], std::memory_order_relaxed);
        g_pm_hook_frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
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

    if (!rf::parse_enum_or_int(tPid, !g_pid_override.empty() ? g_pid_override : get("p2_player_id"), pid)) {
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
// ---- Sonda de GameObjects (solo lectura) ----------------------------------------------
std::string go_name(Obj* go) { return go ? rf::mstring(rf::call_obj(go, "get_Name", {}, false)) : std::string("null"); }

std::string go_parent_name(Obj* go) {
    Obj* tr  = go ? rf::call_obj(go, "get_Transform", {}, false) : nullptr;
    Obj* par = tr ? rf::call_obj(tr, "get_Parent", {}, false) : nullptr;
    Obj* pgo = par ? rf::call_obj(par, "get_GameObject", {}, false) : nullptr;
    return go_name(pgo);
}

std::string go_children(Obj* go) {
    std::string out;
    Obj* tr = go ? rf::call_obj(go, "get_Transform", {}, false) : nullptr;
    Obj* ch = tr ? rf::call_obj(tr, "get_Child", {}, false) : nullptr;
    for (int i = 0; ch && i < 8; ++i) {
        Obj* cgo = rf::call_obj(ch, "get_GameObject", {}, false);
        out += "'" + go_name(cgo) + "' ";
        ch = rf::call_obj(ch, "get_Next", {}, false);
    }
    return out.empty() ? std::string("(sin hijos o no legibles)") : out;
}

std::string go_components(Obj* go) {
    std::string out;
    if (!go) return out;
    for (const char* tn : {"app.Player", "app.character.Character", "app.PlayerV", "app.PlayerVergilPL",
                           "app.PlayerNero", "app.PlayerDante", "app.PadInput"}) {
        auto* t = rf::type(tn);
        Obj* rt = t ? t->get_runtime_type() : nullptr;
        if (!rt) continue;
        if (Obj* c = rf::call_obj(go, "getComponent", {rt}, false)) {
            char b[48]; std::snprintf(b, sizeof(b), "=%p ", static_cast<void*>(c));
            out += std::string(tn) + b + "(tipo real " + rf::obj_type(c) + ") ";
        }
    }
    return out.empty() ? std::string("(ninguno de los tipos probados)") : out;
}

void log_go(const char* tag, Obj* go) {
    log("  %s GO=%p nombre='%s' padre='%s' componentes: %s", tag, static_cast<void*>(go),
        go_name(go).c_str(), go_parent_name(go).c_str(), go_components(go).c_str());
    log("      hijos: %s", go_children(go).c_str());
}

// GameObjects que devuelve CharacterFactory.create(prefab, parent, isInGame) (los del juego y los de p2spawn).
struct InstRec { Obj* go; unsigned frame; };
std::mutex g_pad_mtx;
std::vector<Obj*> g_pads;           // app.PadInput distintos vistos en update()

void on_pad(void* pad) {
    if (!pad) return;
    std::lock_guard<std::mutex> lk(g_pad_mtx);
    for (Obj* p : g_pads) if (p == pad) return;
    if (g_pads.size() < 16) { static_cast<Obj*>(pad)->add_ref(); g_pads.push_back(static_cast<Obj*>(pad)); }
}
std::mutex g_inst_mtx;
std::vector<InstRec> g_inst;
unsigned g_probe_at = 0;

void on_instance(void* go) {
    if (!go) return;
    std::lock_guard<std::mutex> lk(g_inst_mtx);
    static_cast<Obj*>(go)->add_ref();
    g_inst.push_back({static_cast<Obj*>(go), g_frame.load()});
    if (g_inst.size() > 24) { g_inst.front().go->release(); g_inst.erase(g_inst.begin()); }
}

void clear_instances() {
    std::lock_guard<std::mutex> lk(g_inst_mtx);
    for (auto& r : g_inst) r.go->release();
    g_inst.clear();
}

void dump_instances(const char* why) {
    std::vector<InstRec> copy;
    { std::lock_guard<std::mutex> lk(g_inst_mtx); copy = g_inst; }
    log("--- GameObjects creados por CharacterFactory.create(prefab,...) [%s]: %zu ---", why, copy.size());
    for (const auto& r : copy) {
        char tag[48]; std::snprintf(tag, sizeof(tag), "(creado en f%u)", r.frame);
        log_go(tag, r.go);
    }
}

void describe_player(int idx, Obj* p) {
    int32_t pid = -1;
    rf::call_i32(p, "get_playerID", {}, pid, false);
    rf::V3 pos;
    const bool has_pos = rf::world_position(p, pos);
    Obj* pad = rf::call_obj(p, "get_padInput", {}, false);
    Obj* pgo = rf::call_obj(p, "get_GameObject", {}, false);
    log("  player[%d] GO='%s' padre='%s'", idx, go_name(pgo).c_str(), go_parent_name(pgo).c_str());
    log("  player[%d] componentes de su GO: %s", idx, go_components(pgo).c_str());
    log("  player[%d] hijos de su GO: %s", idx, go_children(pgo).c_str());
    {
        int32_t ct = -1; bool man = false, master = false;
        rf::call_i32(p, "get_playerControlType", {}, ct, false);
        rf::call_bool(p, "get_isManuallyControlled", {}, man, false);
        rf::call_bool(p, "get_isMasterManualPlayer", {}, master, false);
        Obj* ai = rf::call_obj(p, "get_aiModeController", {}, false);
        log("  player[%d] controlType=%d(0=Manual,1=Auto) isManuallyControlled=%d isMasterManualPlayer=%d aiModeController=%p",
            idx, ct, man, master, static_cast<void*>(ai));
    }
    log("  player[%d] %p tipo=%s id=%s pos=(%.2f, %.2f, %.2f) padInput=%p%s%s", idx, static_cast<void*>(p),
        rf::obj_type(p).c_str(), rf::enum_str(rf::type("app.PlayerID"), pid).c_str(),
        pos.x, pos.y, pos.z, static_cast<void*>(pad), has_pos ? "" : " [sin pos]",
        p == g_p1 ? " [P1/manual]" : (p == g_p2 ? " [P2]" : ""));
}

void refresh_players() {
    // P1 = jugador controlado manualmente
    Obj* manual = rf::call_obj(g_pm, "get_manualPlayer", {}, false);
    if (manual != g_p1) {
        if (!manual) {
            // Fin de mision: todo lo grabado deja de ser valido.
            release(g_p2);
            g_expect_p2 = false;
            g_loading = false;
            release(g_spawn_req);
            g_known_players.clear();
            release(g_p1);
            log("P1 desaparecio (manualPlayer == null): se descartan P1/P2 (las grabaciones de create se conservan hasta que el juego las descargue)");
        } else {
            if (g_expect_p2) log("ATENCION: manualPlayer cambio DURANTE el spawn de P2 (posible reemplazo de P1)");
            else release(g_p2);
            release(g_p1);
            g_p1 = manual;
            g_p1->add_ref();
            log("P1 (manualPlayer) detectado: %s", rf::obj_type(g_p1).c_str());
            hook_player_type(g_p1);
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
            if (p && p != g_p1 && !g_known_players.count(p)) {
                g_p2 = p;
                g_p2->add_ref();
                if (Obj* pad = rf::call_obj(g_p2, "get_padInput", {}, false)) p2input::set_target(pad);
                log("P2 DETECTADO en PlayerManager.players[%d]: %s", i, rf::obj_type(g_p2).c_str());
                hook_player_type(g_p2);
                describe_player(i, g_p2);
                break;
            }
        }
    }

    // Seguimiento del RequestData del spawn
    if (g_spawn_req) {
        Obj* factory = rf::singleton("app.CharacterFactory");
        bool fin = false;
        if (factory && rf::call_bool(factory, "isFinished", {g_spawn_req}, fin, false)) {
            if ((fin ? 1 : 0) != g_spawn_finished) {
                g_spawn_finished = fin ? 1 : 0;
                log("CharacterFactory.isFinished(request) -> %s", fin ? "TRUE" : "false");
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

// El juego crea un PlayerManager NUEVO en cada mision (visto en el trazado: 0x...F6CF0 y luego
// 0x...83220). Nunca se cachea el puntero: se resuelve de nuevo y, si cambio, se reinicia el estado.
Obj* resolve_pm() {
    Obj* cur = rf::singleton("app.PlayerManager");
    if (!cur) {
        const unsigned f = g_pm_hook_frame.load(), now = g_frame.load();
        if (f && now - f < 90) cur = static_cast<Obj*>(g_pm_from_hook.load());
    }
    return cur;
}

void sync_manager() {
    Obj* cur = resolve_pm();
    if (cur == g_pm) return;
    log("PlayerManager cambio: %p -> %p (nueva mision o fin de mision)", static_cast<void*>(g_pm), static_cast<void*>(cur));
    release(g_p1); release(g_p2); release(g_spawn_req);
    clear_instances();
    g_known_players.clear();
    g_expect_p2 = false; g_loading = false;
    g_last_count = -1;
    g_pm = cur;
}

} // namespace

// ---- API publica -----------------------------------------------------------------------
bool init() {
    auto t = rf::type("app.PlayerManager");
    auto m = t ? rf::find_method(t, "doUpdate", 0) : nullptr;
    if (!m) { log("init: app.PlayerManager::doUpdate no encontrado"); return false; }
    m->add_hook(pre_mgr, post_mgr, false);
    trace::set_create_listener(on_factory_create);
    trace::set_create_result_listener(on_create_result);
    trace::set_unload_listener(on_unload);
    trace::set_instance_listener(on_instance);
    trace::set_pad_listener(on_pad);
    log("init: hook de diagnostico en app.PlayerManager::doUpdate; grabacion de CharacterFactory.create activa");
    return true;
}

void on_frame() {
    ++g_frames;

    if (g_frames % 30 == 0) sync_manager();
    p2input::on_frame();
    if (g_probe_at && g_frames >= g_probe_at) { g_probe_at = 0; dump_instances("90 frames despues de p2spawn"); }
    if (!g_apl && g_frames % 60 == 0) {
        g_apl = rf::singleton(kApl);
        if (g_apl) log("%s disponible (%p)", kApl, static_cast<void*>(g_apl));
    }

    if (g_pm && g_frames % 30 == 0) { refresh_players(); poll_loading(); }
}

void report_state() {
    sync_manager();
    if (g_pm) refresh_players();
    log("=========== ESTADO ===========");
    { std::lock_guard<std::mutex> lk(g_rec_mtx);
      log("grabaciones de create vigentes: %zu", g_records.size());
      for (const auto& r : g_records) {
          log("  id=%d mode=%d padre=%p delegado=%p request=%p (frame %u)", r.id, r.mode, r.parent, r.finish, r.req, r.frame);
          log_go("  (padre grabado)", static_cast<Obj*>(r.parent));
      } }
    dump_instances("state");
    report_pads();
    log("P1=%p P2=%p  doUpdate del manager disparado por ultima vez en frame %u", static_cast<void*>(g_p1), static_cast<void*>(g_p2), g_pm_hook_frame.load());
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

} // namespace ss::player

namespace ss::player {
void report_pads() {
    std::vector<Obj*> copy;
    { std::lock_guard<std::mutex> lk(g_pad_mtx); copy = g_pads; }
    log("--- app.PadInput distintos vistos en update(): %zu ---", copy.size());
    for (Obj* o : copy) {
        auto u32 = [&](const char* f) { auto* p = o->get_field<uint32_t>(f); return p ? *p : 0xFFFFFFFFu; };
        auto b   = [&](const char* f) { auto* p = o->get_field<uint8_t>(f);  return p ? static_cast<int>(*p) : -1; };
        auto* axis = o->get_field<float>("<axisRaw_L>k__BackingField");
        log("  PadInput %p isEnable=%d isAutoPad=%d updateCounter=%u on=0x%08x trigger=0x%08x release=0x%08x axisRawL=(%.2f, %.2f)",
            static_cast<void*>(o), b("<isEnable>k__BackingField"), b("<isAutoPad>k__BackingField"),
            u32("<updateCounter>k__BackingField"), u32("<buttonOnFlags>k__BackingField"),
            u32("<buttonTriggerFlags>k__BackingField"), u32("<buttonReleaseFlags>k__BackingField"),
            axis ? axis[0] : 0.f, axis ? axis[1] : 0.f);
    }
    if (g_pm) {
        Obj* plist = rf::field_obj(g_pm, "players");
        for (int i = 0, n = rf::list_count(plist); i < n; ++i) {
            Obj* p = rf::list_item(plist, i);
            Obj* o = p ? rf::call_obj(p, "get_padInput", {}, false) : nullptr;
            if (!o) continue;
            auto* uc = o->get_field<uint32_t>("<updateCounter>k__BackingField");
            auto* en = o->get_field<uint8_t>("<isEnable>k__BackingField");
            auto* on = o->get_field<uint32_t>("<buttonOnFlags>k__BackingField");
            log("  player[%d] padInput=%p isEnable=%d updateCounter=%u on=0x%08x", i, static_cast<void*>(o),
                en ? static_cast<int>(*en) : -1, uc ? *uc : 0xFFFFFFFFu, on ? *on : 0xFFFFFFFFu);
        }
    }
    if (g_p1) {
        for (const char* f : {"<padInput>k__BackingField", "<enemyPadInput>k__BackingField", "<tracePadInput>k__BackingField", "CachedPadInput"}) {
            auto* pp = g_p1->get_field<void*>(f);
            log("  P1.%s = %p", f, pp ? *pp : nullptr);
        }
    }
}
} // namespace ss::player

namespace ss::player {
void p2_load(const std::string& arg) {
    g_pid_override = arg;
    sync_manager();
    if (g_pm) refresh_players();
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
    g_known_players.clear();                       // para reconocer al jugador NUEVO (no a los que ya existian)
    for (int i = 0; i < g_baseline; ++i) if (Obj* p = rf::list_item(rf::field_obj(g_pm, "players"), i)) g_known_players.insert(p);

    rf::Ret r{};
    const bool preload = !(cfg.count("p2_preload") && cfg.at("p2_preload") == "0");
    if (preload) {
        const bool ok = rf::invoke(g_apl, "requestPreloadPlayerResource", {rf::iv(g_req_pid)}, r);
        log("requestPreloadPlayerResource(%lld) -> %s", static_cast<long long>(g_req_pid), ok ? "ok" : "FALLO");
    }
    const bool ok = rf::invoke(g_apl, "requestLoadPlayerResource",
                               {rf::iv(g_req_pid), rf::iv(g_req_cos), rf::iv(g_req_sit), rf::iv(1)}, r);   // reload=true, como hace el juego (traza)
    log("requestLoadPlayerResource -> %s", ok ? "ok" : "FALLO");
    if (!ok) return;

    g_loading = true;
    g_expect_p2 = true;
    log("Carga pedida. Observa dmc5_ss.log / dmc5_ss_trace.log; luego usa p2state y, si hace falta, p2activate.");
}

void p2_adopt(const std::string& arg) {
    sync_manager();
    if (g_pm) refresh_players();
    if (!g_pm || !g_p1) { log("p2adopt: no hay P1"); return; }
    rf::TypeDef* tPid = rf::type("app.PlayerID");
    int64_t want = -1;
    const bool has = !arg.empty() && tPid && rf::parse_enum_or_int(tPid, arg, want);
    Obj* list = rf::field_obj(g_pm, "players");
    Obj* pick = nullptr;
    for (int i = 0, n = rf::list_count(list); i < n && !pick; ++i) {
        Obj* p = rf::list_item(list, i);
        if (!p || p == g_p1) continue;
        int32_t pid = -1; rf::call_i32(p, "get_playerID", {}, pid, false);
        if (!has || pid == want) pick = p;
    }
    if (!pick) { log("p2adopt: no hay en PlayerManager.players ningun jugador (distinto de P1)%s", has ? " con ese PlayerID" : ""); return; }
    release(g_p2);
    g_p2 = pick; g_p2->add_ref();
    g_expect_p2 = false;
    hook_player_type(g_p2);
    Obj* pad = rf::call_obj(g_p2, "get_padInput", {}, false);
    if (pad) p2input::set_target(pad);
    log("p2adopt: P2 ADOPTADO = %p (%s), padInput=%p", static_cast<void*>(g_p2), rf::obj_type(g_p2).c_str(), static_cast<void*>(pad));
    describe_player(-1, g_p2);
}

void p2_control(bool manual) {
    if (!g_p2) { log("p2%s: no hay P2 (usa p2adopt o p2load)", manual ? "manual" : "auto"); return; }
    rf::Ret r{};
    const bool ok = rf::invoke(g_p2, "set_playerControlType", {rf::iv(manual ? 0 : 1)}, r);
    log("set_playerControlType(%s) -> %s", manual ? "Manual" : "Auto", ok ? "ok" : "FALLO");
    describe_player(-1, g_p2);
}

void p2_warp(const std::string& arg) {
    if (!g_p1 || !g_p2) { log("p2warp: faltan P1 o P2"); return; }
    rf::V3 pos;
    if (!rf::world_position(g_p1, pos)) { log("p2warp: no pude leer la posicion de P1"); return; }
    float rot[4] = {0, 0, 0, 1};
    Obj* go = rf::call_obj(g_p1, "get_GameObject", {}, false);
    Obj* tr = go ? rf::call_obj(go, "get_Transform", {}, false) : nullptr;
    rf::Ret rr{};
    if (tr && rf::invoke(tr, "get_Rotation", {}, rr, false)) std::memcpy(rot, rr.bytes.data(), sizeof(rot));
    float tgt[4] = {pos.x + 1.5f, pos.y, pos.z, 0.f};
    rf::Ret r{};
    bool ok;
    if (arg == "request") ok = rf::invoke(g_p2, "requestWarp", {tgt, rot}, r);
    else                  ok = rf::invoke(g_p2, "warp", {tgt, rot, rf::iv(0), rf::iv(0)}, r);
    log("p2warp(%s): destino=(%.2f, %.2f, %.2f) rot=(%.2f, %.2f, %.2f, %.2f) -> %s",
        arg == "request" ? "requestWarp" : "warp", tgt[0], tgt[1], tgt[2], rot[0], rot[1], rot[2], rot[3], ok ? "ok" : "FALLO");
}

void p2_spawn(const std::string& arg) {
    sync_manager();
    if (g_pm) refresh_players();
    if (!g_pm || !g_p1) { log("p2spawn: no hay P1 (entra en una mision y espera a controlar al personaje)"); return; }

    std::vector<FactoryRecord> recs;
    { std::lock_guard<std::mutex> lk(g_rec_mtx); recs = g_records; }
    if (recs.empty()) {
        log("p2spawn: NO hay ningun CharacterFactory.create InGame grabado. El plugin debe estar cargado ANTES "
            "de que empiece la mision: reinicia la mision (o reiniciala desde el menu) con el plugin ya activo.");
        return;
    }
    for (const auto& r : recs) log("  grabado: id=%d mode=%d padre=%p delegado=%p", r.id, r.mode, r.parent, r.finish);

    // id a crear: argumento > config > primer grabado
    int64_t id = recs.front().id;
    const auto cfg = read_config();
    if (!arg.empty()) id = std::strtoll(arg.c_str(), nullptr, 0);
    else if (cfg.count("p2_factory_id")) id = std::strtoll(cfg.at("p2_factory_id").c_str(), nullptr, 0);

    // padre y delegado: los del grabado con el mismo id; si no, los del primero
    const FactoryRecord* use = &recs.front();
    for (const auto& r : recs) if (r.id == id) { use = &r; break; }

    Obj* factory = rf::singleton("app.CharacterFactory");
    if (!factory) { log("p2spawn: app.CharacterFactory no disponible"); return; }

    // fotografia de los jugadores actuales para reconocer al nuevo
    g_known_players.clear();
    Obj* list = rf::field_obj(g_pm, "players");
    for (int i = 0, n = rf::list_count(list); i < n; ++i) if (Obj* p = rf::list_item(list, i)) g_known_players.insert(p);

    log("p2spawn: CharacterFactory.create(id=%lld, costume=null, parent=%p, mode=%d, finish=%p)",
        static_cast<long long>(id), use->parent, use->mode, use->finish);
    rf::Ret r{};
    t_spawning = true;
    const bool ok = rf::invoke(factory, "create",
        {rf::iv(id), nullptr, use->parent, rf::iv(use->mode), use->finish}, r);
    t_spawning = false;
    log("p2spawn: create -> %s, RequestData=%p", ok ? "ok" : "FALLO", r.ptr);
    if (!ok) return;

    release(g_spawn_req);
    g_spawn_req = static_cast<Obj*>(r.ptr);
    if (g_spawn_req) g_spawn_req->add_ref();
    g_spawn_finished = -1;
    g_expect_p2 = true;
    g_probe_at = g_frames + 90;
    log("p2spawn: esperando a que el juego registre al nuevo jugador (ver 'players:' y 'P2 DETECTADO').");
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
