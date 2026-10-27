// ============================================================================
// tracer.cpp - observacion pasiva del flujo nativo de jugadores.
//
// Idea: en vez de adivinar con que argumentos hay que llamar a
// AdditionalPlayerLoadManager / CharacterFactory / PlayerManager, dejamos que
// el PROPIO juego las llame (al empezar una mision, cambiar de personaje...) y
// registramos metodo, argumentos (enums con su nombre) y valor devuelto.
// Eso da la secuencia real y los valores reales para reproducirla despues.
//
// Layout de argv en el pre-hook (REFramework):
//   metodo de instancia: argv[0]=contexto, argv[1]=this, argv[2..]=parametros
//   metodo estatico    : argv[0]=contexto, argv[1..]=parametros
// Cada hook necesita su propia funcion (el callback no recibe identidad), por
// eso se generan kMaxSlots pares de thunks por plantilla.
// ============================================================================
#include "tracer.hpp"
#include "reflect_util.hpp"
#include <array>
#include <utility>

namespace ss::trace {
namespace {

constexpr int      kMaxSlots        = 48;
constexpr unsigned kMaxLinesPerSlot = 400;

struct Slot {
    rf::Method*               method = nullptr;
    std::string               label;
    std::vector<rf::TypeDef*> ptypes;
    rf::TypeDef*              rtype = nullptr;
    bool                      is_static = false;
    unsigned                  limit = kMaxLinesPerSlot;
    std::atomic<unsigned>     hits{0};
};

Slot               g_slots[kMaxSlots];
int                g_count = 0;
std::atomic<bool>  g_enabled{true};
bool               g_done = false;

struct Target { const char* type; const char* method; unsigned limit; };
// Solo metodos CONFIRMADOS en los volcados reales del juego.
constexpr Target kTargets[] = {
    // NOTA: se han quitado set_manualPlayer y set_CurrentState. Son setters triviales cuyo
    // codigo nativo comparte el compilador con muchos otros tipos (la traza mostraba
    // this=app.PlayerChain / ActivateMotionSynchronizer): solo generaban ruido.
    {"app.PlayerManager", "addPlayer", 400},
    {"app.PlayerManager", "addPlayerCore", 400},
    {"app.PlayerManager", "removePlayer", 400},
    {"app.PlayerManager", "setCostumeInfo", 400},
    {"app.PlayerManager", "setCostumeID", 60},
    {"app.PlayerManager", "setSituationID", 60},
    {"app.PlayerManager", "playAIMode", 400},

    {"app.AdditionalPlayerLoadManager", "requestPreloadPlayerResource", 400},
    {"app.AdditionalPlayerLoadManager", "requestLoadPlayerResource", 400},
    {"app.AdditionalPlayerLoadManager", "requestUnloadPlayerResource", 400},
    {"app.AdditionalPlayerLoadManager", "requestFinishPlayerResource", 400},
    {"app.AdditionalPlayerLoadManager", "requestFinishAllPlayerResource", 400},

    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "init", 400},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "preloadNakidPlayerResource", 400},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "loadAddtionalPlayerResource", 400},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "activatePlayerResource", 400},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "unloadAdditionalPlayerResource", 400},

    {"app.CharacterFactory", "load", 400},
    {"app.CharacterFactory", "create", 400},      // las dos sobrecargas
    {"app.CharacterFactory", "unload", 400},
    {"app.CharacterFactory", "unloadAll", 400},

    // v6: que ocurre cuando el juego (o p2spawn) termina de crear los objetos
    {"app.CharacterFactory.FinishCreationEvent", "Invoke", 60},
    {"app.PlayerManager", "findPlayer", 3},
    {"app.PlayerManager", "updateMissionInitCheck", 3},
    {"app.PlayerManager", "initCostumeInfo", 20},

    {"app.NetworkCharacterManager", "registerCharacter", 60},
    {"app.NetworkCharacterManager", "releaseCharacter", 60},

    // Entrada: se ejecuta cada frame por jugador; solo interesan las primeras llamadas
    // para ver QUE objeto PadInput se actualiza y desde que hilo.
    {"app.PadInput", "update", 12},
};
constexpr int kNumTargets = static_cast<int>(sizeof(kTargets) / sizeof(kTargets[0]));
bool g_target_done[kNumTargets] = {};

CreateListener g_create_listener = nullptr;
RequestListener g_create_result_listener = nullptr;
RequestListener g_unload_listener = nullptr;
RequestListener g_instance_listener = nullptr;
RequestListener g_pad_listener = nullptr;

// Describe a que objeto/metodo apunta un delegado (una sola vez por delegado).
std::string describe_delegate(rf::Obj* d) {
    static std::vector<void*> seen;
    for (void* p : seen) if (p == d) return "";
    if (seen.size() < 64) seen.push_back(d);
    rf::Obj* target = rf::call_obj(d, "get_Target", {}, false);
    rf::Obj* method = rf::call_obj(d, "get_Method", {}, false);
    std::string mname = "?";
    if (method) mname = rf::mstring(rf::call_obj(method, "get_Name", {}, false));
    char b[64]; std::snprintf(b, sizeof(b), "%p", static_cast<void*>(target));
    return std::string(" {DELEGADO target=") + b + " " + rf::obj_type(target) + " metodo=" + mname + "}";
}

// ---- Formato de valores ------------------------------------------------------
std::string fmt_value(rf::TypeDef* t, void* raw) {
    char b[96];
    const auto bits = reinterpret_cast<uintptr_t>(raw);
    switch (rf::kind_of(t)) {
        case rf::Kind::Enum:
            return rf::enum_str(t, static_cast<int32_t>(bits)) + " <" + rf::tname(t) + ">";
        case rf::Kind::Bool:
            return (bits & 0xFF) ? "true" : "false";
        case rf::Kind::Int:
            std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(static_cast<intptr_t>(bits)));
            return b;
        case rf::Kind::Float: {
            float f; const uint32_t lo = static_cast<uint32_t>(bits);
            std::memcpy(&f, &lo, sizeof(f));
            std::snprintf(b, sizeof(b), "%g", f);
            return b;
        }
        case rf::Kind::Object:
        case rf::Kind::String:
            if (!raw) return "null";
            std::snprintf(b, sizeof(b), "%p ", raw);
            {
                std::string r = std::string(b) + rf::obj_type(static_cast<rf::Obj*>(raw));
                if (t && t->get_vm_obj_type() == REFRAMEWORK_VM_OBJ_TYPE_DELEGATE)
                    r += describe_delegate(static_cast<rf::Obj*>(raw));
                return r;
            }
        case rf::Kind::Struct:
            std::snprintf(b, sizeof(b), "&%p <%s>", raw, rf::tname(t).c_str());
            return b;
        default:
            std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(bits));
            return b;
    }
}

int on_pre(int idx, int argc, void** argv) {
    Slot& s = g_slots[idx];
    const unsigned n = s.hits.fetch_add(1, std::memory_order_relaxed) + 1;
    // El listener se avisa SIEMPRE (aunque se haya agotado el limite de lineas de log).
    if (g_create_listener && argv && !s.is_static && argc >= 7 && s.ptypes.size() == 5 &&
        s.label.rfind("app.CharacterFactory::create/5", 0) == 0) {
        g_create_listener(static_cast<int32_t>(reinterpret_cast<intptr_t>(argv[2])), argv[3], argv[4],
                          static_cast<int32_t>(reinterpret_cast<intptr_t>(argv[5])), argv[6]);
    }
    if (g_pad_listener && argv && !s.is_static && argc >= 2 && s.label.rfind("app.PadInput::update/0", 0) == 0)
        g_pad_listener(argv[1]);
    if (g_unload_listener && argv && !s.is_static && argc >= 3 && s.ptypes.size() == 1 &&
        s.label.rfind("app.CharacterFactory::unload/1", 0) == 0) {
        g_unload_listener(argv[2]);
    }
    if (!g_enabled.load(std::memory_order_relaxed) || n > s.limit || !argv)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;

    std::string line = "PRE  " + s.label + " [t" + std::to_string(GetCurrentThreadId()) +
                       " argc=" + std::to_string(argc) + "] ";
    int first = 1;
    if (!s.is_static) {
        if (argc > 1 && argv[1]) {
            char b[32]; std::snprintf(b, sizeof(b), "%p ", argv[1]);
            line += std::string("this=") + b + rf::obj_type(static_cast<rf::Obj*>(argv[1])) + " ";
        } else line += "this=null ";
        first = 2;
    }
    for (size_t i = 0; i < s.ptypes.size(); ++i) {
        const int ai = first + static_cast<int>(i);
        line += "| p" + std::to_string(i) + "=";
        line += (ai < argc || argc <= 0) ? fmt_value(s.ptypes[i], argv[ai]) : std::string("?");
        line += " ";
    }
    log_trace("%s", line.c_str());
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void on_post(int idx, void** ret_val) {
    Slot& s = g_slots[idx];
    if (g_create_result_listener && ret_val && !s.is_static && s.ptypes.size() == 5 &&
        s.label.rfind("app.CharacterFactory::create/5", 0) == 0) {
        g_create_result_listener(*ret_val);
    }
    if (g_instance_listener && ret_val && s.is_static && s.ptypes.size() == 3 &&
        s.label.rfind("app.CharacterFactory::create/3", 0) == 0) {
        g_instance_listener(*ret_val);
    }
    if (!g_enabled.load(std::memory_order_relaxed) || s.hits.load(std::memory_order_relaxed) > s.limit) return;
    std::string r = "(void)";
    if (s.rtype && rf::tname(s.rtype) != "System.Void") r = ret_val ? fmt_value(s.rtype, *ret_val) : std::string("?");
    log_trace("POST %s -> %s", s.label.c_str(), r.c_str());
}

template <int I> int pre_thunk(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { return on_pre(I, argc, argv); }
template <int I> void post_thunk(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) { on_post(I, ret); }

template <int... Is> constexpr auto make_pre(std::integer_sequence<int, Is...>) {
    return std::array<REFPreHookFn, sizeof...(Is)>{ &pre_thunk<Is>... };
}
template <int... Is> constexpr auto make_post(std::integer_sequence<int, Is...>) {
    return std::array<REFPostHookFn, sizeof...(Is)>{ &post_thunk<Is>... };
}
const auto kPre  = make_pre(std::make_integer_sequence<int, kMaxSlots>{});
const auto kPost = make_post(std::make_integer_sequence<int, kMaxSlots>{});

} // namespace

bool install() {
    if (g_done) return true;
    static unsigned attempts = 0;
    ++attempts;

    bool all_resolved = true;
    for (int i = 0; i < kNumTargets; ++i) {
        if (g_target_done[i]) continue;
        const Target& tg = kTargets[i];
        rf::TypeDef* t = rf::type(tg.type);
        if (!t) {
            all_resolved = false;
            if (attempts == 1 || attempts % 120 == 0) log("trace: tipo aun no disponible: %s", tg.type);
            continue;
        }
        const auto methods = rf::own_methods_named(t, tg.method);
        if (methods.empty()) {
            log("trace: el tipo %s existe pero NO tiene el metodo %s (se omite)", tg.type, tg.method);
            g_target_done[i] = true;
            continue;
        }
        for (auto m : methods) {
            if (g_count >= kMaxSlots) { log("trace: sin slots libres"); break; }
            Slot& s = g_slots[g_count];
            s.method    = m;
            s.is_static = m->is_static();
            s.ptypes    = rf::param_types(m);
            s.rtype     = m->get_return_type();
            s.limit     = tg.limit;
            s.label     = std::string(tg.type) + "::" + tg.method + "/" + std::to_string(m->get_num_params());
            const unsigned id = m->add_hook(kPre[g_count], kPost[g_count], false);
            log("trace: hook #%d en %s (id %u)", g_count, s.label.c_str(), id);
            ++g_count;
        }
        g_target_done[i] = true;
    }

    if (all_resolved || attempts > 600) {
        g_done = true;
        log("trace: instalacion terminada, %d hooks (intentos: %u)", g_count, attempts);
    }
    return g_done;
}

void set_create_listener(CreateListener fn) { g_create_listener = fn; }
void set_create_result_listener(RequestListener fn) { g_create_result_listener = fn; }
void set_unload_listener(RequestListener fn) { g_unload_listener = fn; }
void set_instance_listener(RequestListener fn) { g_instance_listener = fn; }
void set_pad_listener(RequestListener fn) { g_pad_listener = fn; }

void set_enabled(bool on) { g_enabled.store(on); log("trace: %s", on ? "ACTIVADO" : "DESACTIVADO"); }

void summary() {
    log("trace: resumen de %d hooks", g_count);
    for (int i = 0; i < g_count; ++i)
        log("trace:   %-90s hits=%u", g_slots[i].label.c_str(), g_slots[i].hits.load());
}

} // namespace ss::trace
