#pragma once
#include <cstddef>
#include <string_view>
#include <sys/mman.h>

namespace lux {

// A request body too big for memory: spooled to a temp file while it arrived
// and mapped read-only. The pages are file-backed, so the kernel can drop
// them under pressure instead of holding the whole upload in RAM.
class MappedBody {
public:
    MappedBody(void* p, size_t n) : p_(p), n_(n) {}
    MappedBody(const MappedBody&) = delete;
    MappedBody& operator=(const MappedBody&) = delete;
    ~MappedBody() { if (p_) ::munmap(p_, n_); }
    std::string_view view() const { return {static_cast<const char*>(p_), n_}; }
private:
    void*  p_;
    size_t n_;
};

} // namespace lux
