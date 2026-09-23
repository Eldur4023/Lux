#include <lux_script/type.hpp>
#include <lux_script/ast.hpp>

namespace lux_script {

Type Type::from_declared(const TypeRef& t) {
    Type result = Type::unknown();

    if (t.name == "int" || t.name == "long") {
        result = Type::primitive(Kind::Int);
        result.spelling_ = t.name;
    } else if (t.name == "float" || t.name == "double") {
        result = Type::primitive(Kind::Float);
        result.spelling_ = t.name;
    } else if (t.name == "bool") {
        result = Type::primitive(Kind::Bool);
    } else if (t.name == "string") {
        result = Type::primitive(Kind::String);
    } else if (t.name == "void") {
        result = Type::void_();
    } else if (t.name == "Json") {
        result = Type::json();
    } else if (t.name == "List") {
        // No place uses the element today (Local::type only stores "List"),
        // but it's preserved for when phase 1's IR actually needs it --
        // Json as a placeholder for "not specified".
        Type elem = t.args.empty() ? Type::json() : Type::from_declared(t.args[0]);
        result = Type::list_of(std::move(elem));
    } else if (t.name == "Dict") {
        // args[0] is the key (always string, see §8 of the grammar: it's
        // not preserved, dict_of() only carries the value's type). args[1]
        // is the value.
        Type value = t.args.size() >= 2 ? Type::from_declared(t.args[1]) : Type::json();
        result = Type::dict_of(std::move(value));
    } else {
        result = Type::class_ref(t.name);
    }

    result.optional_ = t.optional;
    return result;
}

Type Type::from_legacy_name(const std::string& name) {
    if (name.empty()) return Type::unknown();
    TypeRef t;
    t.name = name;
    return from_declared(t);
}

std::string Type::base_name() const {
    return spelling_.empty() ? canonical_name() : spelling_;
}

} // namespace lux_script
