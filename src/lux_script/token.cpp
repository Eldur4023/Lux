#include <lux_script/token.hpp>
#include <unordered_map>
#include <utility>

namespace lux_script {

namespace {

// Every keyword once: keyword_or_ident() reads it one way, tok_name() the other.
constexpr std::pair<std::string_view, Tok> kKeywords[] = {
    {"import", Tok::KwImport}, {"class", Tok::KwClass}, {"fn", Tok::KwFn},
    {"app", Tok::KwApp}, {"group", Tok::KwGroup}, {"endpoint", Tok::KwEndpoint},
    {"on", Tok::KwOn}, {"error", Tok::KwError}, {"origins", Tok::KwOrigins},
    {"validate", Tok::KwValidate}, {"static", Tok::KwStatic}, {"spa", Tok::KwSpa},
    {"enum", Tok::KwEnum},

    {"get", Tok::KwGet}, {"post", Tok::KwPost}, {"put", Tok::KwPut},
    {"patch", Tok::KwPatch}, {"delete", Tok::KwDelete}, {"options", Tok::KwOptions},
    {"any", Tok::KwAny}, {"sse", Tok::KwSse}, {"ws", Tok::KwWs},

    {"if", Tok::KwIf}, {"else", Tok::KwElse}, {"elif", Tok::KwElif}, {"while", Tok::KwWhile},
    {"for", Tok::KwFor}, {"in", Tok::KwIn}, {"return", Tok::KwReturn},
    {"require", Tok::KwRequire}, {"try", Tok::KwTry}, {"catch", Tok::KwCatch},
    {"break", Tok::KwBreak}, {"continue", Tok::KwContinue},
    {"switch", Tok::KwSwitch}, {"case", Tok::KwCase},

    {"and", Tok::KwAnd}, {"or", Tok::KwOr}, {"not", Tok::KwNot},
    {"true", Tok::KwTrue}, {"false", Tok::KwFalse}, {"null", Tok::KwNull},
    {"this", Tok::KwThis}, {"await", Tok::KwAwait}, {"void", Tok::KwVoid},
};

} // namespace

const char* tok_name(Tok t) {
    switch (t) {
        case Tok::EndOfFile: return "end of file";
        case Tok::Newline:   return "end of line";
        case Tok::Indent:    return "indentation";
        case Tok::Dedent:    return "dedent";
        case Tok::Ident:     return "identifier";
        case Tok::Int:       return "integer";
        case Tok::Float:     return "float";
        case Tok::String:    return "string";

        case Tok::LParen:   return "(";
        case Tok::RParen:   return ")";
        case Tok::LBracket: return "[";
        case Tok::RBracket: return "]";
        case Tok::LBrace:   return "{";
        case Tok::RBrace:   return "}";
        case Tok::Comma:    return ",";
        case Tok::Colon:    return ":";
        case Tok::Dot:      return ".";
        case Tok::Question: return "?";
        case Tok::Arrow:    return "->";
        case Tok::Assign:   return "=";
        case Tok::Eq:       return "==";
        case Tok::NotEq:    return "!=";
        case Tok::Lt:       return "<";
        case Tok::LtEq:     return "<=";
        case Tok::Gt:       return ">";
        case Tok::GtEq:     return ">=";
        case Tok::Plus:     return "+";
        case Tok::Minus:    return "-";
        case Tok::Star:     return "*";
        case Tok::Slash:    return "/";
        case Tok::Percent:  return "%";
        case Tok::PlusPlus:   return "++";
        case Tok::MinusMinus: return "--";
        case Tok::PlusEq:     return "+=";
        case Tok::MinusEq:    return "-=";
        case Tok::StarEq:     return "*=";
        case Tok::SlashEq:    return "/=";
        case Tok::PercentEq:  return "%=";
        default: break;
    }
    for (const auto& [spelling, tok] : kKeywords)
        if (tok == t) return spelling.data();
    return "?";
}

Tok keyword_or_ident(std::string_view s) {
    static const std::unordered_map<std::string_view, Tok> kw(std::begin(kKeywords),
                                                                std::end(kKeywords));
    auto it = kw.find(s);
    return it == kw.end() ? Tok::Ident : it->second;
}

} // namespace lux_script
