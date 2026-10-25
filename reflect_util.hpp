#pragma once
// ============================================================================
// reflect_util.hpp - utilidades de reflexion sobre la TDB de REFramework.
// Escrito contra el API.hpp que trae el repo (deps/reframework/API.hpp).
// Todo se ejecuta en el hilo del juego (callback de UpdateBehavior); las
// caches no tienen bloqueo.
// ============================================================================
#include "common.hpp"
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ss::rf {

using TypeDef = REF::TypeDefinition;
using Method  = REF::Method;
using Field   = REF::Field;
using Obj     = REF::ManagedObject;
using Ret     = reframework::InvokeRet;

// ---- Empaquetado de argumentos: cada arg ocupa 8 bytes ---------------------
inline void* iv(int64_t v) { return reinterpret_cast<void*>(static_cast<intptr_t>(v)); }
inline void* fv(float f) { uintptr_t r = 0; std::memcpy(&r, &f, sizeof(f)); return reinterpret_cast<void*>(r); }

// ---- Tipos --------------------------------------------------------------------
inline TypeDef* type(const char* full_name) {
    auto tdb = REF::get()->tdb();
    return tdb ? tdb->find_type(full_name) : nullptr;
}

inline std::string tname(TypeDef* t) { return t ? t->get_full_name() : std::string("<null>"); }

inline std::string obj_type(Obj* o) {
    if (!o) return "null";
    auto td = o->get_type_definition();
    return td ? tname(td) : std::string("?");
}

inline bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

// ---- Metodos ------------------------------------------------------------------
// Busca por nombre y n.o de parametros (sin contar `this`) subiendo por los padres.
inline Method* find_method(TypeDef* t, std::string_view name, int nparams = -1) {
    if (!t) return nullptr;
    static std::unordered_map<std::string, Method*> cache;
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(t)) + "|" + std::string(name) + "|" + std::to_string(nparams);
    if (auto it = cache.find(key); it != cache.end()) return it->second;

    Method* found = nullptr;
    TypeDef* cur = t;
    for (int depth = 0; cur && depth < 16 && !found; ++depth, cur = cur->get_parent_type()) {
        for (auto m : cur->get_methods()) {
            if (name == m->get_name() && (nparams < 0 || static_cast<int>(m->get_num_params()) == nparams)) {
                found = m;
                break;
            }
        }
    }
    if (found) cache[key] = found;   // no cacheamos fallos: el tipo podria cargarse despues
    return found;
}

// Todas las sobrecargas con ese nombre DECLARADAS en `t` (sin subir por los padres).
inline std::vector<Method*> own_methods_named(TypeDef* t, std::string_view name) {
    std::vector<Method*> out;
    if (!t) return out;
    for (auto m : t->get_methods())
        if (name == m->get_name()) out.push_back(m);
    return out;
}

inline std::vector<TypeDef*> param_types(Method* m) {
    std::vector<TypeDef*> out;
    for (const auto& p : m->get_params()) out.push_back(reinterpret_cast<TypeDef*>(p.t));
    return out;
}

// ---- Clasificacion de tipos ------------------------------------------------------
enum class Kind { Enum, Bool, Int, Float, Double, String, Object, Struct, Unknown };

inline Kind kind_of(TypeDef* t) {
    if (!t) return Kind::Unknown;
    if (t->is_enum()) return Kind::Enum;
    const std::string n = tname(t);
    if (n == "System.Boolean") return Kind::Bool;
    if (n == "System.Single")  return Kind::Float;
    if (n == "System.Double")  return Kind::Double;
    if (n == "System.String")  return Kind::String;
    static const char* ints[] = {"System.Int32", "System.UInt32", "System.Int64", "System.UInt64",
                                 "System.Int16", "System.UInt16", "System.Byte", "System.SByte",
                                 "System.Char", "System.IntPtr", "System.UIntPtr"};
    for (auto s : ints) if (n == s) return Kind::Int;
    switch (t->get_vm_obj_type()) {
        case REFRAMEWORK_VM_OBJ_TYPE_OBJECT:
        case REFRAMEWORK_VM_OBJ_TYPE_ARRAY:
        case REFRAMEWORK_VM_OBJ_TYPE_DELEGATE: return Kind::Object;
        case REFRAMEWORK_VM_OBJ_TYPE_STRING:   return Kind::String;
        case REFRAMEWORK_VM_OBJ_TYPE_VALTYPE:  return Kind::Struct;
        default: return Kind::Unknown;
    }
}

// ---- Enums ------------------------------------------------------------------------
inline int64_t read_int(const void* p, uint32_t size) {
    if (!p) return 0;
    switch (size) {
        case 1:  return *static_cast<const int8_t*>(p);
        case 2:  return *static_cast<const int16_t*>(p);
        case 8:  return *static_cast<const int64_t*>(p);
        default: return *static_cast<const int32_t*>(p);
    }
}

using EnumList = std::vector<std::pair<std::string, int64_t>>;

inline const EnumList& enum_values(TypeDef* t) {
    static std::unordered_map<TypeDef*, EnumList> cache;
    static const EnumList empty;
    if (!t || !t->is_enum()) return empty;
    if (auto it = cache.find(t); it != cache.end()) return it->second;

    EnumList list;
    uint32_t size = t->get_valuetype_size();
    if (size == 0 || size > 8) size = 4;
    for (auto f : t->get_fields()) {
        if (!f->is_static()) continue;                 // salta el campo de instancia "value__"
        void* p = f->get_init_data();
        if (!p) p = f->get_data_raw(nullptr, false);
        if (!p) continue;
        list.emplace_back(f->get_name(), read_int(p, size));
    }
    return cache.emplace(t, std::move(list)).first->second;
}

inline std::string enum_str(TypeDef* t, int64_t v) {
    for (const auto& [name, val] : enum_values(t))
        if (val == v) return name + "(" + std::to_string(v) + ")";
    return std::to_string(v);
}

inline bool enum_from_name(TypeDef* t, std::string_view name, int64_t& out) {
    for (const auto& [n, v] : enum_values(t))
        if (icontains(n, name) && icontains(name, n)) { out = v; return true; }   // igualdad sin distinguir mayusculas
    return false;
}

// "3" o un nombre de enumerador -> valor
inline bool parse_enum_or_int(TypeDef* t, const std::string& token, int64_t& out) {
    if (token.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(token.c_str(), &end, 0);
    if (end && *end == '\0') { out = v; return true; }
    return enum_from_name(t, token, out);
}

// ---- Invocacion ---------------------------------------------------------------------
// Invoca un metodo de INSTANCIA buscandolo por el tipo en ejecucion de `self`.
inline bool invoke(Obj* self, const char* name, const std::vector<void*>& args, Ret& out, bool verbose = true) {
    if (!self) return false;
    auto td = self->get_type_definition();
    auto m = find_method(td, name, static_cast<int>(args.size()));
    if (!m) {
        if (verbose) log("Metodo no encontrado: %s::%s(%zu args)", tname(td).c_str(), name, args.size());
        return false;
    }
    out = m->invoke(self, args);
    if (out.exception_thrown) {
        if (verbose) log("Excepcion gestionada en %s::%s", tname(td).c_str(), name);
        return false;
    }
    return true;
}

inline Obj* call_obj(Obj* self, const char* name, const std::vector<void*>& args = {}, bool verbose = true) {
    Ret r{};
    return invoke(self, name, args, r, verbose) ? static_cast<Obj*>(r.ptr) : nullptr;
}

inline bool call_i32(Obj* self, const char* name, const std::vector<void*>& args, int32_t& out, bool verbose = true) {
    Ret r{};
    if (!invoke(self, name, args, r, verbose)) return false;
    out = static_cast<int32_t>(r.dword);
    return true;
}

inline bool call_bool(Obj* self, const char* name, const std::vector<void*>& args, bool& out, bool verbose = true) {
    Ret r{};
    if (!invoke(self, name, args, r, verbose)) return false;
    out = r.byte != 0;
    return true;
}

// ---- Singletons, campos y listas ---------------------------------------------------
inline Obj* singleton(const char* name) { return REF::get()->get_managed_singleton(name); }

// Lee un campo de referencia (p. ej. una List<T>) de un objeto gestionado.
inline Obj* field_obj(Obj* o, const char* name) {
    if (!o) return nullptr;
    auto pp = o->get_field<Obj*>(name);
    return pp ? *pp : nullptr;
}
inline bool field_i32(Obj* o, const char* name, int32_t& out) {
    if (!o) return false;
    auto p = o->get_field<int32_t>(name);
    if (!p) return false;
    out = *p;
    return true;
}
inline bool field_bool(Obj* o, const char* name, bool& out) {
    if (!o) return false;
    auto p = o->get_field<uint8_t>(name);
    if (!p) return false;
    out = *p != 0;
    return true;
}

inline int list_count(Obj* list) {
    int32_t n = -1;
    if (!list) return -1;
    return call_i32(list, "get_Count", {}, n, false) ? n : -1;
}
inline Obj* list_item(Obj* list, int i) { return call_obj(list, "get_Item", {iv(i)}, false); }

// ---- Vectores ------------------------------------------------------------------------
struct V3 { float x = 0, y = 0, z = 0; };

inline bool get_v3(Obj* o, const char* getter, V3& out) {
    Ret r{};
    if (!invoke(o, getter, {}, r, false)) return false;
    std::memcpy(&out, r.bytes.data(), sizeof(V3));
    return true;
}

// Posicion mundial de un componente (via.Component -> GameObject -> Transform).
inline bool world_position(Obj* component, V3& out) {
    Obj* go = call_obj(component, "get_GameObject", {}, false);
    Obj* tr = go ? call_obj(go, "get_Transform", {}, false) : nullptr;
    return tr && get_v3(tr, "get_Position", out);
}

} // namespace ss::rf
