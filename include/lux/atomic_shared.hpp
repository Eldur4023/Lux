#pragma once
// std::atomic<std::shared_ptr<T>> where the standard library has it (libstdc++); on libc++ (the Android NDK)
// the same load()/store() over the atomic_load/atomic_store free functions.
#include <atomic>
#include <memory>
#include <version>

namespace lux {

#if defined(__cpp_lib_atomic_shared_ptr)
template <class T> using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;
#else
template <class T> class atomic_shared_ptr {
public:
    atomic_shared_ptr() = default;
    atomic_shared_ptr(std::shared_ptr<T> p) : p_(std::move(p)) {}
    std::shared_ptr<T> load() const { return std::atomic_load(&p_); }
    void store(std::shared_ptr<T> p) { std::atomic_store(&p_, std::move(p)); }
private:
    std::shared_ptr<T> p_;
};
#endif

} // namespace lux
