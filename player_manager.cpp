#include "player_manager.hpp"
#include "camera_manager.hpp"
#include "reflect_util.hpp"

namespace ss::player {

    namespace {

        using rf::Obj;

        Obj* g_mgr = nullptr;
        Obj* g_p1 = nullptr;
        Obj* g_p2 = nullptr;
        Obj* g_p2_go = nullptr;

        // Instancia de PlayerManager capturada por su hook.
        std::atomic<void*> g_mgr_from_hook{ nullptr };

        // Instancias de los gestores capturadas por sus hooks.
        std::atomic<void*> g_factory_from_hook{ nullptr };
        std::atomic<void*> g_additional_from_hook{ nullptr };

        // Indican si se ejecutaron sus métodos doUpdate().
        std::atomic<bool> g_factory_hook_called{ false };
        std::atomic<bool> g_additional_hook_called{ false };

        // Instancias observadas durante el diagnóstico.
        Obj* g_factory = nullptr;
        Obj* g_additional_mgr = nullptr;

        bool g_char_hooked = false;
        bool g_spawn_requested = false;
        unsigned g_frame = 0;

        // Evitan repetir mensajes del diagnóstico.
        bool g_factory_hook_reported = false;
        bool g_additional_hook_reported = false;
        bool g_factory_missing_logged = false;
        bool g_additional_missing_logged = false;

        // ------------------------------------------------------------
        // Hook de app.PlayerManager::doUpdate
        // ------------------------------------------------------------

        int pre_mgr(
            int argc,
            void** argv,
            REFrameworkTypeDefinitionHandle*,
            unsigned long long
        ) {
            g_in_manager.store(1, std::memory_order_relaxed);

            if (argc > 1 && argv && argv[1]) {
                g_mgr_from_hook.store(
                    argv[1],
                    std::memory_order_relaxed
                );
            }

            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        }

        void post_mgr(
            void**,
            REFrameworkTypeDefinitionHandle,
            unsigned long long
        ) {
            g_in_manager.store(0, std::memory_order_relaxed);
        }

        // ------------------------------------------------------------
        // Hooks de diagnóstico: CharacterFactory
        // ------------------------------------------------------------

        int pre_factory(
            int argc,
            void** argv,
            REFrameworkTypeDefinitionHandle*,
            unsigned long long
        ) {
            if (argc > 1 && argv && argv[1]) {
                g_factory_from_hook.store(
                    argv[1],
                    std::memory_order_relaxed
                );

                g_factory_hook_called.store(
                    true,
                    std::memory_order_relaxed
                );
            }

            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        }

        void post_factory(
            void**,
            REFrameworkTypeDefinitionHandle,
            unsigned long long
        ) {
        }

        // ------------------------------------------------------------
        // Hooks de diagnóstico: AdditionalPlayerLoadManager
        // ------------------------------------------------------------

        int pre_additional(
            int argc,
            void** argv,
            REFrameworkTypeDefinitionHandle*,
            unsigned long long
        ) {
            if (argc > 1 && argv && argv[1]) {
                g_additional_from_hook.store(
                    argv[1],
                    std::memory_order_relaxed
                );

                g_additional_hook_called.store(
                    true,
                    std::memory_order_relaxed
                );
            }

            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        }

        void post_additional(
            void**,
            REFrameworkTypeDefinitionHandle,
            unsigned long long
        ) {
        }

        // ------------------------------------------------------------
        // Hook experimental del personaje
        // ------------------------------------------------------------

        int pre_char(
            int argc,
            void** argv,
            REFrameworkTypeDefinitionHandle*,
            unsigned long long
        ) {
            void* self = argc > 1 && argv ? argv[1] : nullptr;

            g_in_character.store(1, std::memory_order_relaxed);

            g_active_player.store(
                self && self == static_cast<void*>(g_p2) ? 1 : 0,
                std::memory_order_relaxed
            );

            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        }

        void post_char(
            void**,
            REFrameworkTypeDefinitionHandle,
            unsigned long long
        ) {
            g_in_character.store(0, std::memory_order_relaxed);
            g_active_player.store(0, std::memory_order_relaxed);
        }

        // ------------------------------------------------------------
        // Liberación de referencias
        // ------------------------------------------------------------

        void release(Obj*& obj) {
            if (obj) {
                obj->release();
                obj = nullptr;
            }
        }

        // ------------------------------------------------------------
        // Instalación del hook experimental de P1
        // ------------------------------------------------------------

        void install_character_hook() {
            if (g_char_hooked || !g_p1) {
                return;
            }

            auto td = g_p1->get_type_definition();

            if (!td) {
                return;
            }

            const char* type_name = td->get_name();
            const char* type_namespace = td->get_namespace();
            const std::string full_name = td->get_full_name();

            log(
                "P1 tipo index=%u fqn=%u",
                td->get_index(),
                td->get_fqn()
            );

            log(
                "P1 get_name: %s",
                type_name ? type_name : "(null)"
            );

            log(
                "P1 namespace: %s",
                type_namespace ? type_namespace : "(null)"
            );

            log(
                "P1 full_name: %s",
                full_name.c_str()
            );

            for (const char* name : {
                "doUpdate",
                "update",
                "lateUpdate"
                }) {
                auto method = rf::find_method(td, name, 0);

                if (!method) {
                    continue;
                }

                method->add_hook(pre_char, post_char, false);
                g_char_hooked = true;

                const std::string declaring_name =
                    method->get_declaring_type()->get_full_name();

                log(
                    "Hook de personaje instalado en %s::%s",
                    declaring_name.c_str(),
                    name
                );

                return;
            }

            log(
                "P1 no tiene doUpdate/update/lateUpdate sin parámetros "
                "(revisa dmc5_ss.log tras F8)"
            );
        }

        // ------------------------------------------------------------
        // Actualización de la referencia a P1
        // ------------------------------------------------------------

        void refresh_players() {
            Obj* player = rf::call_obj(
                g_mgr,
                "getPlayer",
                { rf::u32(0) },
                true
            );

            if (player == g_p1) {
                return;
            }

            release(g_p2);
            release(g_p2_go);

            camera::set_player2_target(nullptr);

            release(g_p1);

            g_p1 = player;

            if (g_p1) {
                g_p1->add_ref();

                log("P1 detectado (getPlayer(0) != null)");

                install_character_hook();
            }
            else {
                log("P1 desapareció (getPlayer(0) == null)");
            }
        }

        // ------------------------------------------------------------
        // Intento experimental de clonado (sin cambios)
        // ------------------------------------------------------------

        bool try_spawn_p2() {
            if (!g_p1) {
                log("Spawn: P1 aún no existe (entra en una misión)");
                return false;
            }

            Obj* obj_mgr =
                REF::get()->get_managed_singleton("app.ObjectManager");

            if (!obj_mgr) {
                log("Spawn: app.ObjectManager no disponible.");
                return false;
            }

            Obj* go1 = nullptr;

            for (uint32_t id = 0; id <= 4; ++id) {
                Obj* candidate = rf::call_obj(
                    obj_mgr,
                    "getPlayer",
                    { rf::u32(id) },
                    true
                );

                if (candidate) {
                    log(
                        "Spawn: ObjectManager encontró GameObject "
                        "para PlayerID %u",
                        id
                    );

                    go1 = candidate;
                    break;
                }
            }

            if (!go1) {
                log(
                    "Spawn: no se encontró GameObject "
                    "para PlayerID 0-4."
                );

                return false;
            }

            auto gt = rf::type("via.GameObject");

            rf::Method* clone = nullptr;

            for (const char* name : {
                "clone",
                "instantiate",
                "duplicate"
                }) {
                clone = rf::find_method(gt, name, 0);

                if (clone) {
                    break;
                }
            }

            if (!clone) {
                log(
                    "Spawn: via.GameObject no tiene clone/instantiate/"
                    "duplicate. Se necesita usar el sistema de carga "
                    "app.AdditionalPlayerLoadManager."
                );

                return false;
            }

            std::vector<void*> clone_args;

            rf::Ret result = rf::from_invoke_ret(
                clone->invoke(go1, clone_args)
            );

            Obj* go2 = result.exception_thrown
                ? nullptr
                : static_cast<Obj*>(result.ptr);

            if (!go2) {
                log("Spawn: el clonado falló o devolvió null");
                return false;
            }

            rf::TypeDef* player_type =
                g_p1 ? g_p1->get_type_definition() : nullptr;

            Obj* p2 = player_type
                ? rf::call_obj(go2, "getComponent", { player_type })
                : nullptr;

            if (!p2) {
                log("Spawn: el clon no contiene el componente de jugador");
                return false;
            }

            Obj* t1 = rf::call_obj(go1, "get_Transform");
            Obj* t2 = rf::call_obj(go2, "get_Transform");

            rf::V3 pos;

            if (t1 && t2 && rf::get_v3(t1, "get_Position", pos)) {
                const float position[4] = {
                    pos.x + 2.0f,
                    pos.y,
                    pos.z,
                    0.0f
                };

                rf::set_v4(t2, "set_Position", position);
            }

            g_p2 = p2;
            g_p2->add_ref();

            g_p2_go = go2;
            g_p2_go->add_ref();

            camera::set_player2_target(g_p2_go);

            log(
                "P2 creado por clonado (aún sin registrar "
                "en app.PlayerManager)."
            );

            return true;
        }

    } // namespace

    // ------------------------------------------------------------
    // Inicialización de hooks
    // ------------------------------------------------------------

    bool init() {
        auto type = rf::type("app.PlayerManager");

        if (!type) {
            log("Tipo app.PlayerManager no existe");
            return false;
        }

        auto method = rf::find_method(type, "doUpdate", 0);

        if (!method) {
            log("app.PlayerManager::doUpdate no encontrado");
            return false;
        }

        method->add_hook(pre_mgr, post_mgr, false);

        log("Hook en app.PlayerManager::doUpdate instalado");

        // Hook de diagnóstico de CharacterFactory.
        auto factory_type = rf::type("app.CharacterFactory");

        auto factory_update = rf::find_method(
            factory_type,
            "doUpdate",
            0
        );

        if (factory_update) {
            factory_update->add_hook(
                pre_factory,
                post_factory,
                false
            );

            log(
                "Hook de diagnóstico en "
                "app.CharacterFactory::doUpdate instalado"
            );
        }
        else {
            log(
                "No se pudo instalar el hook: "
                "app.CharacterFactory::doUpdate no encontrado"
            );
        }

        // Hook de diagnóstico de AdditionalPlayerLoadManager.
        auto additional_type =
            rf::type("app.AdditionalPlayerLoadManager");

        auto additional_update = rf::find_method(
            additional_type,
            "doUpdate",
            0
        );

        if (additional_update) {
            additional_update->add_hook(
                pre_additional,
                post_additional,
                false
            );

            log(
                "Hook de diagnóstico en "
                "app.AdditionalPlayerLoadManager::doUpdate instalado"
            );
        }
        else {
            log(
                "No se pudo instalar el hook: "
                "app.AdditionalPlayerLoadManager::doUpdate "
                "no encontrado"
            );
        }

        return true;
    }

    // ------------------------------------------------------------
    // Actualización por frame
    // ------------------------------------------------------------

    void on_frame() {
        ++g_frame;

        // Diagnóstico periódico: no crear ni clonar personajes.
        if (g_frame % 60 == 1) {

            // CharacterFactory: intentar singleton y luego hook.
            Obj* factory_singleton =
                REF::get()->get_managed_singleton(
                    "app.CharacterFactory"
                );

            Obj* factory_hook = static_cast<Obj*>(
                g_factory_from_hook.load(
                    std::memory_order_relaxed
                )
                );

            Obj* factory = factory_singleton
                ? factory_singleton
                : factory_hook;

            if (factory && factory != g_factory) {
                g_factory = factory;

                log(
                    "CharacterFactory: instancia obtenida mediante "
                    "%s (%p)",
                    factory_singleton ? "singleton" : "hook doUpdate",
                    static_cast<void*>(factory)
                );
            }
            else if (!factory && !g_factory_missing_logged) {
                log(
                    "CharacterFactory: singleton null y todavía "
                    "no se capturó una instancia por hook"
                );

                g_factory_missing_logged = true;
            }

            if (
                g_factory_hook_called.load(
                    std::memory_order_relaxed
                ) &&
                !g_factory_hook_reported
                ) {
                log(
                    "CharacterFactory::doUpdate ejecutado; instancia=%p",
                    g_factory_from_hook.load(
                        std::memory_order_relaxed
                    )
                );

                g_factory_hook_reported = true;
            }

            // AdditionalPlayerLoadManager: singleton y hook.
            Obj* additional_singleton =
                REF::get()->get_managed_singleton(
                    "app.AdditionalPlayerLoadManager"
                );

            Obj* additional_hook = static_cast<Obj*>(
                g_additional_from_hook.load(
                    std::memory_order_relaxed
                )
                );

            Obj* additional = additional_singleton
                ? additional_singleton
                : additional_hook;

            if (additional && additional != g_additional_mgr) {
                g_additional_mgr = additional;

                log(
                    "AdditionalPlayerLoadManager: instancia obtenida "
                    "mediante %s (%p)",
                    additional_singleton ? "singleton" : "hook doUpdate",
                    static_cast<void*>(additional)
                );
            }
            else if (
                !additional &&
                !g_additional_missing_logged
                ) {
                log(
                    "AdditionalPlayerLoadManager: singleton null y "
                    "todavía no se capturó una instancia por hook"
                );

                g_additional_missing_logged = true;
            }

            if (
                g_additional_hook_called.load(
                    std::memory_order_relaxed
                ) &&
                !g_additional_hook_reported
                ) {
                log(
                    "AdditionalPlayerLoadManager::doUpdate ejecutado; "
                    "instancia=%p",
                    g_additional_from_hook.load(
                        std::memory_order_relaxed
                    )
                );

                g_additional_hook_reported = true;
            }
        }

        // Obtener PlayerManager.
        if (!g_mgr) {
            g_mgr =
                REF::get()->get_managed_singleton("app.PlayerManager");

            if (g_mgr) {
                log(
                    "app.PlayerManager obtenido como singleton gestionado"
                );
            }
            else {
                void* manager = g_mgr_from_hook.load(
                    std::memory_order_relaxed
                );

                if (manager) {
                    g_mgr = static_cast<Obj*>(manager);

                    log(
                        "app.PlayerManager obtenido del hook de doUpdate"
                    );
                }
                else if (g_frame % 600 == 0) {
                    log("app.PlayerManager todavía no disponible");
                }
            }
        }

        if (!g_mgr) {
            return;
        }

        // Actualizar la referencia a P1 periódicamente.
        if (g_frame % 30 == 0) {
            refresh_players();
        }

        // F9: conserva el intento experimental anterior.
        if (g_spawn_requested) {
            g_spawn_requested = false;

            if (g_p2) {
                log("P2 ya existe");
            }
            else {
                try_spawn_p2();
            }
        }
    }

    void request_spawn() {
        g_spawn_requested = true;
    }

    bool has_player2() {
        return g_p2 != nullptr;
    }

    // ------------------------------------------------------------
    // Diagnóstico solicitado por F8
    // ------------------------------------------------------------

    void dump_info() {
        // Métodos de PlayerManager.
        rf::dump_type("app.PlayerManager");

        // Métodos del gestor de carga de jugadores adicionales.
        rf::dump_additional_player_manager_info();

        // Campos del gestor, del loader y de PlayerManager.
        rf::dump_additional_player_fields();

        // Métodos de CharacterFactory y RequestData.
        rf::dump_type("app.CharacterFactory");
        rf::dump_type("app.CharacterFactory.RequestData");

        // Métodos del tipo real del personaje P1.
        if (g_p1 && g_p1->get_type_definition()) {
            std::string full_name =
                g_p1->get_type_definition()->get_full_name();

            rf::dump_type(full_name.c_str());
        }
        else {
            log(
                "dump_info: P1 aún no existe; entra en una misión "
                "y pulsa F8 de nuevo"
            );
        }
    }

} // namespace ss::player
