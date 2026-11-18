// Fórmulas de hoja de cálculo con dados, para reglas que define el usuario
// (Grimorio: sistemas de rol sin programar).
//
//   formula.check(expr)        -> {ok, error, names}        valida al guardar
//   formula.stats(expr, vars)  -> {min, max, avg, dice}     sin tirar
//   formula.roll(expr, vars)   -> {total, detail, min, max, avg}
//
// Lenguaje:  3   2.5   "texto"   + - * /   ( )   = <> < <= > >=
//            si(c; a; b)  min(..)  max(..)  abs(x)  redondear(x)  entero(x)
//            techo(x)  resto(a; b)  a ^ b  potencia(a; b)  raiz(x)  logaritmo(x; base)  ln(x)
//            redondear(x; decimales)    suma(..)  contar(lista)  sumar_si(valores; condiciones)
//            contar_si(condiciones)  contiene(lista o texto; "x")  uno_de(x; a; b; ..)
//            vacio(x)  unir(a; b; ..)  — alias en inglés: if, round, floor, ceil,
//            sum, count, sumif, countif, contains, oneof, empty, join.
//            Separador de argumentos , o ;
//            Un texto se une a otro (o a un número) con &:  "Nivel " & nivel
//            si() puede elegir entre textos, y unir() junta lo que le des (una
//            lista, separada por comas): el resultado de una fórmula puede ser
//            un texto, y si depende de una tirada (si(d100 <= habilidad; "Éxito"; "Fallo"))
//            stats() da las salidas posibles separadas por « | » y roll() una de ellas.
//            El resultado también puede ser una lista (suma, texto…): se
//            devuelve como `items` y como un texto con los elementos separados.
//
// Dados:     2d6   d20   d%   4dF (Fate)   (nivel)d6   2d(fuerza)   (a)d(b)
//            y modificadores, en cualquier orden, pegados al dado:
//              kh3 kl2        quedarse con los 3 más altos / 2 más bajos (kh = kh1)
//              dh1 dl1        descartar el más alto / más bajo
//              !  !>=5        el dado que da el máximo (o cumple la condición) suma otro
//              r<=2  ro<=2    repetir mientras cumpla / repetir una sola vez
//              s>=8           contar éxitos (cada dado que cumple vale 1) en vez de sumar
//              f=1            cada dado que cumple resta 1 a los éxitos
//            4d6kh3   (fuerza)d10s>=8f=1   2d20kl1   d6!
//
// Nombres: `mana_base`, `[Maná base]`, `clase.mana_base`. Se comparan
// normalizados (minúsculas, sin acentos, lo demás a "_") contra las claves de
// `vars`, así que "Maná base" y "mana_base" son el mismo. Un camino que pasa
// por una lista da una lista: `suma(partes_del_cuerpo.leves)`; `*` recorre
// todos los valores de un diccionario: `contar_si(heridas.*.letal >= 1)`. Las operaciones
// y comparaciones con listas van elemento a elemento, así una progresión es
// una lista con una columna de nivel:
//   sumar_si(clase.progresion.mana; clase.progresion.nivel <= nivel)
//
// En `vars`: un número es un número; null y false son 0; true es 1; un texto
// que se lee como fórmula sin variables ("1d6", "3") es ese número o dado, así
// un campo de dado se usa como cualquier otro valor; cualquier otro texto
// ("Grande", el nombre de una referencia) es un texto: se compara con `=` y
// `<>` sin distinguir mayúsculas ni acentos, y con contiene() / uno_de(), pero
// no se suma. Una referencia sola ("raza") vale su nombre. {"$formula": expr, "$vars": ámbito} es otra
// fórmula, evaluada con sus propias variables: así una fórmula usa otra sin
// perder sus dados (se tiran de verdad y salen en el desglose).
//
// `stats` acota cada subexpresión con un intervalo [min, max] y su media.
// Con sumas y restas de NdM la media es exacta; con modificadores de dado,
// si(), min(), max() o productos de dados se calcula simulando la fórmula
// 3000 veces con una semilla fija (siempre el mismo resultado; por eso
// Grimorio la muestra como "≈"). Un dado que explota no tiene máximo: el
// intervalo cuenta hasta 3 explosiones por dado.
#include <lux_script/builtin_module.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux_script {

namespace {

constexpr int    kMaxDepth = 64;
constexpr size_t kMaxLen   = 2000;
constexpr long   kMaxDice  = 1000;
constexpr long   kMaxSides = 10000;

std::mt19937_64& rng() {
    thread_local std::mt19937_64 r{std::random_device{}()};
    return r;
}

// ── Nombres ──────────────────────────────────────────────────────────────────

// Latin-1 0xC0..0xFF sin acento; '_' para lo que no es letra (× ÷).
constexpr const char* kLatin1 = "aaaaaaaceeeeiiiidnooooo_ouuuuytsaaaaaaaceeeeiiiidnooooo_ouuuuyty";

std::string normalize(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char o = '_';
        if (std::isalnum(c)) o = static_cast<char>(std::tolower(c));
        else if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char n = static_cast<unsigned char>(s[i + 1]);
            if (n >= 0x80 && n <= 0xBF) { o = kLatin1[n - 0x80]; ++i; }
        } else if (c >= 0x80) {
            while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i;
        }
        if (o == '_' && (out.empty() || out.back() == '_')) continue;
        out += o;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// ── Árbol ────────────────────────────────────────────────────────────────────

struct DiceMods {
    long        kh = -1, kl = -1, dh = 0, dl = 0;
    bool        explode = false, ex_default = true;
    std::string ex_op;  long ex_n = 0;
    bool        reroll = false, rr_once = false;
    std::string rr_op;  long rr_n = 0;
    bool        succ = false;
    std::string s_op;   long s_n = 0;
    bool        fail = false;
    std::string f_op;   long f_n = 0;
    std::string text;                      // tal cual se escribió: "kh3!"
    bool any() const { return !text.empty(); }
};

struct Node {
    enum Kind { Num, Dice, Var, Neg, Bin, Call, Text } kind = Num;
    double                   num = 0;
    bool                     fudge = false;   // Dice: kids = {cantidad, caras}
    DiceMods                 dm;
    std::string              text;            // Text
    std::vector<std::string> path;     // Var: segmentos normalizados
    std::string              op;       // Bin: "+", "<=", …   Call: nombre canónico
    std::vector<Node>        kids;
};

struct FnInfo { const char* canon; int min_args; int max_args; };

const FnInfo* function_info(const std::string& name) {
    static const std::vector<std::pair<std::string, FnInfo>> fns = {
        {"si",        {"si", 3, 3}},        {"if",      {"si", 3, 3}},
        {"min",       {"min", 1, -1}},      {"max",     {"max", 1, -1}},
        {"abs",       {"abs", 1, 1}},
        {"redondear", {"redondear", 1, 2}}, {"round",   {"redondear", 1, 2}},
        {"ln",        {"ln", 1, 1}},
        {"entero",    {"entero", 1, 1}},    {"floor",   {"entero", 1, 1}},
        {"techo",     {"techo", 1, 1}},     {"ceil",    {"techo", 1, 1}},
        {"resto",     {"resto", 2, 2}},
        {"potencia",  {"potencia", 2, 2}},  {"pow",     {"potencia", 2, 2}},
        {"raiz",      {"raiz", 1, 1}},      {"sqrt",    {"raiz", 1, 1}},
        {"logaritmo", {"logaritmo", 1, 2}}, {"log",     {"logaritmo", 1, 2}},
        {"contiene",  {"contiene", 2, 2}},  {"contains", {"contiene", 2, 2}},
        {"uno_de",    {"uno_de", 2, -1}},   {"oneof",   {"uno_de", 2, -1}},
        {"vacio",     {"vacio", 1, 1}},     {"empty",   {"vacio", 1, 1}},
        {"unir",      {"unir", 1, -1}},     {"join",    {"unir", 1, -1}},
        {"suma",      {"suma", 1, -1}},     {"sum",     {"suma", 1, -1}},
        {"contar",    {"contar", 1, 1}},    {"count",   {"contar", 1, 1}},
        {"sumar_si",  {"sumar_si", 2, 2}},  {"sumif",   {"sumar_si", 2, 2}},
        {"contar_si", {"contar_si", 1, 1}}, {"countif", {"contar_si", 1, 1}},
    };
    for (const auto& [n, info] : fns) if (n == name) return &info;
    return nullptr;
}

// ── Análisis ─────────────────────────────────────────────────────────────────

class Parser {
public:
    explicit Parser(std::string_view src) : s_(src) {}

    bool parse(Node& out, std::string& error) {
        if (s_.size() > kMaxLen) { error = "la fórmula es demasiado larga"; return false; }
        out = expr(0);
        skip();
        if (err_.empty() && i_ < s_.size()) fail("sobra algo a partir de aquí");
        error = err_;
        return err_.empty();
    }

private:
    std::string_view s_;
    size_t           i_ = 0;
    std::string      err_;

    void fail(const std::string& msg) {
        if (err_.empty()) err_ = msg + " (posición " + std::to_string(i_ + 1) + ")";
    }
    void skip() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    bool eat(std::string_view tok) {
        skip();
        if (s_.substr(i_, tok.size()) != tok) return false;
        i_ += tok.size();
        return true;
    }
    static bool ident_char(char c) {
        unsigned char u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '_' || u >= 0x80;
    }

    Node expr(int depth) {
        if (depth > kMaxDepth) { fail("la fórmula está demasiado anidada"); return {}; }
        Node left = concat(depth);
        for (const char* op : {"<>", "!=", "<=", ">=", "=", "<", ">"}) {
            if (eat(op)) {
                Node n; n.kind = Node::Bin; n.op = std::string(op) == "!=" ? "<>" : op;
                n.kids.push_back(std::move(left));
                n.kids.push_back(concat(depth));
                return n;
            }
        }
        return left;
    }
    Node concat(int depth) {
        Node left = additive(depth);
        while (err_.empty() && eat("&")) {
            Node n; n.kind = Node::Bin; n.op = "&";
            n.kids.push_back(std::move(left));
            n.kids.push_back(additive(depth));
            left = std::move(n);
        }
        return left;
    }
    Node additive(int depth) {
        Node left = term(depth);
        while (err_.empty()) {
            std::string op = eat("+") ? "+" : eat("-") ? "-" : "";
            if (op.empty()) break;
            Node n; n.kind = Node::Bin; n.op = op;
            n.kids.push_back(std::move(left));
            n.kids.push_back(term(depth));
            left = std::move(n);
        }
        return left;
    }
    Node term(int depth) {
        Node left = unary(depth);
        while (err_.empty()) {
            std::string op = eat("*") ? "*" : eat("/") ? "/" : "";
            if (op.empty()) break;
            Node n; n.kind = Node::Bin; n.op = op;
            n.kids.push_back(std::move(left));
            n.kids.push_back(unary(depth));
            left = std::move(n);
        }
        return left;
    }
    Node unary(int depth) {
        if (eat("-")) {
            Node n; n.kind = Node::Neg;
            n.kids.push_back(unary(depth + 1));
            return n;
        }
        return power(depth);
    }
    // a ^ b (asocia a la derecha y liga más que el producto, como en una hoja de cálculo)
    Node power(int depth) {
        Node base = atom(depth);
        if (err_.empty() && eat("^")) {
            Node n; n.kind = Node::Bin; n.op = "^";
            n.kids.push_back(std::move(base));
            n.kids.push_back(unary(depth + 1));
            return n;
        }
        return base;
    }

    static Node num_node(double x) { Node n; n.kind = Node::Num; n.num = x; return n; }

    bool digit_at(size_t at) const { return at < s_.size() && std::isdigit(static_cast<unsigned char>(s_[at])); }

    long read_long() {
        size_t j = i_;
        while (digit_at(i_)) ++i_;
        return std::stol(std::string(s_.substr(j, std::min<size_t>(i_ - j, 9))));
    }

    // Un comparador pegado al modificador: >=  <=  >  <  =
    bool read_cmp(std::string& op) {
        for (const char* c : {">=", "<=", ">", "<", "="}) {
            if (s_.substr(i_, std::string_view(c).size()) == c) { op = c; i_ += op.size(); return true; }
        }
        return false;
    }

    // kh3 kl dh1 dl ! !>=5 r<=2 ro<=2 s>=8 f=1, pegados al dado.
    bool dice_mods(DiceMods& m) {
        const size_t start = i_;
        while (i_ < s_.size()) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s_[i_])));
            const char d = i_ + 1 < s_.size() ? static_cast<char>(std::tolower(static_cast<unsigned char>(s_[i_ + 1]))) : '\0';
            if ((c == 'k' || c == 'd') && (d == 'h' || d == 'l')) {
                i_ += 2;
                long n = digit_at(i_) ? read_long() : 1;
                if (c == 'k') (d == 'h' ? m.kh : m.kl) = n;
                else (d == 'h' ? m.dh : m.dl) += n;
            } else if (c == '!') {
                ++i_;
                m.explode = true;
                m.ex_default = !read_cmp(m.ex_op);
                if (!m.ex_default) {
                    if (!digit_at(i_)) { fail("falta el número después de «!" + m.ex_op + "»"); return false; }
                    m.ex_n = read_long();
                }
            } else if (c == 'r') {
                ++i_;
                m.rr_once = (i_ < s_.size() && (s_[i_] == 'o' || s_[i_] == 'O'));
                if (m.rr_once) ++i_;
                if (!read_cmp(m.rr_op) || !digit_at(i_)) { fail("después de «r» va una condición: r<=2"); return false; }
                m.rr_n = read_long();
                m.reroll = true;
            } else if (c == 's' || c == 'f') {
                ++i_;
                std::string op;
                if (!read_cmp(op) || !digit_at(i_)) { fail(std::string("después de «") + c + "» va una condición: " + c + ">=8"); return false; }
                long n = read_long();
                if (c == 's') { m.succ = true; m.s_op = op; m.s_n = n; }
                else          { m.fail = true; m.f_op = op; m.f_n = n; }
            } else break;
        }
        m.text = std::string(s_.substr(start, i_ - start));
        return true;
    }

    // Lo que sigue a la cantidad de un dado: dN, d%, dF, d(expr), y sus modificadores.
    // `explicit_count`: había un número o un grupo delante (así «dF» o «d(» no se confunden con nombres).
    bool dice_tail(Node count, bool explicit_count, size_t start, Node& out) {
        if (i_ >= s_.size() || (s_[i_] != 'd' && s_[i_] != 'D')) return false;
        const size_t save = i_;
        ++i_;
        Node sides;
        bool fudge = false;
        if (digit_at(i_)) sides = num_node(static_cast<double>(read_long()));
        else if (i_ < s_.size() && s_[i_] == '%') { ++i_; sides = num_node(100); }
        else if (explicit_count && i_ < s_.size() && (s_[i_] == 'F' || s_[i_] == 'f')) { ++i_; fudge = true; sides = num_node(1); }
        else if (explicit_count && i_ < s_.size() && s_[i_] == '(') {
            ++i_;
            sides = expr(1);
            if (!eat(")")) { fail("falta cerrar el paréntesis de los dados"); return true; }
        } else { i_ = save; return false; }
        DiceMods m;
        if (!dice_mods(m)) return true;
        if (i_ < s_.size() && ident_char(s_[i_])) { i_ = save; return false; }   // «d20mod»: un nombre
        if (count.kind == Node::Num && (count.num < 0 || count.num > kMaxDice)) { fail("entre 0 y " + std::to_string(kMaxDice) + " dados por tirada"); return true; }
        if (sides.kind == Node::Num && !fudge && (sides.num < 1 || sides.num > kMaxSides)) { fail("un dado tiene entre 1 y " + std::to_string(kMaxSides) + " caras"); return true; }
        out = Node{};
        out.kind = Node::Dice; out.fudge = fudge; out.dm = m;
        out.text = std::string(s_.substr(start, i_ - start));      // «(fuerza)d10s>=8», para el desglose
        out.kids.push_back(std::move(count));
        out.kids.push_back(std::move(sides));
        return true;
    }

    std::string segment() {
        skip();
        if (i_ < s_.size() && s_[i_] == '[') {
            size_t end = s_.find(']', i_);
            if (end == std::string_view::npos) { fail("falta cerrar el ']'"); return ""; }
            std::string seg = normalize(s_.substr(i_ + 1, end - i_ - 1));
            i_ = end + 1;
            if (seg.empty()) fail("nombre vacío entre corchetes");
            return seg;
        }
        size_t start = i_;
        while (i_ < s_.size() && ident_char(s_[i_])) ++i_;
        return normalize(s_.substr(start, i_ - start));
    }

    Node atom(int depth) {
        skip();
        if (i_ >= s_.size()) { fail("falta un valor al final"); return {}; }
        const char c = s_[i_];
        Node n;
        if (c == '"' || c == '\'') {
            size_t end = s_.find(c, i_ + 1);
            if (end == std::string_view::npos) { fail("falta cerrar las comillas"); return n; }
            n.kind = Node::Text;
            n.text = std::string(s_.substr(i_ + 1, end - i_ - 1));
            i_ = end + 1;
            return n;
        }
        const size_t group_start = i_;
        if (eat("(")) {
            n = expr(depth + 1);
            if (!eat(")")) { fail("falta cerrar el paréntesis"); return n; }
            Node dice;
            if (err_.empty() && dice_tail(n, true, group_start, dice)) return dice;       // (nivel)d6
            return n;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
            size_t start = i_;
            while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.')) ++i_;
            std::string lit(s_.substr(start, i_ - start));
            if (lit.find('.') == std::string::npos && dice_tail(num_node(static_cast<double>(std::stol(lit.substr(0, 9)))), true, start, n)) return n;
            try { n.num = std::stod(lit); } catch (...) { fail("número mal escrito"); }
            return n;
        }
        if (dice_tail(num_node(1), false, i_, n)) return n;
        if (c == '[' || ident_char(c)) {
            std::vector<std::string> path{segment()};
            skip();
            if (path[0].size() > 0 && i_ < s_.size() && s_[i_] == '(' && c != '[') {
                const FnInfo* info = function_info(path[0]);
                if (!info) { fail("no existe la función «" + path[0] + "»"); return n; }
                ++i_;
                n.kind = Node::Call; n.op = info->canon;
                if (!eat(")")) {
                    do { n.kids.push_back(expr(depth + 1)); } while (err_.empty() && (eat(",") || eat(";")));
                    if (!eat(")")) fail("falta cerrar el paréntesis de " + n.op + "()");
                }
                const int argc = static_cast<int>(n.kids.size());
                if (argc < info->min_args || (info->max_args >= 0 && argc > info->max_args))
                    fail(n.op + "() no admite " + std::to_string(argc) + " argumento(s)");
                return n;
            }
            while (err_.empty() && i_ < s_.size() && s_[i_] == '.') {
                ++i_;
                if (i_ < s_.size() && s_[i_] == '*') { ++i_; path.push_back("*"); }   // todos los valores
                else path.push_back(segment());
            }
            for (const auto& seg : path) if (seg.empty()) fail("nombre mal escrito");
            n.kind = Node::Var; n.path = std::move(path);
            return n;
        }
        fail(std::string("no esperaba '") + c + "'");
        return n;
    }
};

void collect_names(const Node& n, std::vector<std::string>& out) {
    if (n.kind == Node::Var) {
        std::string joined;
        for (const auto& seg : n.path) joined += (joined.empty() ? "" : ".") + seg;
        if (std::find(out.begin(), out.end(), joined) == out.end()) out.push_back(joined);
    }
    for (const auto& k : n.kids) collect_names(k, out);
}

// ── Evaluación ───────────────────────────────────────────────────────────────

struct R {
    double lo = 0, hi = 0, avg = 0, val = 0;
    bool   dice = false;
    bool   is_list = false;
    bool   text = false;       // vino de un texto
    bool   bad_num = false;    // …y no es un número: solo se compara
    std::string txt;
    std::vector<std::string> alts;   // un texto que depende de una tirada: lo que puede salir
    std::vector<R> items;

    static R num(double x) { R r; r.lo = r.hi = r.avg = r.val = x; return r; }
};

bool cmp_long(const std::string& op, long a, long b) {
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == ">")  return a > b;
    if (op == "<")  return a < b;
    return a == b;
}

class Evaluator {
public:
    Evaluator(const Value* vars, bool roll) : vars_(vars), roll_(roll) {}

    std::string              error;
    std::vector<std::string> rolls;   // "2d6 → 3 + 5"
    bool                     approx = false;   // alguna media de las calculadas no es exacta
    bool                     quiet = false;    // sin desglose (al simular)
    std::mt19937_64*         gen = nullptr;    // por defecto, el del hilo

    R eval(const Node& n, bool record = true) {
        if (!error.empty()) return {};
        // Una variable puede ser otra fórmula que se evalúa en cada referencia: sin tope, a+a+a… anidado explota exponencialmente.
        if (++steps_ > kMaxSteps) { error = "la fórmula es demasiado costosa"; return {}; }
        switch (n.kind) {
            case Node::Num:  return R::num(n.num);
            case Node::Text: return text_value(n.text, "texto");
            case Node::Dice: return dice(n, record);
            case Node::Var:  return var(n);
            case Node::Neg:  return combine("*", R::num(-1), eval(n.kids[0], record));
            case Node::Bin:  return binary(n, record);
            case Node::Call: return call(n, record);
        }
        return {};
    }

private:
    const Value* vars_;
    bool         roll_;
    int          nested_ = 0;
    long         steps_  = 0;
    static constexpr long kMaxSteps = 1'000'000;
    std::unordered_map<std::string, std::pair<bool, Node>> parsed_;

    std::mt19937_64& rnd() { return gen ? *gen : rng(); }

    const std::pair<bool, Node>& parsed(const std::string& src, std::string& perr) {
        auto it = parsed_.find(src);
        if (it == parsed_.end()) {
            Node sub;
            bool ok = Parser(src).parse(sub, perr);
            it = parsed_.emplace(src, std::make_pair(ok, std::move(sub))).first;
            if (!ok) parse_errors_[src] = perr;
        } else if (!it->second.first) perr = parse_errors_[src];
        return it->second;
    }
    std::unordered_map<std::string, std::string> parse_errors_;

    R scalar(R r, const std::string& where) {
        if (r.is_list && error.empty())
            error = "una lista solo se puede usar dentro de suma(), min(), max() o contar() (en «" + where + "»)";
        return numeric(r, where);
    }

    R single(R r, const std::string& where) {
        if (r.is_list && error.empty()) error = "«" + where + "» espera un solo valor, no una lista";
        return r;
    }

    // Lo que no es un número no entra en una operación.
    R numeric(R r, const std::string& where) {
        if (r.bad_num && error.empty())
            error = "se esperaba un número y hay un texto («" + r.txt + "») en «" + where + "»";
        return r;
    }

    static bool has_var(const Node& n) {
        if (n.kind == Node::Var) return true;
        for (const auto& k : n.kids) if (has_var(k)) return true;
        return false;
    }

    // Un texto es un número o dado si se lee como fórmula sin variables; si no, es un texto.
    R text_value(const std::string& str, const std::string& name) {
        R t;
        t.text = true;
        t.txt = str;
        if (str.empty()) return t;
        std::string perr;
        const auto& p = parsed(str, perr);
        if (!p.first || has_var(p.second) || p.second.kind == Node::Text) { t.bad_num = true; return t; }
        if (nested_ > 4) { error = "«" + name + "» se refiere a sí mismo"; return {}; }
        ++nested_;
        R r = eval(p.second);
        --nested_;
        r.text = true;
        r.txt = str;
        return r;
    }

public:
    static std::string number_text(double x) {
        if (std::floor(x) == x && std::fabs(x) < 9e15) return std::to_string(static_cast<long long>(x));
        return std::to_string(x);
    }
private:
    static std::string text_of(const R& r) { return r.text ? r.txt : number_text(r.val); }

    static bool equal_values(const R& a, const R& b) {
        if (!a.bad_num && !b.bad_num) return a.val == b.val;
        return normalize(text_of(a)) == normalize(text_of(b));
    }

    // ── Dados ────────────────────────────────────────────────────────────────

    struct Pool { std::vector<long> vals; std::vector<bool> dropped; long total = 0; };

    long die(long sides, bool fudge) {
        std::uniform_int_distribution<long> d(fudge ? -1 : 1, fudge ? 1 : sides);
        return d(rnd());
    }

    Pool roll_pool(long count, long sides, bool fudge, const DiceMods& m) {
        Pool p;
        const long top = fudge ? 1 : sides;
        int budget = 100;      // dados extra por explosiones en toda la tirada
        for (long i = 0; i < count; ++i) {
            long v = die(sides, fudge);
            if (m.reroll) for (int t = 0; t < (m.rr_once ? 1 : 50) && cmp_long(m.rr_op, v, m.rr_n); ++t) v = die(sides, fudge);
            p.vals.push_back(v);
            if (m.explode) {
                long cur = v;
                while (budget > 0 && (m.ex_default ? cur == top : cmp_long(m.ex_op, cur, m.ex_n))) {
                    cur = die(sides, fudge);
                    p.vals.push_back(cur);
                    --budget;
                }
            }
        }
        p.dropped.assign(p.vals.size(), false);
        // Quedarse con / descartar, sobre los dados ordenados de menor a mayor.
        std::vector<size_t> idx(p.vals.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return p.vals[x] < p.vals[y]; });
        auto drop_front = [&](long n) {          // los más bajos que sigan vivos
            for (size_t k = 0; k < idx.size() && n > 0; ++k) if (!p.dropped[idx[k]]) { p.dropped[idx[k]] = true; --n; }
        };
        auto drop_back = [&](long n) {
            for (size_t k = idx.size(); k-- > 0 && n > 0;) if (!p.dropped[idx[k]]) { p.dropped[idx[k]] = true; --n; }
        };
        auto alive = [&]() { long c = 0; for (size_t i = 0; i < p.vals.size(); ++i) if (!p.dropped[i]) ++c; return c; };
        if (m.kh >= 0) drop_front(std::max(0L, alive() - m.kh));
        if (m.kl >= 0) drop_back(std::max(0L, alive() - m.kl));
        drop_front(m.dl);
        drop_back(m.dh);
        for (size_t i = 0; i < p.vals.size(); ++i) {
            if (p.dropped[i]) continue;
            if (m.succ) { if (cmp_long(m.s_op, p.vals[i], m.s_n)) ++p.total; if (m.fail && cmp_long(m.f_op, p.vals[i], m.f_n)) --p.total; }
            else if (m.fail) { if (cmp_long(m.f_op, p.vals[i], m.f_n)) --p.total; }
            else p.total += p.vals[i];
        }
        return p;
    }

    R dice(const Node& n, bool record) {
        const Node& cn = n.kids[0];
        const Node& sn = n.kids[1];
        const DiceMods& m = n.dm;
        R c = scalar(eval(cn, record), "la cantidad de dados");
        R sd = scalar(eval(sn, record), "las caras del dado");
        if (!error.empty()) return {};
        const bool plain = c.lo == c.hi && sd.lo == sd.hi && !m.any();     // NdM con N y M conocidos: media exacta
        const long clo = std::max(0L, std::lround(c.lo)), chi = std::lround(c.hi);
        const long slo = std::lround(sd.lo), shi = std::lround(sd.hi);
        if (chi > kMaxDice || std::lround(c.val) > kMaxDice) { error = "entre 0 y " + std::to_string(kMaxDice) + " dados por tirada"; return {}; }
        if (!n.fudge && (slo < 1 || shi > kMaxSides)) { error = "un dado tiene entre 1 y " + std::to_string(kMaxSides) + " caras"; return {}; }
        R r;
        r.dice = true;
        // Cuántos cuentan, con cada extremo de la cantidad.
        auto kept = [&](long k) {
            if (m.kh >= 0) k = std::min(k, m.kh);
            if (m.kl >= 0) k = std::min(k, m.kl);
            return std::max(0L, k - m.dl - m.dh);
        };
        const long minface = n.fudge ? -1 : 1;
        const long maxface = n.fudge ? 1 : shi;
        const long boom = m.explode ? 4 : 1;
        if (m.succ || m.fail) {
            r.lo = m.fail ? -static_cast<double>(kept(clo) ? kept(chi) : 0) * boom : 0;
            r.hi = m.succ ? static_cast<double>(kept(chi)) * boom : 0;
        } else {
            r.lo = static_cast<double>(kept(clo) * minface);
            r.hi = static_cast<double>(kept(chi) * maxface * boom);
        }
        r.avg = n.fudge ? 0 : (static_cast<double>(clo + chi) / 2.0) * (static_cast<double>(slo + shi) / 2.0 + 1) / 2.0;
        if (!plain) approx = true;
        if (!roll_) { r.val = r.avg; return r; }
        const long count = std::max(0L, std::lround(c.val));
        const long sides = std::lround(sd.val);
        if (!n.fudge && sides < 1) { error = "un dado tiene entre 1 y " + std::to_string(kMaxSides) + " caras"; return {}; }
        Pool p = roll_pool(count, sides, n.fudge, m);
        r.val = static_cast<double>(p.total);
        if (record && !quiet) {
            std::string detail = n.text + " →";
            for (size_t i = 0; i < p.vals.size() && i < 24; ++i) {
                detail += " " + (p.dropped[i] ? "(" + std::to_string(p.vals[i]) + ")" : std::to_string(p.vals[i]));
            }
            if (p.vals.size() > 24) detail += " …";
            if (m.succ || m.fail) detail += " = " + std::to_string(p.total) + (p.total == 1 || p.total == -1 ? " éxito" : " éxitos");
            else if (p.vals.size() > 24 || m.any()) detail += " = " + std::to_string(p.total);
            rolls.push_back(detail);
        }
        return r;
    }

    // Busca un segmento normalizado entre las claves de un Dict.
    static const Value* member(const Value& d, const std::string& seg) {
        for (const auto& [k, v] : d.as_dict()) if (normalize(k) == seg) return &v;
        return nullptr;
    }

    R from_value(const Value& v, const std::string& name) {
        if (v.is_null()) return R::num(0);
        if (v.is_bool()) return R::num(v.as_bool() ? 1 : 0);
        if (v.is_num())  return R::num(v.as_float());
        if (v.is_list()) {
            R r; r.is_list = true;
            for (const Value& x : v.as_list()) {
                R item = from_value(x, name);
                r.dice = r.dice || item.dice;
                r.items.push_back(std::move(item));
            }
            return r;
        }
        if (v.is_str()) return text_value(v.as_str(), name);
        if (v.is_dict()) {
            auto f = v.as_dict().find("$formula");
            if (f != v.as_dict().end() && f->second.is_str()) {
                if (nested_ > 8) { error = "«" + name + "»: demasiadas fórmulas encadenadas"; return {}; }
                std::string perr;
                const auto& p = parsed(f->second.as_str(), perr);
                if (!p.first) { error = "«" + name + "»: " + perr; return {}; }
                auto scope = v.as_dict().find("$vars");
                const Value* saved = vars_;
                if (scope != v.as_dict().end()) vars_ = &scope->second;
                ++nested_;
                R r = eval(p.second);
                --nested_;
                vars_ = saved;
                if (!error.empty() && error.find("«" + name + "»") == std::string::npos)
                    error = "«" + name + "»: " + error;
                return r;
            }
            auto nm = v.as_dict().find("nombre");       // una referencia sola vale su nombre
            if (nm != v.as_dict().end() && nm->second.is_str()) {
                R t; t.text = true; t.bad_num = true; t.txt = nm->second.as_str();
                return t;
            }
        }
        error = "«" + name + "» no es un valor: ¿falta indicar un campo? (p. ej. " + name + ".nombre_del_campo)";
        return {};
    }

    static std::string path_name(const Node& n) {
        std::string name;
        for (const auto& seg : n.path) name += (name.empty() ? "" : ".") + seg;
        return name;
    }

    // Recorre el camino de un nombre; al cruzar una lista sigue por cada elemento.
    bool resolve(const Node& n, std::vector<Value>& cur, bool& through_list) {
        const std::string name = path_name(n);
        if (!vars_ || !vars_->is_dict()) { error = "no existe «" + name + "»"; return false; }
        cur = {*vars_};
        through_list = false;
        for (const auto& seg : n.path) {
            std::vector<Value> next;
            if (seg == "*") {                     // cada valor de un Dict o elemento de una lista
                through_list = true;
                for (const Value& c : cur) {
                    if (c.is_dict()) for (const auto& [k, v] : c.as_dict()) next.push_back(v);
                    else if (c.is_list()) for (const Value& v : c.as_list()) next.push_back(v);
                    else { error = "«" + name + "»: «*» necesita algo con varios valores"; return false; }
                }
                cur = std::move(next);
                continue;
            }
            for (const Value& c : cur) {
                if (c.is_list()) {
                    through_list = true;
                    bool dicts = false, hit = false;
                    for (const Value& item : c.as_list())
                        if (item.is_dict()) {
                            dicts = true;
                            if (const Value* m = member(item, seg)) { next.push_back(*m); hit = true; }
                        }
                    if (dicts && !hit) { error = "no existe «" + name + "»"; return false; }   // ninguna fila lo tiene
                } else if (c.is_dict()) {
                    const Value* m = member(c, seg);
                    if (!m) { error = "no existe «" + name + "»"; return false; }
                    next.push_back(*m);
                } else {
                    error = "«" + name + "»: no se puede entrar en un valor simple";
                    return false;
                }
            }
            cur = std::move(next);
        }
        return true;
    }

    R var(const Node& n) {
        std::vector<Value> cur;
        bool through_list = false;
        if (!resolve(n, cur, through_list)) return {};
        const std::string name = path_name(n);
        if (!through_list) return cur.empty() ? R::num(0) : from_value(cur[0], name);
        R r; r.is_list = true;
        for (const Value& v : cur) {
            R item = from_value(v, name);
            if (!error.empty()) return {};
            r.dice = r.dice || item.dice;
            r.items.push_back(std::move(item));
        }
        return r;
    }

    // contar() no necesita que los elementos sean números: cuenta lo que hay.
    R count(const Node& k, bool record) {
        if (k.kind == Node::Var) {
            std::vector<Value> cur;
            bool through_list = false;
            if (!resolve(k, cur, through_list)) return {};
            if (through_list) return R::num(static_cast<double>(cur.size()));
            if (cur.size() == 1 && cur[0].is_list()) return R::num(static_cast<double>(cur[0].as_list().size()));
        }
        R x = eval(k, record);
        return R::num(x.is_list ? static_cast<double>(x.items.size()) : 1);
    }

    static bool cmp(const std::string& op, double a, double b) {
        if (op == "=")  return a == b;
        if (op == "<>") return a != b;
        if (op == "<")  return a < b;
        if (op == "<=") return a <= b;
        if (op == ">")  return a > b;
        return a >= b;
    }

    R binary(const Node& n, bool record) {
        R a = eval(n.kids[0], record);
        R b = eval(n.kids[1], record);
        if (!error.empty()) return {};
        return combine(n.op, a, b);
    }

    // Con una lista, elemento a elemento (un valor suelto se repite para cada uno).
    R combine(const std::string& op, const R& a, const R& b) {
        if (a.is_list || b.is_list) {
            if (a.is_list && b.is_list && a.items.size() != b.items.size()) {
                error = "«" + op + "» entre listas de distinto tamaño (" + std::to_string(a.items.size()) +
                        " y " + std::to_string(b.items.size()) + ")";
                return {};
            }
            R out;
            out.is_list = true;
            const size_t len = a.is_list ? a.items.size() : b.items.size();
            for (size_t i = 0; i < len && error.empty(); ++i) {
                out.items.push_back(combine(op, a.is_list ? a.items[i] : a, b.is_list ? b.items[i] : b));
                out.dice = out.dice || out.items.back().dice;
            }
            return out;
        }
        R r;
        r.dice = a.dice || b.dice;
        if (op == "&") {                                   // juntar textos (o números) en un texto
            r.text = true; r.bad_num = true;
            r.txt = text_of(a) + text_of(b);
            if (!a.alts.empty() || !b.alts.empty()) {          // cada salida con cada salida (hasta 24)
                const std::vector<std::string> xs = a.alts.empty() ? std::vector<std::string>{text_of(a)} : a.alts;
                const std::vector<std::string> ys = b.alts.empty() ? std::vector<std::string>{text_of(b)} : b.alts;
                for (const auto& x : xs) for (const auto& y : ys) if (r.alts.size() < 24) r.alts.push_back(x + y);
                r.txt.clear();
                for (size_t i = 0; i < r.alts.size(); ++i) r.txt += (i ? " | " : "") + r.alts[i];
                r.dice = true;
            }
            return r;
        }
        if ((op == "=" || op == "<>") && (a.bad_num || b.bad_num)) {      // texto contra algo: se compara el texto
            if (!a.alts.empty() || !b.alts.empty()) {                     // un texto que depende de una tirada
                const std::vector<std::string> xs = a.alts.empty() ? std::vector<std::string>{text_of(a)} : a.alts;
                const std::vector<std::string> ys = b.alts.empty() ? std::vector<std::string>{text_of(b)} : b.alts;
                int yes = 0, total = 0;
                for (const auto& x : xs) for (const auto& y : ys) { ++total; if (normalize(x) == normalize(y)) ++yes; }
                if (op == "<>") yes = total - yes;
                r.dice = true; approx = true;
                r.lo = yes == total ? 1 : 0; r.hi = yes == 0 ? 0 : 1;
                r.avg = static_cast<double>(yes) / total;
                r.val = yes * 2 >= total ? 1 : 0;
                return r;
            }
            const bool same = equal_values(a, b);
            r.lo = r.hi = r.avg = r.val = (op == "=") == same ? 1 : 0;
            return r;
        }
        if (a.bad_num || b.bad_num) {
            error = "se esperaba un número y hay un texto («" + (a.bad_num ? a.txt : b.txt) + "»): con «" + op + "» no se pueden usar textos";
            return {};
        }
        if (op == "+") { r.lo = a.lo + b.lo; r.hi = a.hi + b.hi; r.avg = a.avg + b.avg; r.val = a.val + b.val; }
        else if (op == "-") { r.lo = a.lo - b.hi; r.hi = a.hi - b.lo; r.avg = a.avg - b.avg; r.val = a.val - b.val; }
        else if (op == "*" || op == "/") {
            if (op == "/" && b.lo == 0 && b.hi == 0) { error = "división entre cero"; return {}; }
            if (op == "/" && roll_ && b.val == 0) { error = "división entre cero"; return {}; }
            if ((op == "*" && a.dice && b.dice) || (op == "/" && b.dice)) approx = true;
            auto f = [&](double x, double y) { return op == "*" ? x * y : x / (y == 0 ? (y >= 0 ? 1e-9 : -1e-9) : y); };
            double c[4] = {f(a.lo, b.lo), f(a.lo, b.hi), f(a.hi, b.lo), f(a.hi, b.hi)};
            r.lo = *std::min_element(c, c + 4);
            r.hi = *std::max_element(c, c + 4);
            r.avg = op == "*" ? a.avg * b.avg : (b.avg == 0 ? 0 : a.avg / b.avg);
            r.val = op == "*" ? a.val * b.val : (b.val == 0 ? 0 : a.val / b.val);
        } else if (op == "^") {
            if ((a.lo < 0 && std::floor(b.val) != b.val) || a.val < 0 && std::floor(b.val) != b.val) {
                error = "no se puede elevar un número negativo a un exponente con decimales"; return {};
            }
            auto f = [](double x, double y) { return std::pow(x, y); };
            double c[4] = {f(a.lo, b.lo), f(a.lo, b.hi), f(a.hi, b.lo), f(a.hi, b.hi)};
            for (double x : c) if (!std::isfinite(x)) { error = "la potencia se sale de rango"; return {}; }
            r.lo = *std::min_element(c, c + 4);
            r.hi = *std::max_element(c, c + 4);
            if (a.lo < 0 && a.hi > 0) r.lo = std::min(r.lo, 0.0);
            r.avg = std::pow(a.avg, b.avg);
            r.val = std::pow(a.val, b.val);
            if (!std::isfinite(r.val) || !std::isfinite(r.avg)) { error = "la potencia se sale de rango"; return {}; }
            if (a.dice || b.dice) approx = true;
        } else {
            r.val = cmp(op, a.val, b.val) ? 1 : 0;
            bool decided_true = false, decided_false = false;
            if (op == "=" || op == "<>") {
                const bool constant = a.lo == a.hi && b.lo == b.hi;
                const bool disjoint = a.hi < b.lo || b.hi < a.lo;
                if (constant) (cmp(op, a.lo, b.lo) ? decided_true : decided_false) = true;
                else if (disjoint) (op == "=" ? decided_false : decided_true) = true;
            } else {
                int t = cmp(op, a.lo, b.hi) + cmp(op, a.hi, b.lo) + cmp(op, a.lo, b.lo) + cmp(op, a.hi, b.hi);
                decided_true = t == 4;
                decided_false = t == 0;
            }
            r.lo = decided_true ? 1 : 0;
            r.hi = decided_false ? 0 : 1;
            r.avg = decided_true ? 1 : decided_false ? 0 : 0.5;
            if (!decided_true && !decided_false && r.dice) approx = true;
        }
        return r;
    }

    void flatten(const R& r, std::vector<R>& out) {
        if (!r.is_list) { out.push_back(r); return; }
        for (const R& x : r.items) flatten(x, out);
    }

    R call(const Node& n, bool record) {
        const std::string& fn = n.op;
        if (fn == "si") {
            R c = scalar(eval(n.kids[0], record), "si");
            // Sin tirar los dados de la rama que no se elige.
            R a = single(eval(n.kids[1], false), "si");
            R b = single(eval(n.kids[2], false), "si");
            if (!error.empty()) return {};
            if (a.bad_num || b.bad_num) {                    // elige entre textos
                if (c.lo != c.hi && !roll_) {                // depende de una tirada: salen las dos
                    R r;
                    r.text = true; r.bad_num = true; r.dice = true;
                    for (const R* x : {&a, &b}) {
                        const std::vector<std::string> from = x->alts.empty() ? std::vector<std::string>{text_of(*x)} : x->alts;
                        for (const auto& t : from)
                            if (std::find(r.alts.begin(), r.alts.end(), t) == r.alts.end()) r.alts.push_back(t);
                    }
                    for (size_t i = 0; i < r.alts.size(); ++i) r.txt += (i ? " | " : "") + r.alts[i];
                    return r;
                }
                const bool yes = (roll_ ? c.val : c.lo) != 0;
                return eval(n.kids[yes ? 1 : 2], record);
            }
            numeric(a, "si"); numeric(b, "si");
            if (!error.empty()) return {};
            R r;
            r.dice = c.dice || a.dice || b.dice;
            if (c.lo == c.hi) { R chosen = c.lo != 0 ? a : b; r.lo = chosen.lo; r.hi = chosen.hi; r.avg = chosen.avg; }
            else { r.lo = std::min(a.lo, b.lo); r.hi = std::max(a.hi, b.hi); r.avg = c.avg * a.avg + (1 - c.avg) * b.avg; if (r.dice) approx = true; }
            r.val = roll_ ? eval(n.kids[c.val != 0 ? 1 : 2], record).val : r.avg;
            return r;
        }
        if (fn == "contar") return count(n.kids[0], record);
        if (fn == "unir") {
            std::string out;
            for (const auto& k : n.kids) {
                R x = eval(k, record);
                if (!error.empty()) return {};
                if (x.is_list) {
                    std::vector<R> items;
                    flatten(x, items);
                    for (size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + text_of(items[i]);
                } else out += text_of(x);
            }
            R r; r.text = true; r.bad_num = true; r.txt = out;
            return r;
        }
        if (fn == "contiene") {
            R x = eval(n.kids[0], record);
            R y = eval(n.kids[1], record);
            if (!error.empty()) return {};
            if (y.is_list) { error = "contiene(): lo que se busca ha de ser un solo valor"; return {}; }
            const std::string needle = normalize(text_of(y));
            bool found = false;
            if (x.is_list) {
                std::vector<R> items;
                flatten(x, items);
                for (const R& it : items) if (normalize(text_of(it)) == needle) found = true;
            } else {
                found = needle.empty() ? false : normalize(text_of(x)).find(needle) != std::string::npos;
            }
            return R::num(found ? 1 : 0);
        }
        if (fn == "uno_de") {
            R x = single(eval(n.kids[0], record), "uno_de");
            bool found = false;
            for (size_t i = 1; i < n.kids.size() && error.empty(); ++i) {
                R y = single(eval(n.kids[i], record), "uno_de");
                if (error.empty() && equal_values(x, y)) found = true;
            }
            if (!error.empty()) return {};
            return R::num(found ? 1 : 0);
        }
        if (fn == "vacio") {
            R x = eval(n.kids[0], record);
            if (!error.empty()) return {};
            if (x.is_list) return R::num(x.items.empty() ? 1 : 0);
            if (x.bad_num) return R::num(x.txt.empty() ? 1 : 0);
            return R::num(x.lo == 0 && x.hi == 0 ? 1 : 0);
        }
        if (fn == "sumar_si" || fn == "contar_si") {
            std::vector<R> conds, vals;
            flatten(eval(n.kids.back(), record), conds);
            if (fn == "sumar_si") flatten(eval(n.kids[0], record), vals);
            else vals.assign(conds.size(), R::num(1));
            if (!error.empty()) return {};
            if (vals.size() != conds.size()) {
                error = fn + "(): los valores y las condiciones tienen distinto tamaño (" +
                        std::to_string(vals.size()) + " y " + std::to_string(conds.size()) + ")";
                return {};
            }
            R r;
            for (size_t i = 0; i < vals.size(); ++i) {
                const R& c = conds[i];
                const R& v = vals[i];
                numeric(v, fn);
                r.dice = r.dice || c.dice || v.dice;
                if (c.lo == c.hi) {                       // condición decidida
                    if (c.lo != 0) { r.lo += v.lo; r.hi += v.hi; r.avg += v.avg; }
                } else {
                    r.lo += std::min(0.0, v.lo); r.hi += std::max(0.0, v.hi); r.avg += c.avg * v.avg;
                    if (c.dice || v.dice) approx = true;
                }
                if (c.val != 0) r.val += v.val;
            }
            return r;
        }
        std::vector<R> args;
        for (const auto& k : n.kids) flatten(eval(k, record), args);
        if (!error.empty()) return {};
        R r;
        for (const R& x : args) { numeric(x, fn); r.dice = r.dice || x.dice; }
        if (!error.empty()) return {};
        if (fn == "suma") {
            for (const R& x : args) { r.lo += x.lo; r.hi += x.hi; r.avg += x.avg; r.val += x.val; }
            return r;
        }
        if (fn == "min" || fn == "max") {
            if (args.empty()) return R::num(0);
            const bool mx = fn == "max";
            auto pick = [mx](double x, double y) { return mx ? std::max(x, y) : std::min(x, y); };
            r.lo = args[0].lo; r.hi = args[0].hi; r.avg = args[0].avg; r.val = args[0].val;
            for (size_t i = 1; i < args.size(); ++i) {
                r.lo = pick(r.lo, args[i].lo); r.hi = pick(r.hi, args[i].hi);
                r.avg = pick(r.avg, args[i].avg); r.val = pick(r.val, args[i].val);
            }
            if (r.dice && args.size() > 1) approx = true;
            return r;
        }
        if (fn == "resto") {
            if (args.size() != 2) { error = "resto() espera dos valores"; return {}; }
            if (args[1].lo <= 0 && args[1].hi >= 0) { error = "resto(): no se puede dividir entre cero"; return {}; }
            auto mod = [&](double x, double y) { double m = std::fmod(x, y); return (m != 0 && (m < 0) != (y < 0)) ? m + y : m; };
            r.val = mod(args[0].val, args[1].val);
            const double top = std::fabs(args[1].hi) > std::fabs(args[1].lo) ? std::fabs(args[1].hi) : std::fabs(args[1].lo);
            r.lo = args[0].lo == args[0].hi && args[1].lo == args[1].hi ? r.val : 0;
            r.hi = args[0].lo == args[0].hi && args[1].lo == args[1].hi ? r.val : top;
            r.avg = args[0].lo == args[0].hi && args[1].lo == args[1].hi ? r.val : mod(args[0].avg, args[1].avg);
            if (r.dice) approx = true;
            return r;
        }
        if (fn == "potencia") {
            if (args.size() != 2) { error = "potencia() espera dos valores"; return {}; }
            return combine("^", args[0], args[1]);
        }
        if (fn == "redondear" && args.size() == 2) {                // redondear(x; decimales)
            const double k = std::pow(10.0, std::max(0.0, std::min(9.0, std::round(args[1].val))));
            auto f = [k](double x) { return std::round(x * k) / k; };
            r.lo = f(args[0].lo); r.hi = f(args[0].hi); r.avg = f(args[0].avg); r.val = f(args[0].val);
            r.dice = args[0].dice;
            if (r.dice && args[0].lo != args[0].hi) approx = true;
            return r;
        }
        if (fn == "logaritmo") {
            if (args.empty() || args.size() > 2) { error = "logaritmo() espera un valor y, si quieres, la base"; return {}; }
            const double base = args.size() == 2 ? args[1].val : 10.0;
            if (base <= 0 || base == 1) { error = "logaritmo(): la base ha de ser positiva y distinta de 1"; return {}; }
            if (args[0].lo <= 0 || args[0].val <= 0) { error = "logaritmo(): solo de números mayores que 0"; return {}; }
            const double lb = std::log(base);
            r.lo = std::log(args[0].lo) / lb; r.hi = std::log(args[0].hi) / lb;
            if (lb < 0) std::swap(r.lo, r.hi);
            r.avg = std::log(args[0].avg) / lb; r.val = std::log(args[0].val) / lb;
            r.dice = args[0].dice;
            if (r.dice && args[0].lo != args[0].hi) approx = true;
            return r;
        }
        if (args.size() != 1) { error = fn + "() espera un solo valor"; return {}; }
        const R& a = args[0];
        if (fn == "raiz" || fn == "ln") {
            if ((fn == "raiz" && a.lo < 0) || (fn == "ln" && a.lo <= 0)) {
                error = fn == "raiz" ? "raiz(): no existe de un número negativo" : "ln(): solo de números mayores que 0";
                return {};
            }
            auto f = [&](double x) { return fn == "raiz" ? std::sqrt(x) : std::log(x); };
            r.lo = f(a.lo); r.hi = f(a.hi); r.avg = f(a.avg); r.val = f(a.val);
            r.dice = a.dice;
            if (r.dice && a.lo != a.hi) approx = true;
            return r;
        }
        if (fn == "abs") {
            if (a.lo >= 0) r = a;
            else if (a.hi <= 0) { r.lo = -a.hi; r.hi = -a.lo; r.avg = -a.avg; r.val = -a.val; }
            else { r.lo = 0; r.hi = std::max(-a.lo, a.hi); r.avg = std::fabs(a.avg); r.val = std::fabs(a.val); if (a.dice) approx = true; }
            r.dice = a.dice;
            return r;
        }
        double (*f)(double) = fn == "redondear" ? static_cast<double(*)(double)>(std::round)
                            : fn == "entero"    ? static_cast<double(*)(double)>(std::floor)
                                                : static_cast<double(*)(double)>(std::ceil);
        r.lo = f(a.lo); r.hi = f(a.hi); r.avg = f(a.avg); r.val = f(a.val);
        if (a.dice && (a.lo != a.hi)) approx = true;
        return r;
    }
};

Value number(double x) {
    if (std::isfinite(x) && x == std::floor(x) && std::fabs(x) < 9e15) return Value::integer(static_cast<long long>(x));
    return Value::real(std::round(x * 100) / 100);
}

bool parse_or_error(const std::string& src, Node& out, std::string& error) {
    std::string perr;
    if (Parser(src).parse(out, perr)) return true;
    error = perr;
    return false;
}

Value fn_check(NativeCtx&, std::vector<Value>& a, std::string&) {
    Node root;
    std::string perr;
    Value::Dict d;
    const bool ok = Parser(a[0].as_str()).parse(root, perr);
    Value::List names;
    if (ok) {
        std::vector<std::string> found;
        collect_names(root, found);
        for (auto& n : found) names.push_back(Value::str(n));
    }
    d["ok"] = Value::boolean(ok);
    d["error"] = Value::str(perr);
    d["names"] = Value::list(std::move(names));
    return Value::dict(std::move(d));
}

// Media de una fórmula con dados que no se calcula en exacto: se tira muchas veces
// con una semilla fija, así el mismo resultado en cada visita.
bool simulated_average(const Node& root, const Value* vars, double& avg) {
    std::mt19937_64 g(0x6772696d6f72696fULL);
    Evaluator sim(vars, true);
    sim.gen = &g;
    sim.quiet = true;
    double sum = 0;
    int ok = 0;
    // Hasta 3000 tiradas, pero sin pasar de ~40 ms: una fórmula carísima no puede colgar la página.
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3000; ++i) {
        sim.error.clear();
        R r = sim.eval(root, false);
        if (sim.error.empty() && !r.is_list) { sum += r.val; ++ok; }
        if ((i & 31) == 31 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(40)) break;
    }
    if (ok < 100) return false;
    avg = sum / ok;
    return true;
}

// El resultado es una lista: sus elementos (números, o textos si lo eran) y su texto.
Value list_result(const R& r, bool roll) {
    std::vector<R> flat;
    std::function<void(const R&)> walk = [&](const R& x) {
        if (x.is_list) { for (const R& i : x.items) walk(i); } else flat.push_back(x);
    };
    walk(r);
    Value::List items;
    std::string joined;
    for (size_t i = 0; i < flat.size(); ++i) {
        const R& x = flat[i];
        if (x.bad_num) { items.push_back(Value::str(x.txt)); joined += (i ? ", " : "") + x.txt; }
        else {
            const double v = roll ? x.val : x.avg;
            items.push_back(number(v));
            joined += (i ? ", " : "") + (x.text ? x.txt : Evaluator::number_text(v));
        }
    }
    Value::Dict d;
    d["min"] = Value::integer(0); d["max"] = Value::integer(0); d["avg"] = Value::integer(0);
    d["total"] = Value::integer(0);
    d["dice"] = Value::boolean(r.dice);
    d["is_text"] = Value::boolean(true);
    d["is_list"] = Value::boolean(true);
    d["text"] = Value::str(joined);
    d["items"] = Value::list(std::move(items));
    return Value::dict(std::move(d));
}

Value fn_stats(NativeCtx&, std::vector<Value>& a, std::string& error) {
    Node root;
    if (!parse_or_error(a[0].as_str(), root, error)) return Value::null();
    Evaluator ev(a.size() > 1 ? &a[1] : nullptr, false);
    R r = ev.eval(root);
    if (!ev.error.empty()) { error = ev.error; return Value::null(); }
    if (r.is_list) return list_result(r, false);
    if (r.bad_num) {                                          // el resultado es un texto
        Value::Dict d;
        d["min"] = Value::integer(0); d["max"] = Value::integer(0); d["avg"] = Value::integer(0);
        d["dice"] = Value::boolean(r.dice);
        d["is_text"] = Value::boolean(true);
        d["text"] = Value::str(r.txt);
        return Value::dict(std::move(d));
    }
    if (ev.approx) {
        double avg;
        if (simulated_average(root, a.size() > 1 ? &a[1] : nullptr, avg)) r.avg = std::min(r.hi, std::max(r.lo, avg));
    }
    Value::Dict d;
    d["min"] = number(r.lo);
    d["max"] = number(r.hi);
    d["avg"] = number(r.avg);
    d["dice"] = Value::boolean(r.dice);
    return Value::dict(std::move(d));
}

Value fn_roll(NativeCtx&, std::vector<Value>& a, std::string& error) {
    Node root;
    if (!parse_or_error(a[0].as_str(), root, error)) return Value::null();
    Evaluator ev(a.size() > 1 ? &a[1] : nullptr, true);
    R r = ev.eval(root);
    if (!ev.error.empty()) { error = ev.error; return Value::null(); }
    if (r.is_list) return list_result(r, true);
    std::string detail;
    for (const auto& s : ev.rolls) detail += (detail.empty() ? "" : " · ") + s;
    Value::Dict d;
    d["total"] = number(r.bad_num ? 0 : r.val);
    if (r.bad_num) { d["text"] = Value::str(r.txt); d["is_text"] = Value::boolean(true); }
    d["detail"] = Value::str(detail);
    d["min"] = number(r.lo);
    d["max"] = number(r.hi);
    d["avg"] = number(r.avg);
    return Value::dict(std::move(d));
}

} // namespace

LUX_MODULE(formula, {
    {"check", "s>d",   fn_check},
    {"stats", "s|D>d", fn_stats},
    {"roll",  "s|D>d", fn_roll},
})

} // namespace lux_script
