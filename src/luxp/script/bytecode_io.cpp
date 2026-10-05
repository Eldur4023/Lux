#include "script/bytecode_io.hpp"

#include <lux_script/bytecode.hpp>
#include <lux_script/natives.hpp>
#include <page/doc_codec.hpp>   // NameTable
#include <util/wire.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace luxium::script {

namespace {

using lux_script::Chunk;
using lux_script::Instr;
using lux_script::Op;
using luxium::page::NameTable;

// Bumped whenever the encoding or the Op enum changes: an old .luxbc is
// refused (re-pack it) instead of being run with shifted opcodes.
constexpr uint32_t kFormat = 3;   // 3: LEB128, string table, line deltas, no local names
constexpr uint32_t kOpCount = static_cast<uint32_t>(Op::ReturnNull) + 1;
static_assert(kOpCount == 46, "Op changed: bump kFormat and update stack_effect()");

// Hard ceilings so a forged file cannot ask for absurd allocations.
constexpr uint32_t kMaxFunctions = 1u << 16;
constexpr uint32_t kMaxCode      = 1u << 22;
constexpr uint32_t kMaxConsts    = 1u << 20;
constexpr uint32_t kMaxLocals    = 1u << 16;
constexpr int      kMaxStack     = 1 << 16;

// Natives travel BY NAME (a table per program; CallNative's operand indexes
// it).  The loader resolves each name against this browser's own table --
// browser_api.inc, which IS the policy: a name it does not list is refused,
// whatever version of Lux compiled the page.
using wire::Reader;
using wire::Writer;

enum : uint8_t { kNull, kBool, kInt, kFloat, kStr, kFunc };

// (pops, pushes) of one instruction; pops < 0 = invalid opcode.
std::pair<int, int> stack_effect(const Instr& in) {
    const int argc = int(in.operand & 0xFF);
    const int count = int(std::min<uint32_t>(in.operand, kMaxStack + 1));   // no int overflow
    switch (in.op) {
        case Op::Const: case Op::LoadLocal: return {0, 1};
        case Op::StoreLocal: case Op::Pop: return {1, 0};
        case Op::CoerceInt: case Op::CoerceFloat: case Op::Neg: case Op::Not:
        case Op::IterList: case Op::GetMember: return {1, 1};
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        case Op::AddInt: case Op::SubInt: case Op::MulInt:
        case Op::LtInt: case Op::LeInt: case Op::GtInt: case Op::GeInt:
        case Op::GetIndex: case Op::SetMember: return {2, 1};
        case Op::SetIndex: return {3, 1};
        case Op::ConcatN: return {count, 1};
        case Op::MakeList: return {count, 1};
        case Op::MakeDict: return {count * 2, 1};
        case Op::Jump: return {0, 0};
        case Op::JumpIfFalse: return {1, 0};
        case Op::JumpIfFalsePeek: case Op::JumpIfTruePeek: return {1, 1};   // see verify_chunk
        case Op::CallFunction: case Op::CallNative: return {argc, 1};
        case Op::CallMethod: return {argc + 1, 1};
        case Op::Return: return {1, 0};
        case Op::ReturnNull: return {0, 0};
        default: return {-1, 0};   // module calls and await: never in a page
    }
}

bool fail(std::string* err, size_t fn, size_t pc, const std::string& why) {
    if (err) *err = "funcion " + std::to_string(fn) + ", pc " + std::to_string(pc) + ": " + why;
    return false;
}

bool verify_chunk(const Compiled& c, size_t fi, std::string* err) {
    const Chunk& ch = *c.functions[fi];
    const size_t n = ch.code.size();
    auto is_str_const = [&](uint32_t k) { return k < ch.constants.size() && ch.constants[k].is_str(); };

    for (const auto& v : ch.constants)
        if (v.is_func() && (v.as_func_index() < 0 || size_t(v.as_func_index()) >= c.functions.size()))
            return fail(err, fi, 0, "constante fn fuera de rango");
    for (const auto& t : ch.try_ranges)
        if (t.begin > t.end || t.end > n || t.catch_pc >= n)
            return fail(err, fi, 0, "rango try invalido");

    // Operands.
    for (size_t pc = 0; pc < n; ++pc) {
        const Instr& in = ch.code[pc];
        if (stack_effect(in).first < 0) return fail(err, fi, pc, "opcode no permitido en una pagina");
        switch (in.op) {
            case Op::Const:
                if (in.operand >= ch.constants.size()) return fail(err, fi, pc, "constante fuera de rango");
                break;
            case Op::LoadLocal: case Op::StoreLocal:
                if (in.operand >= uint32_t(ch.num_locals)) return fail(err, fi, pc, "slot local fuera de rango");
                break;
            case Op::GetMember: case Op::SetMember:
                if (!is_str_const(in.operand)) return fail(err, fi, pc, "nombre de campo invalido");
                break;
            case Op::CallMethod:
                if (!is_str_const(in.operand >> 8)) return fail(err, fi, pc, "nombre de metodo invalido");
                break;
            case Op::Jump:
                if (in.operand > n) return fail(err, fi, pc, "salto fuera del codigo");
                break;
            // The VM counts steps only on a backward Jump: a backward
            // conditional jump would be a loop the step limit never sees.
            case Op::JumpIfFalse: case Op::JumpIfFalsePeek: case Op::JumpIfTruePeek:
                if (in.operand > n || in.operand <= pc) return fail(err, fi, pc, "salto condicional no hacia delante");
                break;
            case Op::CallFunction: {
                const uint32_t callee = in.operand >> 8;
                if (callee >= c.functions.size()) return fail(err, fi, pc, "funcion inexistente");
                if (int(in.operand & 0xFF) > c.functions[callee]->num_locals)
                    return fail(err, fi, pc, "mas argumentos que locales en la funcion llamada");
                break;
            }
            case Op::CallNative: {
                const int id = int(in.operand >> 8);   // already resolved by load_bytecode
                if (id < 0) return fail(err, fi, pc, "nativa no permitida en el navegador");
                const auto& def = lux_script::native_at(id);
                const int argc = int(in.operand & 0xFF);
                if (argc < def.min_args || (def.max_args >= 0 && argc > def.max_args) || !def.fn)
                    return fail(err, fi, pc, std::string("aridad invalida para ") + def.name);
                break;
            }
            default: break;
        }
    }

    // Stack heights (relative to the frame), one forward pass with a worklist.
    std::vector<int> height(n + 1, -1);
    std::vector<size_t> work;
    auto reach = [&](size_t pc, int h, size_t from) {
        if (h < 0 || h > kMaxStack) return fail(err, fi, from, "pila fuera de limites");
        if (height[pc] == -1) { height[pc] = h; if (pc < n) work.push_back(pc); return true; }
        if (height[pc] != h) return fail(err, fi, from, "altura de pila inconsistente");
        return true;
    };
    if (n == 0) return true;
    if (!reach(0, 0, 0)) return false;
    for (const auto& t : ch.try_ranges)          // the VM resets to the frame and pushes the error
        if (!reach(t.catch_pc, 1, t.catch_pc)) return false;

    while (!work.empty()) {
        const size_t pc = work.back();
        work.pop_back();
        const Instr& in = ch.code[pc];
        const int h = height[pc];
        const auto [pops, pushes] = stack_effect(in);
        if (h < pops) return fail(err, fi, pc, "la pila se vacia");
        switch (in.op) {
            case Op::Return: case Op::ReturnNull: break;
            case Op::Jump:
                if (!reach(in.operand, h, pc)) return false;
                break;
            case Op::JumpIfFalse:
                if (!reach(in.operand, h - 1, pc) || !reach(pc + 1, h - 1, pc)) return false;
                break;
            case Op::JumpIfFalsePeek: case Op::JumpIfTruePeek:   // jump keeps the top, fall-through pops it
                if (!reach(in.operand, h, pc) || !reach(pc + 1, h - 1, pc)) return false;
                break;
            default:
                if (!reach(pc + 1, h - pops + pushes, pc)) return false;
        }
    }
    return true;
}

} // namespace

std::string serialize(const Compiled& c) {
    lux_script::BrowserProfile browser;   // native ids are the browser table's
    std::vector<std::string> names;       // native name table
    auto name_index = [&](int id) {
        const std::string n = lux_script::native_at(id).name;
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == n) return uint32_t(i);
        names.push_back(n);
        return uint32_t(names.size() - 1);
    };
    std::vector<std::vector<uint32_t>> operands;   // CallNative remapped to the name table
    for (const auto& fp : c.functions) {
        operands.emplace_back();
        for (const auto& in : fp->code)
            operands.back().push_back(in.op == Op::CallNative
                ? (name_index(int(in.operand >> 8)) << 8) | (in.operand & 0xFF) : in.operand);
    }

    // Body first: the string table fills while the constants are written,
    // but travels before them.  One table for the whole program: the same
    // "class" or "view-" is spelled once, not once per function.
    NameTable strings;
    Writer body;
    body.uv(c.functions.size());
    for (size_t fi = 0; fi < c.functions.size(); ++fi) {
        const Chunk& ch = *c.functions[fi];
        body.uv(uint32_t(ch.num_locals));
        body.uv(ch.code.size());
        int prev_line = 0;
        for (size_t k = 0; k < ch.code.size(); ++k) {
            const Instr& in = ch.code[k];
            body.u8(uint8_t(in.op));
            body.uv(operands[fi][k]);
            body.sv(int64_t(in.loc.line) - prev_line);
            prev_line = in.loc.line;
        }
        body.uv(ch.constants.size());
        for (const auto& v : ch.constants) {
            if (v.is_null())       { body.u8(kNull); }
            else if (v.is_bool())  { body.u8(kBool); body.u8(v.as_bool()); }
            else if (v.is_int())   { body.u8(kInt); body.sv(v.as_int()); }
            else if (v.is_float()) { body.u8(kFloat); double d = v.as_float(); uint64_t b; std::memcpy(&b, &d, 8); body.u64(b); }
            else if (v.is_str())   { body.u8(kStr); body.uv(strings.id(v.as_str())); }
            else if (v.is_func())  { body.u8(kFunc); body.uv(uint64_t(v.as_func_index())); }
            else                   { body.u8(kNull); }   // the emitter never puts lists/dicts in the pool
        }
        body.uv(ch.try_ranges.size());
        for (const auto& t : ch.try_ranges) { body.uv(t.begin); body.uv(t.end); body.uv(t.catch_pc); }
    }
    body.uv(c.line_map.size());
    int prev = 0;
    for (int l : c.line_map) { body.sv(int64_t(l) - prev); prev = l; }

    Writer w;
    w.out = "LUXBC";
    w.uv(kFormat);
    w.uv(kOpCount);
    w.uv(names.size());
    for (const auto& n : names) w.str(n);
    w.uv(strings.names.size());
    for (const auto& n : strings.names) w.str(n);
    return w.out + body.out;
}

std::unique_ptr<Compiled> load_bytecode(const std::string& bytes, std::string* err) {
    auto bad = [&](const std::string& why) { if (err) *err = why; return nullptr; };
    if (bytes.compare(0, 5, "LUXBC") != 0) return bad("no es bytecode de Luxium");
    Reader r{bytes, 5};
    if (r.uv32() != kFormat || r.uv32() != kOpCount)
        return bad("bytecode de otra version de Luxium: vuelve a empaquetar");

    lux_script::BrowserProfile browser;
    std::vector<int> native_ids;   // name table -> this browser's ids (-1 = not offered)
    const uint32_t nn_names = r.count(1024, 1);
    for (uint32_t i = 0; i < nn_names && r.ok; ++i)
        native_ids.push_back(lux_script::native_id(r.str(256)));
    std::vector<std::string> strings;   // the program's string constants
    const uint32_t n_strings = r.count(kMaxConsts, 1);
    for (uint32_t i = 0; i < n_strings && r.ok; ++i) strings.push_back(r.str());

    auto c = std::make_unique<Compiled>();
    const uint32_t nf = r.uv32();
    if (!r.ok || nf == 0 || nf > kMaxFunctions) return bad("numero de funciones invalido");
    for (uint32_t f = 0; f < nf && r.ok; ++f) {
        auto ch = std::make_shared<Chunk>();
        const uint32_t locals = r.uv32();
        if (locals > kMaxLocals) return bad("demasiados locales");
        ch->num_locals = int(locals);
        // The VM never reads local_names; the emitter fills them only for its
        // own messages.  Keep the slots so nothing indexing them can fall off.
        ch->local_names.assign(locals, std::string());
        const uint32_t ncode = r.count(kMaxCode, 3);
        int64_t line = 0;
        for (uint32_t i = 0; i < ncode && r.ok; ++i) {
            Instr in;
            const uint8_t op = r.u8();
            if (op >= kOpCount) return bad("opcode desconocido");
            in.op = Op(op);
            const uint64_t operand = r.uv();
            if (operand > 0xFFFFFFFFull) return bad("operando fuera de rango");
            in.operand = uint32_t(operand);
            if (in.op == Op::CallNative) {
                const uint32_t k = in.operand >> 8;
                if (k >= native_ids.size()) return bad("nativa fuera de la tabla de nombres");
                if (native_ids[k] < 0) return bad("nativa no permitida en el navegador");
                in.operand = (uint32_t(native_ids[k]) << 8) | (in.operand & 0xFF);
            }
            line += r.sv();
            if (line < 0 || line > INT32_MAX) return bad("linea fuera de rango");
            in.loc.line = int(line);
            ch->code.push_back(in);
        }
        const uint32_t nk = r.count(kMaxConsts, 1);
        for (uint32_t i = 0; i < nk && r.ok; ++i) {
            switch (r.u8()) {
                case kNull:  ch->constants.push_back(Value::null()); break;
                case kBool:  ch->constants.push_back(Value::boolean(r.u8() != 0)); break;
                case kInt:   ch->constants.push_back(Value::integer((long long)r.sv())); break;
                case kFloat: { uint64_t b = r.u64(); double d; std::memcpy(&d, &b, 8); ch->constants.push_back(Value::real(d)); break; }
                case kStr: {
                    const uint32_t k = r.uv32();
                    if (!r.ok || k >= strings.size()) return bad("cadena fuera de la tabla");
                    ch->constants.push_back(Value::str(strings[k]));
                    break;
                }
                case kFunc:  ch->constants.push_back(Value::func((long long)r.uv())); break;
                default:     return bad("constante de tipo desconocido");
            }
        }
        const uint32_t nt = r.count(ncode + 1, 3);
        for (uint32_t i = 0; i < nt && r.ok; ++i) {
            lux_script::TryRange t;
            t.begin = r.uv32(); t.end = r.uv32(); t.catch_pc = r.uv32();
            ch->try_ranges.push_back(t);
        }
        c->functions.push_back(std::move(ch));
    }
    const uint32_t nl = r.count(kMaxCode, 1);
    int64_t mapped = 0;
    for (uint32_t i = 0; i < nl && r.ok; ++i) {
        mapped += r.sv();
        if (mapped < 0 || mapped > INT32_MAX) return bad("line_map invalido");
        c->line_map.push_back(int(mapped));
    }
    if (!r.ok) return bad("bytecode truncado");
    if (r.at != bytes.size()) return bad("bytes sobrantes tras el bytecode");

    for (size_t f = 0; f < c->functions.size(); ++f)
        if (!verify_chunk(*c, f, err)) return nullptr;
    return c;
}

Capabilities capabilities(const Compiled& c) {
    lux_script::BrowserProfile browser;
    Capabilities caps;
    for (const auto& fp : c.functions)
        for (const auto& in : fp->code) {
            if (in.op == Op::CallNative) {
                std::string name = lux_script::native_at(int(in.operand >> 8)).name;
                if (name.starts_with("__")) name = name.substr(2);
                caps.natives.insert(name);
            } else if (in.op == Op::CallMethod) {
                caps.methods.insert(fp->constants[in.operand >> 8].as_str());
            }
        }
    return caps;
}

} // namespace luxium::script
