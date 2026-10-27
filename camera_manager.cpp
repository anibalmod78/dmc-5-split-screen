// Sonda de camara/render. Solo lee; no modifica nada.
// Objetivo: saber cuantas SceneView existen, el tamano de la principal y los
// parametros de la camara de P1 antes de decidir como partir la pantalla.
#include "camera_manager.hpp"
#include "reflect_util.hpp"

namespace ss::camera {

void probe() {
    log("=========== SONDA DE CAMARA ===========");
    void* sm_raw = REF::get()->get_native_singleton("via.SceneManager");
    rf::Obj* sm = static_cast<rf::Obj*>(sm_raw);
    log("via.SceneManager nativo=%p", sm_raw);
    if (!sm) return;

    // Metodos NATIVOS: se invocan por el tipo declarado (no hay objeto gestionado).
    const std::vector<void*> none;
    auto call_native = [&](void* self, const char* tn, const char* mn) -> void* {
        auto m = rf::find_method(rf::type(tn), mn, 0);
        if (!m) { log("  falta %s::%s", tn, mn); return nullptr; }
        rf::Ret r = m->invoke(static_cast<rf::Obj*>(self), none);
        return r.exception_thrown ? nullptr : r.ptr;
    };

    void* view = call_native(sm, "via.SceneManager", "get_MainView");
    log("MainView=%p", view);
    if (!view) return;

    if (auto m = rf::find_method(rf::type("via.SceneView"), "get_WindowSize", 0)) {
        rf::Ret r = m->invoke(static_cast<rf::Obj*>(view), none);
        float wh[2]{}; std::memcpy(wh, r.bytes.data(), sizeof(wh));
        log("  WindowSize (como float)=%g x %g   (como int32)=%d x %d", wh[0], wh[1],
            *reinterpret_cast<int32_t*>(&wh[0]), *reinterpret_cast<int32_t*>(&wh[1]));
    }
    if (auto m = rf::find_method(rf::type("via.SceneView"), "get_PresentRect", 0)) {
        rf::Ret r = m->invoke(static_cast<rf::Obj*>(view), none);
        float f[4]{}; std::memcpy(f, r.bytes.data(), sizeof(f));
        log("  PresentRect (como float)=%g %g %g %g   (como int32)=%d %d %d %d", f[0], f[1], f[2], f[3],
            *reinterpret_cast<int32_t*>(&f[0]), *reinterpret_cast<int32_t*>(&f[1]),
            *reinterpret_cast<int32_t*>(&f[2]), *reinterpret_cast<int32_t*>(&f[3]));
    }

    void* cam = call_native(view, "via.SceneView", "get_PrimaryCamera");
    log("PrimaryCamera=%p", cam);
    if (cam) {
        // El objeto es NATIVO: no se puede deducir su tipo con get_type_definition();
        // se invoca por el tipo declarado via.Camera (antes salian valores basura).
        for (const char* g : {"get_FOV", "get_AspectRatio", "get_NearClipPlane", "get_FarClipPlane"}) {
            auto m = rf::find_method(rf::type("via.Camera"), g, 0);
            if (!m) { log("  falta via.Camera::%s", g); continue; }
            rf::Ret r = m->invoke(static_cast<rf::Obj*>(cam), none);
            // Un float nativo puede venir en xmm0 y no en el buffer: se muestran los bytes crudos.
            log("  %s: f=%g raw=0x%08x qword=0x%016llx%s", g, r.f, r.dword,
                static_cast<unsigned long long>(r.qword), r.exception_thrown ? " (excepcion)" : "");
        }
    }

    // via.SceneManager.get_SceneViews devuelve un array/lista: solo se informa del tipo.
    rf::Obj* views = static_cast<rf::Obj*>(call_native(sm, "via.SceneManager", "get_SceneViews"));
    log("SceneViews objeto=%p tipo=%s", static_cast<void*>(views), rf::obj_type(views).c_str());
}

} // namespace ss::camera
