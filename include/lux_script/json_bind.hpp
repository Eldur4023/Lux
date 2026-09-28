#pragma once

#include <lux_script/value.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace lux_script {

// ─── Schema-aware JSON body binding ──────────────────────────────────────────
//
// Value::parse_json() builds the whole generic tree — a Dict per object, a
// std::string per key — and only THEN does the body binding walk it looking
// for the fields the class actually declares. For a body bound to a class the
// tree is scaffolding: every key the class doesn't declare is materialized and
// thrown away, and every declared key pays a Dict node plus a key copy on the
// way in and a lookup on the way out.
//
// The entry points here parse the same bytes straight into the class's shape:
// each key is matched against the declared fields as it arrives, declared
// scalars are read directly into Values, unknown keys are validated and
// skipped without building anything, and nested classes / List<Class> recurse
// through the same path.
//
// The observable behavior is byte-for-byte the one bind_body() (project.cpp)
// has always had, and its callers keep owning every message: the binder never
// words a message, it only reports, per declared field, whether the body had
// it (Ok), didn't (Missing — absent, or JSON null), or had it with the wrong
// type (BadType). Invalid JSON and a non-object body are reported apart, so
// the caller can keep answering 400 "invalid JSON" and 422 "the body must be
// a JSON object" exactly as before.

// The scalar flavors a field can hold (the element's, when is_list).
enum class JsonScalar : uint8_t { Str, Bool, Int, Float };

// "no nested class" marker for JsonFieldSpec::nested.
inline constexpr uint32_t kJsonNoNested = 0xFFFFFFFFu;

struct JsonFieldSpec {
    std::string_view name;   // points into the caller's class metadata
    JsonScalar       kind;
    bool             optional = false;
    bool             is_list  = false;
    uint32_t         nested   = kJsonNoNested;  // shape-table index of the element class
};

// The shapes the binder can bind to, by index. One virtual dispatch per key
// of the body — nothing else crosses back into the caller's metadata.
class JsonShapeTable {
public:
    virtual ~JsonShapeTable() = default;
    virtual size_t               field_count(uint32_t cls) const   = 0;
    virtual const JsonFieldSpec& field(uint32_t cls, size_t i) const = 0;
};

enum class JsonBindStatus : uint8_t {
    Missing,  // absent from the body, or JSON null
    Ok,
    BadType,  // present, valid JSON, wrong type for the field
};

enum class JsonBindError : uint8_t {
    None,
    InvalidJson,  // not JSON at all, garbage after the document, nesting cap
    NotAnObject,  // valid JSON, but the body is not an object
};

struct JsonBound {
    // Per declared field, in declaration order. With build_instance the
    // values are MOVED into `instance` and this vector is left empty;
    // without it, `values[i]` is the field's value when status[i] == Ok
    // (float fields already normalized to Value::real).
    std::vector<Value>          values;
    std::vector<JsonBindStatus> status;

    // A Dict with EVERY declared field in declaration order — null where the
    // field is missing or of the wrong type; unknown body keys are not in it.
    // Only built when asked (see build_instance).
    Value instance;
};

// Binds `text` to the class `cls` of `shapes`.
JsonBindError bind_json_class(std::string_view text, const JsonShapeTable& shapes,
                              uint32_t cls, JsonBound& out, bool build_instance = true);

// Convenience for flat, scalar-only classes (the --native route binder's
// case): one class, no shape table to build.
JsonBindError bind_json_flat(std::string_view text, const JsonFieldSpec* fields,
                             size_t count, JsonBound& out, bool build_instance = false);

} // namespace lux_script
