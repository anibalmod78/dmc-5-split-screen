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
    std::atomic<unsigned>     hits{0};
};

Slot               g_slots[kMaxSlots];
int                g_count = 0;
std::atomic<bool>  g_enabled{true};
bool               g_done = false;

struct Target { const char* type; const char* method; };
// Solo metodos CONFIRMADOS en los volcados reales del juego.
constexpr Target kTargets[] = {
    {"app.PlayerManager", "addPlayer"},
    {"app.PlayerManager", "addPlayerCore"},
    {"app.PlayerManager", "removePlayer"},
    {"app.PlayerManager", "setCostumeInfo"},
    {"app.PlayerManager", "setCostumeID"},
    {"app.PlayerManager", "setSituationID"},
    {"app.PlayerManager", "playAIMode"},
    {"app.PlayerManager", "set_manualPlayer"},

    {"app.AdditionalPlayerLoadManager", "requestPreloadPlayerResource"},
    {"app.AdditionalPlayerLoadManager", "requestLoadPlayerResource"},
    {"app.AdditionalPlayerLoadManager", "requestUnloadPlayerResource"},
    {"app.AdditionalPlayerLoadManager", "requestFinishPlayerResource"},
    {"app.AdditionalPlayerLoadManager", "requestFinishAllPlayerResource"},

    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "init"},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "preloadNakidPlayerResource"},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "loadAddtionalPlayerResource"},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "activatePlayerResource"},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "unloadAdditionalPlayerResource"},
    {"app.AdditionalPlayerLoadManager.AdditionalPlayerLoader", "set_CurrentState"},

    {"app.CharacterFactory", "load"},
    {"app.CharacterFactory", "create"},      // las dos sobrecargas
    {"app.CharacterFactory", "unload"},
    {"app.CharacterFactory", "unloadAll"},

    {"app.NetworkCharacterManager", "registerCharacter"},
    {"app.NetworkCharacterManager", "releaseCharacter"},
};
constexpr int kNumTargets = static_cast<int>(sizeof(kTargets) / sizeof(kTargets[0]));
bool g_target_done[kNumTargets] = {};

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
            return std::string(b) + rf::obj_type(static_cast<rf::Obj*>(raw));
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
    if (!g_enabled.load(std::memory_order_relaxed) || n > kMaxLinesPerSlot || !argv)
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
    if (!g_enabled.load(std::memory_order_relaxed) || s.hits.load(std::memory_order_relaxed) > kMaxLinesPerSlot) return;
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

void set_enabled(bool on) { g_enabled.store(on); log("trace: %s", on ? "ACTIVADO" : "DESACTIVADO"); }

void summary() {
    log("trace: resumen de %d hooks", g_count);
    for (int i = 0; i < g_count; ++i)
        log("trace:   %-90s hits=%u", g_slots[i].label.c_str(), g_slots[i].hits.load());
}

} // namespace ss::trace
