
#pragma once
#include "common.hpp"
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ss::rf {

    using Obj = reframework::API::ManagedObject;
    using TypeDef = reframework::API::TypeDefinition;
    using Method = reframework::API::Method;
    using Field = reframework::API::Field;

    struct Ret {
        uint64_t raw{};
        bool exception_thrown{};
        std::vector<uint8_t> bytes;
        float f{};
        void* ptr{};
    };

    inline Ret from_invoke_ret(const reframework::InvokeRet& r) {
        Ret out{};
        out.exception_thrown = r.exception_thrown;
        out.bytes.assign(r.bytes.begin(), r.bytes.end());
        out.raw = r.qword;
        out.f = r.f;
        out.ptr = r.ptr;
        return out;
    }

    inline TypeDef* type(const char* name) {
        auto tdb = REF::get()->tdb();
        return tdb ? tdb->find_type(name) : nullptr;
    }

    inline TypeDef* runtime_type(Obj* obj) {
        return obj ? obj->get_type_definition() : nullptr;
    }

    inline TypeDef* runtime_type(const char* name) {
        return type(name);
    }

    inline Method* find_method(
        TypeDef* td,
        const char* name,
        int num_params = -1
    ) {
        if (!td || !name) return nullptr;

        for (auto* m : td->get_methods()) {
            if (!m) continue;

            const char* method_name = m->get_name();
            if (!method_name) continue;

            if (strcmp(method_name, name) == 0) {
                if (num_params < 0 ||
                    static_cast<int>(m->get_num_params()) == num_params) {
                    return m;
                }
            }
        }

        return nullptr;
    }

    inline bool invoke(
        Obj* obj,
        const char* method_name,
        const std::vector<void*>& args,
        Ret& out,
        bool instance = true
    ) {
        if (!obj || !method_name) return false;

        auto* td = runtime_type(obj);
        if (!td) return false;

        auto* m = find_method(td, method_name);
        if (!m) return false;

        auto r = m->invoke(instance ? obj : nullptr, args);

        out.exception_thrown = r.exception_thrown;
        out.bytes.assign(r.bytes.begin(), r.bytes.end());
        out.raw = r.qword;
        out.f = r.f;
        out.ptr = r.ptr;

        return !r.exception_thrown;
    }

    inline Obj* call_obj(
        Obj* obj,
        const char* method_name,
        const std::vector<void*>& args = {},
        bool instance = true
    ) {
        Ret r{};
        if (invoke(obj, method_name, args, r, instance)) {
            return static_cast<Obj*>(r.ptr);
        }
        return nullptr;
    }

    inline void* call_native(
        void* obj,
        const char* type_name,
        const char* method_name,
        const std::vector<void*>& args = {},
        bool instance = true
    ) {
        auto* td = type(type_name);
        auto* m = find_method(td, method_name);

        if (!m) return nullptr;

        auto r = m->invoke(
            instance ? static_cast<Obj*>(obj) : nullptr,
            args
        );

        if (r.exception_thrown) return nullptr;
        return r.ptr;
    }

    // Matemática auxiliar para vectores y matrices.
    struct V3 {
        float x, y, z;
    };

    inline V3 operator-(const V3& a, const V3& b) {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    inline bool get_v3(Obj* obj, const char* method, V3& out) {
        Ret r{};
        if (!invoke(obj, method, {}, r, true)) return false;

        if (r.bytes.size() < sizeof(V3)) return false;

        std::memcpy(&out, r.bytes.data(), sizeof(V3));
        return true;
    }

    inline void set_v4(Obj* obj, const char* method, const float v[4]) {
        std::vector<void*> args = { const_cast<float*>(v) };
        Ret r{};
        invoke(obj, method, args, r, true);
    }

    inline void look_rotation(const V3& dir, float q[4]) {
        float len = std::sqrt(
            dir.x * dir.x + dir.y * dir.y + dir.z * dir.z
        );

        if (len < 0.0001f) {
            q[0] = 0;
            q[1] = 0;
            q[2] = 0;
            q[3] = 1;
            return;
        }

        float f[3] = {
            dir.x / len,
            dir.y / len,
            dir.z / len
        };

        float yaw = std::atan2(-f[0], -f[2]);
        float pitch = std::asin(f[1]);

        q[0] = std::sin(pitch * 0.5f) * std::cos(yaw * 0.5f);
        q[1] = std::cos(pitch * 0.5f) * std::sin(yaw * 0.5f);
        q[2] = std::sin(pitch * 0.5f) * std::sin(yaw * 0.5f);
        q[3] = std::cos(pitch * 0.5f) * std::cos(yaw * 0.5f);
    }

    inline void dump_type(
        const char* type_name,
        const std::vector<std::string>& keys = {}
    ) {
        auto* td = type(type_name);

        if (!td) {
            log_dump("Tipo no encontrado: %s", type_name);
            return;
        }

        log_dump(
            "--- DUMP: %s (methods: %zu) ---",
            type_name,
            td->get_methods().size()
        );

        for (auto* m : td->get_methods()) {
            if (!m) continue;

            const char* mname = m->get_name();
            if (!mname) continue;

            if (!keys.empty()) {
                bool match = false;
                std::string lower_m = mname;

                std::transform(
                    lower_m.begin(),
                    lower_m.end(),
                    lower_m.begin(),
                    [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    }
                );

                for (const auto& k : keys) {
                    std::string lower_k = k;

                    std::transform(
                        lower_k.begin(),
                        lower_k.end(),
                        lower_k.begin(),
                        [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        }
                    );

                    if (lower_m.find(lower_k) != std::string::npos) {
                        match = true;
                        break;
                    }
                }

                if (!match) continue;
            }

            auto params = m->get_params();
            auto* return_type = m->get_return_type();

            const char* return_name = return_type
                ? return_type->get_name()
                : "void/unknown";

            log_dump(
                "  Method: %s (params: %u) -> %s",
                mname,
                m->get_num_params(),
                return_name
            );

            for (size_t i = 0; i < params.size(); ++i) {
                auto* param_type =
                    reinterpret_cast<TypeDef*>(params[i].t);

                const char* type_name_param = "unknown";
                const char* full_type_name = "unknown";
                std::string found_full_name;

                if (param_type) {
                    const char* found_name = param_type->get_name();

                    found_full_name = param_type->get_full_name();

                    if (found_name) {
                        type_name_param = found_name;
                    }

                    full_type_name = found_full_name.c_str();
                }

                const char* param_name = params[i].name
                    ? params[i].name
                    : "?";

                log_dump(
                    "    Param %zu: %s | full: %s | name: %s",
                    i,
                    type_name_param,
                    full_type_name,
                    param_name
                );
            }
        }
    }

    // NUEVO: inspecciona metadatos de campos.
    // No lee valores de objetos ni invoca métodos del juego.
    inline void dump_fields(const char* type_name) {
        auto* td = type(type_name);

        if (!td) {
            log_dump("[fields] Tipo no encontrado: %s", type_name);
            return;
        }

        auto fields = td->get_fields();

        log_dump(
            "--- CAMPOS: %s (fields: %zu) ---",
            type_name,
            fields.size()
        );

        for (auto* field : fields) {
            if (!field) continue;

            const char* field_name = field->get_name();
            auto* field_type = field->get_type();

            const char* field_type_name = field_type
                ? field_type->get_name()
                : "<tipo nulo>";

            std::string field_full_name;

            if (field_type) {
                field_full_name = field_type->get_full_name();
            }
            else {
                field_full_name = "<tipo nulo>";
            }

            log_dump(
                "  Field: %s | type: %s | full: %s | static: %u | literal: %u",
                field_name ? field_name : "<sin nombre>",
                field_type_name ? field_type_name : "<sin nombre>",
                field_full_name.c_str(),
                field->is_static() ? 1u : 0u,
                field->is_literal() ? 1u : 0u
            );
        }
    }

    // NUEVO: diagnóstico de campos de los tipos relacionados con P2.
    // Solo enumera metadatos; no altera el estado del juego.
    inline void dump_additional_player_fields() {
        const char* names[] = {
            "app.AdditionalPlayerLoadManager",
            "app.AdditionalPlayerLoadManager.AdditionalPlayerLoader",
            "app.PlayerManager"
        };

        for (const char* name : names) {
            dump_fields(name);
        }
    }

    // Diagnóstico conservador: inspecciona nombres de métodos.
    inline void dump_additional_player_manager_info() {
        const char* names[] = {
            "app.AdditionalPlayerLoadManager",
            "app.AdditionalPlayerLoadManager.AdditionalPlayerLoader"
        };

        for (const char* name : names) {
            auto* td = type(name);

            if (!td) {
                log_dump("[additional] Tipo no encontrado: %s", name);
                continue;
            }

            log_dump("[additional] Tipo: %s", name);

            for (auto* m : td->get_methods()) {
                if (!m) continue;

                const char* method_name = m->get_name();
                if (!method_name) continue;

                std::string n = method_name;

                std::transform(
                    n.begin(),
                    n.end(),
                    n.begin(),
                    [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    }
                );

                if (n.find("list") != std::string::npos ||
                    n.find("loader") != std::string::npos ||
                    n.find("player") != std::string::npos ||
                    n.find("request") != std::string::npos ||
                    n.find("state") != std::string::npos) {
                    log_dump(
                        "[additional] Método relacionado: %s",
                        method_name
                    );
                }
            }
        }
    }

    inline void* f32(float v) {
        void* p;
        std::memcpy(&p, &v, sizeof(float));
        return p;
    }

    inline void* u32(uint32_t v) {
        void* p;
        std::memcpy(&p, &v, sizeof(uint32_t));
        return p;
    }

    inline void* b(bool v) {
        uint32_t val = v ? 1 : 0;
        void* p;
        std::memcpy(&p, &val, sizeof(uint32_t));
        return p;
    }

} // namespace ss::rf