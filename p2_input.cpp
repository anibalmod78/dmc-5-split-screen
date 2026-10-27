#include "p2_input.hpp"
#include "reflect_util.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

namespace ss::p2input {
namespace {

using rf::Obj;

thread_local Obj* t_pad  = nullptr;   // PadInput cuyo update() se ejecuta ahora en este hilo
thread_local bool t_self = false;     // update() llamado por este plugin (no cuenta como del juego)

std::atomic<void*>    g_target{nullptr};
std::atomic<int>      g_user{-1};            // -1: sin intercambio; >=0: UserIndex que leera el pad de P2
std::atomic<unsigned> g_target_updates{0};   // update() del juego sobre el pad de P2
unsigned              g_last_target_updates = 0;
unsigned              g_ticks = 0;
bool                  g_installed = false;

std::mutex g_seen_mtx;
std::set<std::tuple<void*, int, int>> g_seen;   // (pad, ranura de GamePad, usuario) ya registrados
std::array<std::string, 6> g_labels;
int g_nlabels = 0;

int pre_update(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    t_pad = (argc > 1) ? static_cast<Obj*>(argv[1]) : nullptr;
    if (!t_self && t_pad && t_pad == g_target.load(std::memory_order_relaxed))
        g_target_updates.fetch_add(1, std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_update(void**, REFrameworkTypeDefinitionHandle, unsigned long long) { t_pad = nullptr; }
void post_gp(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

template <int I>
int pre_gp(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!t_pad || argc < 2 || !argv) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int user = static_cast<int>(reinterpret_cast<intptr_t>(argv[1]) & 0xFFFFFFFF);
    const bool is_target = t_pad == g_target.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_seen_mtx);
        if (g_seen.size() < 60 && g_seen.insert({t_pad, I, user}).second)
            log("GamePad.%s(user=%d) llamado DENTRO de PadInput.update  pad=%p%s%s", g_labels[I].c_str(), user,
                static_cast<void*>(t_pad), is_target ? " [PAD DE P2]" : "", t_self ? " (update propio)" : "");
    }
    const int swap = g_user.load(std::memory_order_relaxed);
    if (swap >= 0 && is_target) argv[1] = reinterpret_cast<void*>(static_cast<intptr_t>(swap));
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

template <int... Is> constexpr auto make_gp(std::integer_sequence<int, Is...>) {
    return std::array<REFPreHookFn, sizeof...(Is)>{ &pre_gp<Is>... };
}
const auto kGpPre = make_gp(std::make_integer_sequence<int, 6>{});

} // namespace

bool install() {
    if (g_installed) return true;
    auto tPad = rf::type("app.PadInput");
    auto tGp  = rf::type("via.hid.GamePad");
    if (!tPad || !tGp) return false;

    for (auto m : rf::own_methods_named(tPad, "update")) {
        if (m->get_num_params() != 0) continue;
        m->add_hook(pre_update, post_update, false);
        log("p2input: hook en app.PadInput::update/0");
    }
    for (const char* name : {"getDevice", "getMergedDevice"}) {
        for (auto m : rf::own_methods_named(tGp, name)) {
            if (g_nlabels >= 6 || m->get_num_params() < 1) continue;
            const auto pt = rf::param_types(m);
            if (pt.empty() || rf::tname(pt[0]) != "via.UserIndex") continue;
            g_labels[g_nlabels] = std::string(name) + "/" + std::to_string(m->get_num_params());
            m->add_hook(kGpPre[g_nlabels], post_gp, false);
            log("p2input: hook en via.hid.GamePad::%s", g_labels[g_nlabels].c_str());
            ++g_nlabels;
        }
    }
    g_installed = true;
    return true;
}

void set_target(void* pad) {
    g_target.store(pad);
    g_target_updates.store(0);
    g_last_target_updates = 0;
    g_ticks = 0;
    log("p2input: PadInput de P2 = %p", pad);
}

void command(const std::string& arg) {
    if (arg.rfind("on", 0) == 0) {
        int user = 1;
        if (arg.size() > 2) user = std::atoi(arg.c_str() + 2);
        if (!g_target.load()) { log("p2pad: no hay PadInput de P2 (usa p2adopt o p2load antes)"); return; }
        g_user.store(user);
        log("p2pad: ACTIVADO. El PadInput de P2 leera el usuario %d (via.hid.GamePad). Se llamara a su update() si el juego no lo hace.", user);
    } else if (arg == "off") {
        g_user.store(-1);
        log("p2pad: DESACTIVADO");
    } else {
        log("p2pad: estado: pad P2=%p usuario=%d updates_del_juego=%u ticks_propios=%u hooks=%d",
            g_target.load(), g_user.load(), g_target_updates.load(), g_ticks, g_installed ? 1 : 0);
    }
}

void on_frame() {
    Obj* pad = static_cast<Obj*>(g_target.load());
    if (!pad || g_user.load() < 0) return;
    const unsigned cur = g_target_updates.load();
    if (cur != g_last_target_updates) { g_last_target_updates = cur; return; }   // el juego ya lo actualiza
    t_self = true;
    rf::Ret r{};
    rf::invoke(pad, "update", {}, r, false);
    t_self = false;
    ++g_ticks;
}

} // namespace ss::p2input
