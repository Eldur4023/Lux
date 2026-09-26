// Cross-connection WebSocket broadcast (NATIVE-MODULES.md): join a named
// "room", leave it, and send a message to everyone currently in it.
// LuxScript's `ws` object (ws.send/ws.recv, GUIDE.md §14) is deliberately
// per-connection only, and state.* (GUIDE.md §15) cannot hold a live
// connection at all -- it stores lux_script::Value, and Value has no
// "native handle" case -- so a multi-client feature ("watch together", a
// chat room, live presence) had no path to a solution without dropping
// into C++. This module is that path, kept as a normal drop-in module
// (src/lux_script/modules/README.md) rather than a core-language feature:
// a room is just a string, membership is opt-in per connection, and none
// of that needs to be baked into the grammar.
//
// The one piece of this that could not be "just call ws.send() from
// another handler": a WSConnection's actual I/O has no locking, by design
// -- every connection is only ever touched by the ONE event-loop thread
// that owns it. A broadcast's whole point is reaching connections owned by
// OTHER threads, so every actual send here goes through
// lux::ws_send_threadsafe() (websocket.hpp), which posts the write onto
// the target's own loop instead of touching it directly -- read that
// function's comment before changing anything here that sends bytes.
//
// An `sse` route can join a room too: a broadcast reaches it as a `data:`
// event. Its members hold their own SSEWriter over the connection (the
// handler's lives inside the handler), written only on the connection's
// own loop, the same hop as ws_send_threadsafe().
#include <lux_script/builtin_module.hpp>
#include <lux/sse.hpp>
#include <lux/websocket.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lux_script {

namespace {

// One entry per (room, connection) pair currently believed to be a member.
// Pruned lazily -- a dead weak_ptr is only noticed and dropped the next
// time that room is iterated (join/leave/broadcast/count all iterate it),
// never proactively on disconnect. CancellationToken (cancel.hpp) has
// exactly one wake-callback slot, already claimed by ws.recv()'s
// RecvAwaitable; hooking a second cleanup callback onto it would race
// whichever of the two registered last, so this deliberately does not try.
// A quiet room can hold a few stale entries until the next join/broadcast
// touches it -- each is a weak_ptr, not the connection itself, so the cost
// is a few dozen bytes, not a leaked connection.
struct SseSub {
    lux::core::EventLoop*                   loop;
    std::shared_ptr<lux::CancellationToken> token;
    std::shared_ptr<lux::SSEWriter>         writer;
};

// A ws connection or an sse stream. `who` tells a member apart for leave()
// and broadcast_others(): the WSState, or the stream's cancel token.
struct Member {
    std::weak_ptr<lux::detail::WSState> ws;
    std::shared_ptr<SseSub>             sse;
    const void*                         who = nullptr;

    bool alive() const { return sse ? !sse->token->is_cancelled() : !ws.expired(); }
    bool send(const std::string& text) const {
        if (!sse) return lux::ws_send_threadsafe(ws, text);
        if (!alive() || !sse->loop) return false;
        sse->loop->post([s = sse, text] { if (!s->token->is_cancelled()) s->writer->send(text); });
        return true;
    }
};
using Members = std::vector<Member>;

class RoomRegistry {
public:
    static RoomRegistry& instance() {
        static RoomRegistry r;
        return r;
    }

    void join(const std::string& room, Member conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& members = rooms_[room];
        remove_locked(members, conn.who);   // joining twice is still one membership
        members.push_back(std::move(conn));
    }

    // How many rooms `identity` left: one (or zero) for a named room, every
    // room it was in for leave_all (room == nullptr).
    int leave(const std::string* room, const void* identity) {
        std::lock_guard<std::mutex> lock(mutex_);
        int removed = 0;
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            if (!room || it->first == *room) removed += remove_locked(it->second, identity) > 0;
            it = it->second.empty() ? rooms_.erase(it) : std::next(it);
        }
        return removed;
    }

    // Members reached, `exclude` left out. "Reached" is "posted onto that
    // member's own loop", never a blocking wait for delivery.
    int broadcast(const std::string& room, const std::string& message, const void* exclude) {
        Members targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = rooms_.find(room);
            if (it == rooms_.end()) return 0;
            remove_locked(it->second, nullptr);
            targets = it->second;   // copied out: never post while holding the lock
        }
        int reached = 0;
        for (const auto& t : targets)
            if (t.who != exclude && t.send(message)) ++reached;
        return reached;
    }

    int count(const std::string& room) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room);
        if (it == rooms_.end()) return 0;
        remove_locked(it->second, nullptr);
        return static_cast<int>(it->second.size());
    }

private:
    // Drops `identity` and every dead connection; returns how many entries
    // were `identity`.
    static int remove_locked(Members& members, const void* identity) {
        int hits = 0;
        std::erase_if(members, [&](const Member& m) {
            const bool alive = m.alive();
            if (alive && identity && m.who == identity) { ++hits; return true; }
            return !alive;
        });
        return hits;
    }

    std::mutex                               mutex_;
    std::unordered_map<std::string, Members> rooms_;
};

// The calling connection's identity (see Member::who), or an error outside
// a ws or sse route.
const void* self(NativeCtx& ctx, const char* fn, std::string& error) {
    if (ctx.ws) return ctx.ws->weak_handle().lock().get();
    if (ctx.sse) return ctx.req.cancel_token.get();
    error = std::string("rooms.") + fn + "() can only be called from a ws or sse route";
    return nullptr;
}

// A Dict or List goes out as JSON; a string as is.
std::string message(const Value& v) { return v.is_str() ? v.as_str() : v.to_json_text(); }

Value fn_rooms_join(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    const void* me = self(ctx, "join", error);
    if (!error.empty()) return Value::null();
    Member m;
    m.who = me;
    if (ctx.ws) m.ws = ctx.ws->weak_handle();
    else m.sse = std::make_shared<SseSub>(SseSub{ctx.req.loop, ctx.req.cancel_token,
        std::make_shared<lux::SSEWriter>(ctx.req._raw_write, ctx.req.cancel_token, ctx.req._force_close)});
    RoomRegistry::instance().join(args[0].as_str(), std::move(m));
    return Value::boolean(true);
}

Value fn_rooms_leave(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    const void* me = self(ctx, "leave", error);
    return error.empty() ? Value::boolean(me && RoomRegistry::instance().leave(&args[0].as_str(), me))
                         : Value::null();
}

Value fn_rooms_leave_all(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    const void* me = self(ctx, "leave_all", error);
    return error.empty() ? Value::integer(me ? RoomRegistry::instance().leave(nullptr, me) : 0)
                         : Value::null();
}

Value fn_rooms_broadcast(NativeCtx&, std::vector<Value>& args, std::string&) {
    return Value::integer(RoomRegistry::instance().broadcast(args[0].as_str(), message(args[1]), nullptr));
}

// broadcast() minus the calling connection -- "everyone else in the room".
// Outside a ws or sse route there is no one to leave out, so it is broadcast().
Value fn_rooms_broadcast_others(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    std::string unused;
    const void* me = ctx.ws || ctx.sse ? self(ctx, "broadcast_others", unused) : nullptr;
    return Value::integer(RoomRegistry::instance().broadcast(args[0].as_str(), message(args[1]), me));
}

Value fn_rooms_count(NativeCtx&, std::vector<Value>& args, std::string&) {
    return Value::integer(RoomRegistry::instance().count(args[0].as_str()));
}

} // namespace

LUX_MODULE(rooms, {
    {"join",             "s>b",  fn_rooms_join},
    {"leave",            "s>b",  fn_rooms_leave},
    {"leave_all",        ">i",   fn_rooms_leave_all},
    {"broadcast",        "sx>i", fn_rooms_broadcast},
    {"broadcast_others", "sx>i", fn_rooms_broadcast_others},
    {"count",            "s>i",  fn_rooms_count},
})

} // namespace lux_script
