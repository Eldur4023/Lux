#include <lux_script/vm.hpp>
#include <lux_script/builtin_module.hpp>

namespace lux_script {

namespace {

// Fails with a message naming the types involved: a runtime error in a .lux
// has to be as readable as a compile-time one.
VM::Result fail(std::string msg, SourceLoc loc) {
    VM::Result r;
    r.status    = VM::Status::Error;
    r.error     = std::move(msg);
    r.error_loc = loc;
    return r;
}

bool numeric_pair(const Value& a, const Value& b) {
    return a.is_num() && b.is_num();
}

// Ordering comparison: only between numbers, or between strings -- the
// actual logic is Value::less_than() (value.hpp/.cpp), shared with
// natives.cpp's List.sort() so the two never quietly diverge. Le/Gt/Ge are
// derived from Lt the usual way (a<=b is !(b<a), a>b is b<a, a>=b is
// !(a<b)): valid for any domain where `ok` comes back true, since numbers
// and strings are both total orders.
bool compare(const Value& a, const Value& b, Op op, bool& ok) {
    switch (op) {
        case Op::Lt: return a.less_than(b, ok);
        case Op::Gt: return b.less_than(a, ok);
        case Op::Le: { bool r = b.less_than(a, ok); return ok && !r; }
        default:     { bool r = a.less_than(b, ok); return ok && !r; } // Ge
    }
}

// A Lux Script value can only be Int/Float/Bool when crossing over to
// native code -- generar_funcion_nativa() already guarantees that a
// function is only offered through this path if all of its parameters and
// its return type are one of those three, so the real type always matches
// what the wrapper expects.
NativeValue a_nativevalue(const Value& v) {
    NativeValue n;
    if (v.is_float()) { n.tag = NativeValue::Tag::Float; n.d = v.as_float(); }
    else if (v.is_bool()) { n.tag = NativeValue::Tag::Bool; n.b = v.as_bool(); }
    else { n.tag = NativeValue::Tag::Int; n.i = v.as_int(); }
    return n;
}

Value de_nativevalue(const NativeValue& n) {
    switch (n.tag) {
        case NativeValue::Tag::Float: return Value::real(n.d);
        case NativeValue::Tag::Bool:  return Value::boolean(n.b);
        default:                      return Value::integer(n.i);
    }
}

} // namespace

VM::Result VM::start(const Chunk& chunk, std::vector<Value> params, NativeCtx& ctx,
                     const FunctionTable* functions, const NativeDispatch* native) {
    functions_ = functions;
    native_    = native;
    frames_.clear();
    stack_.clear();

    // Capacity reserved once: frames_ has a known fixed cap (kMaxFrames)
    // that never changes, and stack_/locals_ use a generous margin covering
    // the bulk of real handlers instead of the arbitrary 32 from before.
    // reserve() doesn't reallocate if the capacity already suffices (the
    // normal case when this same VM is reused between requests via
    // project.cpp's thread_local shared_vm), so this doesn't change
    // observable behavior: it just avoids the repeated reallocations of
    // std::vector::push_back/resize that showed up in the CPU profile
    // (Value::emplace_back, see bench/RESULTS.md).
    frames_.reserve(kMaxFrames);
    stack_.reserve(256);
    locals_.reserve(256);

    locals_.assign(static_cast<size_t>(chunk.num_locals), Value::null());
    for (size_t i = 0; i < params.size() && i < locals_.size(); ++i)
        locals_[i] = std::move(params[i]);

    frames_.push_back(Frame{&chunk, 0, 0, 0});
    return execute(ctx);
}

VM::Result VM::resume(Value awaited, NativeCtx& ctx) {
    // The awaited value takes the slot the `await` expression left behind.
    push(std::move(awaited));
    return execute(ctx);
}

namespace {

// The innermost try covering `pc`: among nested ones, the narrowest range.
const TryRange* find_handler(const Chunk& chunk, size_t pc) {
    const TryRange* best = nullptr;
    for (const auto& r : chunk.try_ranges) {
        if (pc < r.begin || pc >= r.end) continue;
        if (!best || (r.end - r.begin) < (best->end - best->begin)) best = &r;
    }
    return best;
}

Value error_value(const std::string& message) {
    Value::Dict d;
    d["message"] = Value::str(message);
    return Value::dict(std::move(d));
}

} // namespace

// Runs and, if something fails inside a `try`, jumps to its `catch` and goes on.
// The error is delivered as one more value, on top of the stack.
VM::Result VM::execute(NativeCtx& ctx) { return unwind(run_until_error(ctx), ctx); }

VM::Result VM::resume_error(std::string message, SourceLoc loc, NativeCtx& ctx) {
    return unwind(fail(std::move(message), loc), ctx);
}

VM::Result VM::unwind(Result r, NativeCtx& ctx) {
    while (r.status == Status::Error) {
        // The frame's pc already points at the next instruction, so the one
        // that failed is the previous one.  An error climbs the frames until it
        // finds a try covering it: if the callee does not handle it, the caller may.
        const TryRange* h = nullptr;
        while (!frames_.empty()) {
            Frame& f = frames_.back();
            h = find_handler(*f.chunk, f.pc - 1);
            if (h) break;
            stack_.resize(f.stack_base);
            locals_.resize(f.locals_base);
            frames_.pop_back();
        }
        if (!h) return r;

        // At a statement boundary the operand stack is empty, which is where a
        // try can begin; clearing it leaves the state consistent without having
        // to record depths.
        stack_.resize(frames_.back().stack_base);
        push(error_value(r.error));
        frames_.back().pc = h->catch_pc;
        r = run_until_error(ctx);
    }
    return r;
}

// The language's '+', in a single place: both Op::Add and the fallback path of
// Op::ConcatN use it, so collapsing a chain cannot change either the result or
// the error message.
//
// '+' demands that BOTH sides be of the same type.  Two strings concatenate,
// two numbers add, two lists join.
//
// 1 + "1" is an error, not "11": operating across different types is exactly
// what Lux Script does not want to inherit from JavaScript.  To join a number
// to a string you have to say so: "n = " + str(n).
static bool add_values(const Value& a, const Value& b, Value& out, std::string& err) {
    if (a.is_str() && b.is_str()) {
        out = Value::str(a.as_str() + b.as_str());
        return true;
    }
    if (a.is_str() || b.is_str()) {
        err = std::string("cannot add ") + a.type_name() + " and " +
              b.type_name() + "; to concatenate use str(): \"...\" + str(x)";
        return false;
    }
    if (numeric_pair(a, b)) {
        out = (a.is_int() && b.is_int())
                  ? Value::integer(a.as_int() + b.as_int())
                  : Value::real(a.as_float() + b.as_float());
        return true;
    }
    if (a.is_list() && b.is_list()) {
        Value::List l = a.as_list();
        for (const auto& v : b.as_list()) l.push_back(v);
        out = Value::list(std::move(l));
        return true;
    }
    err = std::string("cannot add ") + a.type_name() + " and " + b.type_name();
    return false;
}

// A specialized opcode behaves exactly like its generic form when the type
// guard fails.
static Op generic_form_of(Op op) {
    switch (op) {
        case Op::AddInt: return Op::Add;
        case Op::SubInt: return Op::Sub;
        case Op::MulInt: return Op::Mul;
        case Op::LtInt:  return Op::Lt;
        case Op::LeInt:  return Op::Le;
        case Op::GtInt:  return Op::Gt;
        case Op::GeInt:  return Op::Ge;
        default:         return op;
    }
}

VM::Result VM::run_until_error(NativeCtx& ctx) {
    // The counter resets on every stretch: a legitimate SSE loop can be alive
    // for hours, but between two suspensions it must not exceed kStepLimit.
    long long steps = 0;

    while (!frames_.empty()) {
        Frame&       frame = frames_.back();
        const Chunk& chunk = *frame.chunk;

        // A function that runs off the end returns null to its caller.
        if (frame.pc >= chunk.code.size()) {
            size_t lbase = frame.locals_base, sbase = frame.stack_base;
            frames_.pop_back();
            if (frames_.empty()) return Result{};
            stack_.resize(sbase);
            locals_.resize(lbase);
            push(Value::null());
            continue;
        }

        const Instr& in = chunk.code[frame.pc++];

        switch (in.op) {
            case Op::Const:
                push(chunk.constants[in.operand]);
                break;

            case Op::LoadLocal:
                push(locals_[frame.locals_base + in.operand]);
                break;

            case Op::StoreLocal:
                locals_[frame.locals_base + in.operand] = pop();
                break;

            case Op::Pop:
                pop();
                break;

            case Op::CoerceInt: {
                Value v = pop();
                push(v.is_float() ? Value::integer(static_cast<long long>(v.as_float())) : v);
                break;
            }

            case Op::CoerceFloat: {
                Value v = pop();
                push(v.is_int() ? Value::real(static_cast<double>(v.as_int())) : v);
                break;
            }

            case Op::Add: generic_add: {
                Value b = pop(), a = pop();
                Value       r;
                std::string err;
                if (!add_values(a, b, r, err)) return fail(err, in.loc);
                push(std::move(r));
                break;
            }

            // A chain of '+' already collapsed by the emitter.
            case Op::ConcatN: {
                const size_t n    = in.operand;
                const size_t base = stack_.size() - n;

                // Guard: it is only concatenation if ALL of them are strings.
                // The total is measured on the way, which is what allows one reserve.
                bool   todas = true;
                size_t total = 0;
                for (size_t i = 0; i < n; ++i) {
                    if (!stack_[base + i].is_str()) { todas = false; break; }
                    total += stack_[base + i].as_str().size();
                }

                if (todas) {
                    std::string out;
                    out.reserve(total);
                    for (size_t i = 0; i < n; ++i) out += stack_[base + i].as_str();
                    stack_.resize(base);
                    push(Value::str(std::move(out)));
                    break;
                }

                // If they are not, it folds with the same generic '+' and to the
                // left: same result and same error as without collapsing.
                Value acc = std::move(stack_[base]);
                for (size_t i = 1; i < n; ++i) {
                    Value       r;
                    std::string err;
                    if (!add_values(acc, stack_[base + i], r, err)) {
                        stack_.resize(base);
                        return fail(err, in.loc);
                    }
                    acc = std::move(r);
                }
                stack_.resize(base);
                push(std::move(acc));
                break;
            }

            case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: generic_arith: {
                const Op og = generic_form_of(in.op);
                Value b = pop(), a = pop();
                if (!numeric_pair(a, b))
                    return fail(std::string("arithmetic between ") +
                                a.type_name() + " and " + b.type_name(), in.loc);

                bool ints = a.is_int() && b.is_int();
                if (og == Op::Mod) {
                    if (!ints) return fail("'%' only applies to integers", in.loc);
                    if (b.as_int() == 0) return fail("modulo by zero", in.loc);
                    push(Value::integer(a.as_int() % b.as_int()));
                    break;
                }
                if (og == Op::Div) {
                    if (b.as_float() == 0) return fail("division by zero", in.loc);
                    if (ints && a.as_int() % b.as_int() == 0)
                        push(Value::integer(a.as_int() / b.as_int()));
                    else
                        push(Value::real(a.as_float() / b.as_float()));
                    break;
                }
                if (ints) {
                    long long x = a.as_int(), y = b.as_int();
                    push(Value::integer(og == Op::Sub ? x - y : x * y));
                } else {
                    double x = a.as_float(), y = b.as_float();
                    push(Value::real(og == Op::Sub ? x - y : x * y));
                }
                break;
            }

            case Op::Neg: {
                Value a = pop();
                if (a.is_int())        push(Value::integer(-a.as_int()));
                else if (a.is_float()) push(Value::real(-a.as_float()));
                else return fail(std::string("cannot negate ") + a.type_name(), in.loc);
                break;
            }

            case Op::Eq: { Value b = pop(), a = pop(); push(Value::boolean(a.equals(b)));  break; }
            case Op::Ne: { Value b = pop(), a = pop(); push(Value::boolean(!a.equals(b))); break; }

            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: generic_cmp: {
                Value b = pop(), a = pop();
                bool ok = false;
                bool r  = compare(a, b, generic_form_of(in.op), ok);
                if (!ok)
                    return fail(std::string("cannot compare ") + a.type_name() +
                                " and " + b.type_name(), in.loc);
                push(Value::boolean(r));
                break;
            }

            case Op::Not:
                push(Value::boolean(!pop().truthy()));
                break;

            // ── Integers known at compile time ──────────────────────────────
            //
            // The emitter places these when it can prove both sides are `int`.
            // They skip the generic path's cascade of type checks and the two
            // Value copies: the top is read without popping it.
            //
            //
            // The guard is there because the declared type is not enforced on
            // assignment.  If it does not match, it falls to the generic path and
            // the program behaves the same, with the same error message.
            case Op::AddInt: case Op::SubInt: case Op::MulInt:
            case Op::LtInt:  case Op::LeInt:  case Op::GtInt: case Op::GeInt: {
                const Value& vb = stack_[stack_.size() - 1];
                const Value& va = stack_[stack_.size() - 2];
                if (!va.is_int() || !vb.is_int()) {
                    if (in.op == Op::AddInt) goto generic_add;
                    if (in.op == Op::SubInt || in.op == Op::MulInt) goto generic_arith;
                    goto generic_cmp;
                }
                const long long x = va.as_int(), y = vb.as_int();
                stack_.pop_back();
                stack_.pop_back();
                switch (in.op) {
                    case Op::AddInt: push(Value::integer(x + y));  break;
                    case Op::SubInt: push(Value::integer(x - y));  break;
                    case Op::MulInt: push(Value::integer(x * y));  break;
                    case Op::LtInt:  push(Value::boolean(x <  y)); break;
                    case Op::LeInt:  push(Value::boolean(x <= y)); break;
                    case Op::GtInt:  push(Value::boolean(x >  y)); break;
                    default:         push(Value::boolean(x >= y)); break;
                }
                break;
            }

            case Op::Jump:
                // The counter is only checked here.  An infinite loop needs a
                // backward jump by definition, and infinite recursion is cut
                // earlier by the frame cap: checking on every instruction was a
                // branch per instruction for nothing.
                if (in.operand <= frame.pc && ++steps > kStepLimit)
                    return fail("handler exceeded the step limit: infinite loop?",
                                in.loc);
                frame.pc = in.operand;
                break;

            case Op::JumpIfFalse:
                if (!pop().truthy()) frame.pc = in.operand;
                break;

            case Op::JumpIfFalsePeek:
                if (!stack_.back().truthy()) frame.pc = in.operand; else pop();
                break;

            case Op::JumpIfTruePeek:
                if (stack_.back().truthy()) frame.pc = in.operand; else pop();
                break;

            case Op::MakeList: {
                Value::List l;
                l.resize(in.operand);
                for (uint32_t i = in.operand; i-- > 0;) l[i] = pop();
                push(Value::list(std::move(l)));
                break;
            }

            case Op::MakeDict: {
                // The pairs are already on the stack in key,value order: they are
                // read right there.  They used to be dumped into a temporary
                // vector, which was one more allocation per dictionary, and the
                // dictionary grew in steps because nobody told it the key count.
                const size_t n    = in.operand;
                const size_t base = stack_.size() - n * 2;

                Value::Dict d;
                d.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    Value& k = stack_[base + i * 2];
                    Value& v = stack_[base + i * 2 + 1];
                    if (!k.is_str()) {
                        std::string t = k.type_name();
                        stack_.resize(base);
                        return fail("a Dict key must be a string, not " + t,
                                    in.loc);
                    }
                    d[k.as_str()] = std::move(v);
                }
                stack_.resize(base);
                push(Value::dict(std::move(d)));
                break;
            }

            case Op::GetIndex: {
                Value idx = pop(), obj = pop();
                if (obj.is_list()) {
                    if (!idx.is_int())
                        return fail("a List index must be an int", in.loc);
                    long long i = idx.as_int();
                    auto& l = obj.as_list();
                    if (i < 0 || i >= (long long)l.size())
                        return fail("index out of range: " + std::to_string(i) +
                                    " (size " + std::to_string(l.size()) + ")", in.loc);
                    push(l[static_cast<size_t>(i)]);
                } else if (obj.is_dict()) {
                    if (!idx.is_str())
                        return fail(idx.is_int() ? dict_int_index_error(obj)
                                                 : "a Dict key must be a string", in.loc);
                    auto& d  = obj.as_dict();
                    auto  it = d.find(idx.as_str());
                    push(it == d.end() ? Value::null() : it->second);
                } else {
                    return fail(std::string("cannot index ") + obj.type_name(), in.loc);
                }
                break;
            }

            // A `for` always walks a list: a Dict is walked by its keys, which is
            // what someone coming from Python expects.
            case Op::IterList: {
                Value v = pop();
                if (v.is_list()) { push(std::move(v)); break; }
                if (v.is_dict()) {
                    Value::List keys;
                    keys.reserve(v.as_dict().size());
                    for (const auto& [k, _] : v.as_dict()) keys.push_back(Value::str(k));
                    push(Value::list(std::move(keys)));
                    break;
                }
                // A string walks its own codepoints (utf8_chars(),
                // value.hpp) -- there was no OTHER way to go character by
                // character over a string at all before this (no index-
                // based char access either): a slugify, a per-letter
                // validation, anything of that shape had no way to be
                // written. Same helper `split(s, "")` uses, so both give
                // the same characters for the same string.
                if (v.is_str()) {
                    Value::List chars;
                    for (auto& ch : utf8_chars(v.as_str())) chars.push_back(Value::str(std::move(ch)));
                    push(Value::list(std::move(chars)));
                    break;
                }
                return fail(std::string("cannot iterate over ") + v.type_name() +
                            " with 'for'", in.loc);
            }

            case Op::GetMember: {
                Value obj = pop();
                const std::string& name = chunk.constants[in.operand].as_str();
                if (!obj.is_dict())
                    return fail(std::string("'") + name + "' on " + obj.type_name() +
                                ", which has no fields", in.loc);
                auto& d  = obj.as_dict();
                auto  it = d.find(name);
                push(it == d.end() ? Value::null() : it->second);
                break;
            }

            case Op::CallMethod: {
                uint32_t name_k = in.operand >> 8;
                int      argc   = static_cast<int>(in.operand & 0xFF);

                std::vector<Value> args = pop_args(argc);
                Value recv = pop();

                std::string error;
                Value out = call_method(ctx, recv, chunk.constants[name_k].as_str(),
                                        args, error);
                if (!error.empty()) return fail(std::move(error), in.loc);
                push(std::move(out));
                break;
            }

            // Same shape, two id-spaces: kNatives vs BuiltinModuleRegistry.
            case Op::CallNative: case Op::CallBuiltinModule: {
                int id = static_cast<int>(in.operand >> 8);
                std::vector<Value> args = pop_args(static_cast<int>(in.operand & 0xFF));

                std::string error;
                Value out = in.op == Op::CallNative ? native_at(id).fn(ctx, args, error)
                                                    : builtin_module_function_at(id).call(ctx, args, error);
                if (!error.empty()) return fail(std::move(error), in.loc);
                push(std::move(out));
                break;
            }

            // The VM does not know how to wait: it gathers the arguments, stops,
            // and lets the driver do the real co_await on the engine.  On the way
            // back, resume() pushes the result and the frame carries on.
            //
            // CallAsyncModule's id lives in BuiltinModuleRegistry's id-space,
            // not kNatives': await_is_module tells the driver (project.cpp)
            // so it doesn't confuse it with is_db_await/sleep/__ws_recv.
            case Op::CallAsync: case Op::CallAsyncModule: {
                Result r;
                r.status          = Status::Suspended;
                r.await_id        = static_cast<int>(in.operand >> 8);
                r.await_is_module = in.op == Op::CallAsyncModule;
                r.await_args      = pop_args(static_cast<int>(in.operand & 0xFF));
                r.error_loc       = in.loc;   // where a failed await is reported
                return r;
            }

            case Op::SetIndex: {
                Value v   = pop();
                Value idx = pop();
                Value obj = pop();
                if (obj.is_list()) {
                    if (!idx.is_int())
                        return fail("a List index must be an int", in.loc);
                    long long i = idx.as_int();
                    auto& l = obj.as_list();
                    if (i < 0 || i >= (long long)l.size())
                        return fail("index out of range: " + std::to_string(i) +
                                    " (size " + std::to_string(l.size()) + ")", in.loc);
                    l[static_cast<size_t>(i)] = v;
                } else if (obj.is_dict()) {
                    if (!idx.is_str())
                        return fail("a Dict key must be a string", in.loc);
                    obj.as_dict()[idx.as_str()] = v;
                } else {
                    return fail(std::string("cannot index ") + obj.type_name(),
                                in.loc);
                }
                push(std::move(v));
                break;
            }

            case Op::SetMember: {
                Value v   = pop();
                Value obj = pop();
                const std::string& name = chunk.constants[in.operand].as_str();
                if (!obj.is_dict())
                    return fail(std::string("cannot assign '") + name +
                                "' on " + obj.type_name(), in.loc);
                // The Dict is shared by refcount, so the write is seen by anyone
                // holding the same value.
                obj.as_dict()[name] = v;
                push(std::move(v));
                break;
            }

            case Op::CallFunction: {
                size_t index = in.operand >> 8;
                int    argc  = static_cast<int>(in.operand & 0xFF);

                if (!functions_ || index >= functions_->size())
                    return fail("function not found", in.loc);
                if (frames_.size() >= kMaxFrames)
                    return fail("too much recursion: more than " +
                                std::to_string(kMaxFrames) + " nested calls",
                                in.loc);

                std::vector<Value> args = pop_args(argc);

                // Mixed mode for --native (Phase 2): if this function was
                // compiled to native code, it's called directly and no
                // interpreter frame at all is opened -- the result ends up
                // on the stack exactly as it would after a normal
                // Op::Return, so the rest of the bytecode that invoked it
                // can't tell one from the other. A NativeValue::Tag::Error
                // (division/modulo by zero, see
                // native_gen.cpp::lux_native_fail) turns into the same
                // fail() that the equivalent bytecode would already use.
                if (native_ && native_->funcs && index < native_->funcs->size() &&
                    (*native_->funcs)[index]) {
                    std::vector<NativeValue> nargs(args.size());
                    for (size_t i = 0; i < args.size(); ++i) nargs[i] = a_nativevalue(args[i]);
                    NativeValue r = (*native_->funcs)[index](nargs.data(),
                                                             static_cast<int32_t>(nargs.size()));
                    if (r.tag == NativeValue::Tag::Error) {
                        std::string msg = native_->error_message ? native_->error_message()
                                                                  : "native error";
                        return fail(std::move(msg), in.loc);
                    }
                    push(de_nativevalue(r));
                    break;
                }

                const Chunk& callee = *(*functions_)[index];

                Frame nf;
                nf.chunk       = &callee;
                nf.pc          = 0;
                nf.locals_base = locals_.size();
                nf.stack_base  = stack_.size();

                locals_.resize(locals_.size() +
                               static_cast<size_t>(callee.num_locals), Value::null());
                for (size_t i = 0; i < args.size(); ++i)
                    locals_[nf.locals_base + i] = std::move(args[i]);

                frames_.push_back(nf);
                break;
            }

            case Op::Return: case Op::ReturnNull: {
                Value  v     = in.op == Op::Return ? pop() : Value::null();
                size_t lbase = frame.locals_base, sbase = frame.stack_base;
                frames_.pop_back();
                if (frames_.empty()) {
                    Result r;
                    r.value = std::move(v);
                    return r;
                }
                stack_.resize(sbase);
                locals_.resize(lbase);
                push(std::move(v));
                break;
            }
        }
    }

    return Result{};
}

} // namespace lux_script
