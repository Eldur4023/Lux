#pragma once
// Time zones for the `time` and `rrule` modules.
//   Desktop: C++20 <chrono> time zone database (libstdc++), straight from the system's tzdata.
//   Android: the NDK's libc++ has no tzdb, so Howard Hinnant's date/tz library (third_party/date,
//            the IANA text database shipped with the app; src/android.cpp points it at it).
// Both expose the same time_zone API, so the modules only go through these names.
#include <chrono>
#include <string>

#ifdef __ANDROID__
#include <date/tz.h>
namespace lux_tz {
using time_zone  = date::time_zone;
using local_info = date::local_info;
template <class D> using local_time = date::local_time<D>;
inline const time_zone* locate(const std::string& id) { return date::locate_zone(id); }   // throws if unknown
}
#else
namespace lux_tz {
using time_zone  = std::chrono::time_zone;
using local_info = std::chrono::local_info;
template <class D> using local_time = std::chrono::local_time<D>;
inline const time_zone* locate(const std::string& id) { return std::chrono::get_tzdb().locate_zone(id); }
}
#endif

namespace lux_tz {
// The wall clock of a UTC instant in `tz`.
template <class D>
local_time<D> to_local(const time_zone* tz, std::chrono::sys_time<D> t) {
    return local_time<D>(t.time_since_epoch() + std::chrono::duration_cast<D>(tz->get_info(t).offset));
}
}
