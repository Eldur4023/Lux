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
#include <lux_script/builtin_module.hpp>
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
class RoomRegistry {
public:
    static RoomRegistry& instance() {
        static RoomRegistry r;
        return r;
    }

    void join(const std::string& room, std::weak_ptr<lux::detail::WSState> conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& members = rooms_[room];
        prune_locked(members);
        for (auto& m : members)
            if (same(m, conn)) return; // already a member
        members.push_back(std::move(conn));
    }

    bool leave(const std::string& room, const lux::detail::WSState* identity) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room);
        if (it == rooms_.end()) return false;
        auto& members = it->second;
        bool removed = false;
        for (size_t i = 0; i < members.size();) {
            auto sp = members[i].lock();
            if (!sp) { members.erase(members.begin() + static_cast<long>(i)); continue; }
            if (sp.get() == identity) {
                members.erase(members.begin() + static_cast<long>(i));
                removed = true;
                continue;
            }
            ++i;
        }
        if (members.empty()) rooms_.erase(it);
        return removed;
    }

    int leave_all(const lux::detail::WSState* identity) {
        std::lock_guard<std::mutex> lock(mutex_);
        int removed = 0;
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            auto& members = it->second;
            for (size_t i = 0; i < members.size();) {
                auto sp = members[i].lock();
                if (!sp) { members.erase(members.begin() + static_cast<long>(i)); continue; }
                if (sp.get() == identity) {
                    members.erase(members.begin() + static_cast<long>(i));
                    ++removed;
                    continue;
                }
                ++i;
            }
            if (members.empty()) it = rooms_.erase(it); else ++it;
        }
        return removed;
    }

    // Returns how many live members were actually reached (excluding
    // `exclude`, if non-null). "Reached" means the send was successfully
    // posted onto that member's own loop, not that it was delivered --
    // ws_send_threadsafe() never blocks waiting for that.
    int broadcast(const std::string& room, const std::string& message,
                 const lux::detail::WSState* exclude) {
        std::vector<std::weak_ptr<lux::detail::WSState>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = rooms_.find(room);
            if (it == rooms_.end()) return 0;
            prune_locked(it->second);
            if (it->second.empty()) { rooms_.erase(it); return 0; }
            targets = it->second; // copy out: never hold the lock while posting
        }
        int reached = 0;
        for (auto& t : targets) {
            auto sp = t.lock();
            if (!sp || (exclude && sp.get() == exclude)) continue;
            if (lux::ws_send_threadsafe(t, message)) ++reached;
        }
        return reached;
    }

    int count(const std::string& room) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room);
        if (it == rooms_.end()) return 0;
        prune_locked(it->second);
        return static_cast<int>(it->second.size());
    }

private:
    static bool same(const std::weak_ptr<lux::detail::WSState>& a,
                     const std::weak_ptr<lux::detail::WSState>& b) {
        auto sa = a.lock(), sb = b.lock();
        return sa && sb && sa.get() == sb.get();
    }

    static void prune_locked(std::vector<std::weak_ptr<lux::detail::WSState>>& members) {
        members.erase(
            std::remove_if(members.begin(), members.end(),
                           [](const std::weak_ptr<lux::detail::WSState>& w) { return w.expired(); }),
            members.end());
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::weak_ptr<lux::detail::WSState>>> rooms_;
};

Value fn_rooms_join(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "rooms.join() expects a room name"; return Value::null(); }
    if (!ctx.ws) { error = "rooms.join() can only be called from a ws route"; return Value::null(); }
    RoomRegistry::instance().join(args[0].as_str(), ctx.ws->weak_handle());
    return Value::boolean(true);
}

Value fn_rooms_leave(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "rooms.leave() expects a room name"; return Value::null(); }
    if (!ctx.ws) { error = "rooms.leave() can only be called from a ws route"; return Value::null(); }
    auto self = ctx.ws->weak_handle().lock();
    return Value::boolean(self && RoomRegistry::instance().leave(args[0].as_str(), self.get()));
}

Value fn_rooms_leave_all(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!ctx.ws) { error = "rooms.leave_all() can only be called from a ws route"; return Value::null(); }
    auto self = ctx.ws->weak_handle().lock();
    if (!self) return Value::integer(0);
    return Value::integer(RoomRegistry::instance().leave_all(self.get()));
}

Value fn_rooms_broadcast(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) {
        error = "rooms.broadcast() expects a room name and a message";
        return Value::null();
    }
    return Value::integer(
        RoomRegistry::instance().broadcast(args[0].as_str(), args[1].as_str(), nullptr));
}

// Same as broadcast(), minus the CALLING connection if it is a member --
// the common "echo to everyone else in the room" shape (the sender already
// knows what it sent; it does not need it echoed back). A no-op exclusion,
// not an error, when called outside a ws route: there is no "self" to
// leave out, so it behaves exactly like broadcast().
Value fn_rooms_broadcast_others(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) {
        error = "rooms.broadcast_others() expects a room name and a message";
        return Value::null();
    }
    std::shared_ptr<lux::detail::WSState> self;
    if (ctx.ws) self = ctx.ws->weak_handle().lock();
    return Value::integer(
        RoomRegistry::instance().broadcast(args[0].as_str(), args[1].as_str(), self.get()));
}

Value fn_rooms_count(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "rooms.count() expects a room name"; return Value::null(); }
    return Value::integer(RoomRegistry::instance().count(args[0].as_str()));
}

class RoomsModule : public BuiltinModule {
public:
    const char* name() const override { return "rooms"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"join",             1, 1, fn_rooms_join},
            {"leave",            1, 1, fn_rooms_leave},
            {"leave_all",        0, 0, fn_rooms_leave_all},
            {"broadcast",        2, 2, fn_rooms_broadcast},
            {"broadcast_others", 2, 2, fn_rooms_broadcast_others},
            {"count",            1, 1, fn_rooms_count},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(RoomsModule)

} // namespace lux_script
