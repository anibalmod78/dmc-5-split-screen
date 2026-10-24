#include "camera_manager.hpp"
#include "input_isolation.hpp"
#include "reflect_util.hpp"
#include <chrono>
#include <string>

namespace ss::camera {

namespace {
using rf::Obj;

Obj* g_view   = nullptr;
Obj* g_cam1   = nullptr;
Obj* g_cam2   = nullptr;
Obj* g_go2    = nullptr;
Obj* g_tr2    = nullptr;
Obj* g_p2_go  = nullptr;
Obj* g_p2_tr  = nullptr;

bool g_create_failed = false;
unsigned g_frame = 0;

rf::Method* g_vp_setter = nullptr;
bool g_vp_searched = false;

float g_yaw = 0.f, g_pitch = 0.35f;
constexpr float kDist = 4.5f, kHeight = 1.6f;


void refresh_main_camera() {
    void* sm = REF::get()->get_native_singleton("via.SceneManager");
    if (!sm) {
        log("Camara: SceneManager es nullptr");
        return;
    }

    void* view = rf::call_native(
        sm, "via.SceneManager", "get_MainView", {}, true
    );
    if (!view) {
        log("Camara: SceneManager::get_MainView devolvio nullptr");
        return;
    }

    void* cam = rf::call_native(
        view, "via.SceneView", "get_PrimaryCamera", {}, true
    );
    if (!cam) {
        log("Camara: SceneView::get_PrimaryCamera devolvio nullptr");
        g_view = static_cast<Obj*>(view);
        g_cam1 = nullptr;
        return;
    }

    g_view = static_cast<Obj*>(view);

    if (cam != g_cam1) {
        g_cam1 = static_cast<Obj*>(cam);
        log("Camara principal (P1) localizada");
    }
}


void copy_float(Obj* from, Obj* to, const char* prop) {
    const std::string g = std::string("get_") + prop, s = std::string("set_") + prop;
    rf::Ret r{};
    if (!rf::invoke(from, g.c_str(), {}, r, false)) return;
    rf::Ret r2{};
    rf::invoke(to, s.c_str(), {rf::f32(r.f)}, r2, false);
}

bool create_cam2() {
    if (!g_cam1) return false;

    Obj* go1  = rf::call_obj(g_cam1, "get_GameObject");
    Obj* name = go1 ? rf::call_obj(go1, "get_Name") : nullptr;
    auto create = rf::find_method(rf::type("via.GameObject"), "create", 1);
    if (!name || !create) { log("Cam2: falta get_Name o via.GameObject.create(String)"); return false; }

    std::vector<void*> create_args = {name};
    rf::Ret r = rf::from_invoke_ret(
    create->invoke(nullptr, create_args)
);
    Obj* go2 = r.exception_thrown ? nullptr : static_cast<Obj*>(r.ptr);
    if (!go2) { log("Cam2: GameObject.create falló"); return false; }

    rf::TypeDef* cam_type = rf::type("via.Camera");
    Obj* cam2 = cam_type ? rf::call_obj(go2, "createComponent", {cam_type}) : nullptr;
    if (!cam2) { log("Cam2: createComponent(via.Camera) falló"); return false; }

    go2->add_ref(); cam2->add_ref();
    g_go2 = go2; g_cam2 = cam2;
    g_tr2 = rf::call_obj(go2, "get_Transform");

    for (const char* p : {"FOV", "NearClipPlane", "FarClipPlane"}) copy_float(g_cam1, g_cam2, p);

    rf::Ret e{};
    rf::invoke(g_cam2, "set_Enabled", {rf::b(true)}, e, false);

    log("Cámara 2 creada");
    return true;
}

void destroy_cam2() {
    if (g_go2) {
        if (auto d = rf::find_method(rf::type("via.GameObject"), "destroy", 1)) {
            std::vector<void*> args = {g_go2};
            d->invoke(nullptr, args);
        }
        g_go2->release();
    }
    if (g_cam2) g_cam2->release();
    g_go2 = g_cam2 = g_tr2 = nullptr;
}

float stick(SHORT v) {
    if (std::abs(static_cast<int>(v)) < 8000) return 0.f;
    return static_cast<float>(v) / 32768.f;
}

void follow_p2(float dt) {
    if (!g_cam2 || !g_p2_go || !g_tr2) return;
    if (!g_p2_tr) g_p2_tr = rf::call_obj(g_p2_go, "get_Transform");

    rf::V3 tp;
    if (!rf::get_v3(g_p2_tr, "get_Position", tp)) return;

    XINPUT_STATE st{};
    if (input::read_pad(1, &st)) {
        g_yaw   += stick(st.Gamepad.sThumbRX) * 2.5f * dt;
        g_pitch += stick(st.Gamepad.sThumbRY) * 1.5f * dt;
        g_pitch = std::fmax(-0.2f, std::fmin(1.2f, g_pitch));
    }

    const float cy = std::cos(g_yaw), sy = std::sin(g_yaw), cp = std::cos(g_pitch), sp = std::sin(g_pitch);
    const rf::V3 focus{tp.x, tp.y + kHeight, tp.z};
    const rf::V3 pos{focus.x - sy * cp * kDist, focus.y + sp * kDist, focus.z - cy * cp * kDist};

    float q[4];
    rf::look_rotation(focus - pos, q);
    const float p4[4] = {pos.x, pos.y, pos.z, 0.f};
    rf::set_v4(g_tr2, "set_Position", p4);
    rf::set_v4(g_tr2, "set_Rotation", q);
}

bool window_size(float& w, float& h) {
    if (!g_view) return false;
    rf::Ret r{};
    auto m = rf::find_method(rf::type("via.SceneView"), "get_WindowSize", 0);
    if (!m) return false;
    std::vector<void*> args = {};
    r = rf::from_invoke_ret(m->invoke(g_view, args));
    if (r.exception_thrown) return false;
    float wh[2];
    std::memcpy(wh, r.bytes.data(), sizeof(wh));
    w = wh[0]; h = wh[1];
    return w > 0 && h > 0;
}

void apply_viewports() {
    if (!g_vp_searched) {
        g_vp_searched = true;
        for (const char* n : {"set_ViewportRect", "set_ViewRect", "set_Viewport", "set_ScreenRect"}) {
            g_vp_setter = rf::find_method(rf::type("via.Camera"), n, 1);
            if (g_vp_setter) { log("Setter de viewport: via.Camera::%s", n); break; }
        }
        if (!g_vp_setter) log("via.Camera no tiene setter de viewport conocido.");
    }
    if (!g_vp_setter) return;

    float w = 0, h = 0;
    if (!window_size(w, h)) return;

    const float top[4] = {0.f, 0.f,       w, h * 0.5f};
    const float bot[4] = {0.f, h * 0.5f,  w, h * 0.5f};
    if (g_cam1) {
        std::vector<void*> args = {const_cast<float*>(top)};
        g_vp_setter->invoke(g_cam1, args);
    }
    if (g_cam2) {
        std::vector<void*> args = {const_cast<float*>(bot)};
        g_vp_setter->invoke(g_cam2, args);
    }
}
} // namespace

bool init() { return true; }

void set_player2_target(Obj* go) {
    if (go == g_p2_go) return;
    destroy_cam2();
    g_p2_tr = nullptr;
    g_p2_go = go;
    g_create_failed = false;
}

void request_retry() { g_create_failed = false; }

void update() {
    ++g_frame;
    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    const auto now = clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    dt = std::fmin(dt, 0.1f);

    if (!g_cam1 || g_frame % 30 == 0) refresh_main_camera();
    if (!g_p2_go) return;

    if (!g_cam2 && !g_create_failed) {
        if (!create_cam2()) g_create_failed = true;
    }
    if (!g_cam2) return;

    follow_p2(dt);
    apply_viewports();
}

void dump_info() {
    rf::dump_type("via.Camera", {"viewport", "rect", "view", "aspect", "fov", "primary", "render", "target", "enable"});
    rf::dump_type("via.SceneView", {"camera", "view", "window", "rect", "size", "display"});
    rf::dump_type("via.SceneManager", {"view", "scene", "camera"});
    rf::dump_type("via.render.layer.Scene", {"camera", "view", "rect", "viewport"});
}

} // namespace ss::camera