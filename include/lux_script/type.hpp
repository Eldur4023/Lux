#pragma once
#include <memory>
#include <string>

namespace lux_script {

struct TypeRef; // ast.hpp

// Typed representation of a Lux Script type, meant to replace the ad-hoc
// strings that Emitter uses today (Local::type, type_of()) and to be the
// basis of the typed IR that the native compilation backend needs
// (--native, phase 1).
//
// NOT YET CONNECTED to Emitter/VM: this file is purely additive, a first
// safe step of phase 1. Emitter keeps using std::string until that
// migration happens, which is a separate change reviewable on its own --
// from_declared()/base_name() are the bridge for when it does: they
// reproduce exactly what `TypeRef.name` produces today without needing to
// change Local::type yet.
//
// Decisions that follow the grammar to the letter (see
// LUX_SCRIPT-GRAMMAR.md):
//   - int/long are THE SAME TYPE (§7): a single Kind::Int. But the current
//     checker preserves the exact spelling in its error messages ("values
//     of type long don't have..."), so Type also preserves it (spelling_
//     field) so as not to change a single character of an existing
//     diagnostic once this gets connected.
//   - float/double are THE SAME TYPE (§7): a single Kind::Float, same note.
//   - A Dict's key is always string (§8): Dict only carries the value's
//     type, not a pair of types.
//   - Generics are erased at compile time (§8), but here they ARE
//     preserved: the checker that consumes this needs to know what's
//     inside a List<T>/Dict<string,V> to decide the native representation,
//     even though the bytecode VM erases them afterwards.

class Type {
public:
    // Unknown is today's "not known" (the empty string that type_of()
    // returns when there's nothing obvious to check: a for-loop variable
    // over a Json, an expression that's neither a literal nor a declared
    // identifier...). It's a real state, distinct from Void (a fn that
    // returns nothing IS a known type).
    enum class Kind { Unknown, Void, Int, Float, Bool, String, List, Dict, Class, Json };

    Kind kind() const { return kind_; }
    bool is_optional() const { return optional_; }
    bool is_unknown() const { return kind_ == Kind::Unknown; }

    // Only meaningful if kind() == List (the element type) or
    // kind() == Dict (the value type; the key is always string).
    const Type& element() const { return *elem_; }

    // Only meaningful if kind() == Class.
    const std::string& class_name() const { return class_name_; }

    static Type unknown() { return Type(Kind::Unknown); }
    static Type primitive(Kind k) { return Type(k); }
    static Type list_of(Type elem) { return Type(Kind::List, std::move(elem)); }
    static Type dict_of(Type value) { return Type(Kind::Dict, std::move(value)); }
    static Type class_ref(std::string name) { return Type(Kind::Class, std::move(name)); }
    static Type json() { return Type(Kind::Json); }
    static Type void_() { return Type(Kind::Void); }

    // From a TypeRef in the AST (ast.hpp), exactly as it's written in the
    // .lux source: "int x", "long x", "List<User> xs"... Preserves the
    // exact spelling ("long", "double") for the reason given above. See
    // parser.cpp: TypeRef::str() for the "declared type" notion this starts
    // from. Implemented in type.cpp (needs the full ast.hpp).
    static Type from_declared(const TypeRef& t);

    // From an already-resolved bare name (what TypedName::tipo carries
    // today: a class field, or the exact type of a constant Value). ""
    // gives unknown(). There are no generics or '?' to reconstruct because
    // TypedName never carried them.
    static Type from_legacy_name(const std::string& name);

    // The bare name as used today by Local::type/type_of: no `?`, no
    // generic arguments ("List<int>" gives "List", not "List<int>"). It's
    // the lookup key already expected by ClassSigs and metodos_de()
    // (natives.hpp) -- not to_string(), which is more descriptive but isn't
    // the key those tables use. "" if is_unknown().
    std::string base_name() const;

    // Equality deliberately ignores spelling_: `int` and `long` are the
    // SAME type (§7), and that's what should decide whether something
    // type-checks, not which word was written.
    bool operator==(const Type& other) const {
        if (kind_ != other.kind_ || optional_ != other.optional_) return false;
        if (kind_ == Kind::Class) return class_name_ == other.class_name_;
        if (kind_ == Kind::List || kind_ == Kind::Dict) return *elem_ == *other.elem_;
        return true;
    }

    // Full descriptive notation: "int", "List<string>", "Dict<string,Json>",
    // "MyClass", "int?"... For new messages (the IR, the native backend).
    // It is NOT the lookup key (see base_name()) nor does it necessarily
    // reproduce an already-existing error message.
    std::string to_string() const {
        std::string base = spelling_.empty() ? canonical_name() : spelling_;
        if (kind_ == Kind::List) base = "List<" + elem_->to_string() + ">";
        if (kind_ == Kind::Dict) base = "Dict<string," + elem_->to_string() + ">";
        return optional_ ? base + "?" : base;
    }

private:
    explicit Type(Kind k) : kind_(k) {}
    Type(Kind k, Type inner) : kind_(k), elem_(std::make_shared<Type>(std::move(inner))) {}
    Type(Kind k, std::string name) : kind_(k), class_name_(std::move(name)) {}

    std::string canonical_name() const {
        switch (kind_) {
            case Kind::Unknown: return "";
            case Kind::Void:    return "void";
            case Kind::Int:     return "int";
            case Kind::Float:   return "float";
            case Kind::Bool:    return "bool";
            case Kind::String:  return "string";
            case Kind::Json:    return "Json";
            case Kind::Class:   return class_name_;
            case Kind::List:    return "List";
            case Kind::Dict:    return "Dict";
        }
        return "";
    }

    Kind                  kind_;
    bool                  optional_ = false;
    std::shared_ptr<Type> elem_;        // List/Dict
    std::string           class_name_;  // Class
    std::string           spelling_;    // exact spelling from the .lux source, if it came from from_declared
};

} // namespace lux_script
