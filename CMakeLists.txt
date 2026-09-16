cmake_minimum_required(VERSION 3.20)
project(lux CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --native (Fase 4, rutas): el .so que genera compilar_nativo() para una ruta
# enlaza liblux_script.a de verdad (necesita Value::write_json, no inline en
# el header) desde una biblioteca COMPARTIDA. Un
# .a compilado sin -fPIC no se puede meter dentro de un .so -- de ahi esto
# global, no solo en el target de lux_script: mas simple que marcar cada
# dependencia transitiva (lux, lux_llhttp) una por una, y el coste (PIC en
# el binario `lux` mismo, que no lo necesitaba) es insignificante frente a
# tener que recompilar bajo demanda.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Lux requires Linux: it uses epoll, sendfile(2) and SO_REUSEPORT.")
endif()

option(LUX_IO_URING "io_uring backend instead of epoll (Linux 5.1+)" OFF)
option(LUX_EXAMPLES "Build the examples in examples/"                 ON)

# Manual fuzzing harnesses -- see fuzz/chaos.hpp for why libFuzzer is not used
# (it needs clang; this toolchain is gcc).  Off by default: it is not part of the
# normal build or of ctest, it is launched by hand.
option(LUX_FUZZ "Build the fuzzing harnesses in fuzz/" OFF)

# Persistence modules.  Each one builds only if its client is present: `import
# sqlite` in a binary without it gives a clear error when compiling the .lux, not
# a strange failure in production.
option(LUX_SQLITE   "Modulo sqlite"                                   ON)
option(LUX_POSTGRES "Modulo postgres (libpq)"                         ON)
option(LUX_MYSQL    "Modulo mysql (libmysqlclient)"                   ON)
option(LUX_PDF      "Native module: pdf (cairo)"                      ON)
option(LUX_HTTP     "Native module: http (libcurl)"                   ON)

# ── System dependencies ───────────────────────────────────────────────────────
# Lux links neither OpenSSL, nghttp2 nor zlib: TLS, HTTP/2 and compression are
# the reverse proxy's business.  Only threads are needed.

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── jemalloc ──────────────────────────────────────────────────────────────────
# With several event loops and a database pool, glibc's malloc becomes the
# bottleneck: its arenas serialize and the mutex eats 20% of the time.  Measured
# on large JSON responses, 8 cores:
#
#   /articulos?size=100 (57 KB)   18.782 -> 29.802 req/s   +59%
#   /articulos?page=3   (11 KB)   49.051 -> 65.307 req/s   +33%
#
# It is optional: without it the build proceeds and glibc's is used.
option(LUX_JEMALLOC "Link jemalloc if available" ON)
set(LUX_ALLOC "")
if(LUX_JEMALLOC)
    find_library(JEMALLOC_LIB NAMES jemalloc)
    if(JEMALLOC_LIB)
        set(LUX_ALLOC ${JEMALLOC_LIB})
        message(STATUS "allocator: jemalloc (${JEMALLOC_LIB})")
    else()
        message(STATUS "allocator: glibc — install libjemalloc-dev for the real one")
    endif()
endif()

# No dependencies to fetch from the network: llhttp is vendored and everything
# else —JSON, templates, cryptography— is built in-house.
#
# ── third_party (vendored, no network during cmake) ───────────────────────────
add_library(lux_llhttp STATIC
    third_party/llhttp/src/api.c
    third_party/llhttp/src/http.c
    third_party/llhttp/src/llhttp.c
)
target_include_directories(lux_llhttp PUBLIC third_party/llhttp/include)
set_target_properties(lux_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── Main library ──────────────────────────────────────────────────────────────
set(LUX_SOURCES
    src/app.cpp
    src/router.cpp
    src/blocking_pool.cpp
    src/core/event_loop.cpp
    src/core/tcp_server.cpp
    src/http/http_parser.cpp
    src/http/http_connection.cpp
)

if(LUX_IO_URING)
    list(APPEND LUX_SOURCES src/core/io_uring_loop.cpp)
endif()

add_library(lux STATIC ${LUX_SOURCES})

target_include_directories(lux PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(lux PUBLIC
    lux_llhttp
    Threads::Threads
    ${LUX_ALLOC}
)

if(LUX_IO_URING)
    # io_uring_loop.cpp calls io_uring_setup/io_uring_enter via raw
    # syscall(__NR_io_uring_*, ...) (see its own comment: "Raw syscalls
    # only -- no liburing dependency"), confirmed by grep -- nothing here
    # includes or links liburing. No find_library/link needed.
    target_compile_definitions(lux PUBLIC LUX_IO_URING)
endif()

target_compile_options(lux PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── Native modules: drop-in discovery ───────────────────────────────────────
#
# Anything under src/lux_script/modules/base_modules/ (the officially shipped
# modules) or directly under src/lux_script/modules/ (a user's own, dropped
# next to that folder) that ends in .cpp gets compiled into lux_script with
# no other edit anywhere in the project -- see
# src/lux_script/modules/README.md and builtin_module.hpp's
# LUX_REGISTER_MODULE for the registration half of the mechanism this feeds.
# CONFIGURE_DEPENDS asks CMake to re-run this glob whenever the directory's
# contents might have changed, not only when CMakeLists.txt itself did --
# without it, "drop a file in" would need a CMakeLists.txt touch (even a
# no-op one) to be noticed, defeating the point.
file(GLOB LUX_BASE_MODULE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lux_script/modules/base_modules/*.cpp)
file(GLOB LUX_USER_MODULE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lux_script/modules/*.cpp)
set(LUX_MODULE_SOURCES ${LUX_BASE_MODULE_SOURCES} ${LUX_USER_MODULE_SOURCES})

# User-settable opt-out (`-DLUX_MODULE_EXCLUDE="path/a.cpp;path/b.cpp"`, paths
# relative to the repo root) -- see src/lux_script/modules/README.md.  Kept
# separate from the pdf/http exclusions appended below so setting it never
# clobbers what THIS file needs excluded for its own reasons.
set(LUX_MODULE_EXCLUDE "" CACHE STRING
    "Semicolon-separated .cpp paths (relative to the repo root) to skip from the automatic module build")

# pdf (cairo) and http (libcurl) cannot be compiled unconditionally -- the
# #include alone fails on a system without that library's -dev package.
# Pulled out of the glob result by name (not by skipping the whole
# base_modules/ glob) so every OTHER official module keeps being picked up
# automatically even though these two need special handling; each comes
# back further down via target_sources(), gated on its own dependency
# check succeeding, exactly as before this mechanism existed.
set(_lux_module_exclude
    ${LUX_MODULE_EXCLUDE}
    "src/lux_script/modules/base_modules/pdf.cpp"
    "src/lux_script/modules/base_modules/http.cpp")
foreach(_excluded ${_lux_module_exclude})
    list(REMOVE_ITEM LUX_MODULE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${_excluded})
endforeach()

# ── Lux Script: the language frontend ───────────────────────────────────────
add_library(lux_script STATIC
    src/lux_script/token.cpp
    src/lux_script/diagnostic.cpp
    src/lux_script/lexer.cpp
    src/lux_script/parser.cpp
    src/lux_script/crypto.cpp
    src/lux_script/auth.cpp
    src/lux_script/type.cpp
    src/lux_script/db.cpp
    src/lux_script/value.cpp
    src/lux_script/json_parse.cpp
    src/lux_script/template.cpp
    src/lux_script/natives.cpp
    src/lux_script/builtin_module.cpp
    src/lux_script/emitter.cpp
    src/lux_script/native_gen.cpp
    src/lux_script/native_build.cpp
    src/lux_script/vm.cpp
    src/lux_script/autotest.cpp
    src/lux_script/project.cpp
    ${LUX_MODULE_SOURCES}
)
target_include_directories(lux_script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lux_script PUBLIC lux ${CMAKE_DL_LIBS})

# Every module registers itself with a namespace-scope object that nothing
# else in the project references by symbol (LUX_REGISTER_MODULE,
# builtin_module.hpp) -- so the plain archive-member linking a STATIC
# library gets by default would silently discard an unreferenced module's
# .o file, registration and all, the moment nothing else in a binary calls
# into it directly. lux_link_script() replaces a plain
# `target_link_libraries(<target> PRIVATE lux_script)` wherever lux_script
# is linked, wrapping it in `--whole-archive`/`--no-whole-archive` so every
# member of THAT archive is kept regardless -- scoped to just this one
# archive, not `lux` or libc, by the matching --no-whole-archive right after.
function(lux_link_script target)
    target_link_libraries(${target} PRIVATE
        -Wl,--whole-archive
        lux_script
        -Wl,--no-whole-archive)
endfunction()

# --native (phase 4/5, routes): native_build.cpp invokes g++ as a subprocess
# to compile the .cpp a route generates -- it needs to know, at runtime of the
# `lux` binary, where the project's own headers are (for
# #include <lux/response.hpp>, <lux_script/value.hpp>...) and where the
# already-compiled liblux_script.a/liblux.a ended up. liblux_script is
# needed for Value::write_json (not inline); liblux for
# lux::core::EpollLoop::post -- needed by any route with `await`, because
# the Task<void> a coroutine generates schedules itself to self-destruct
# there once it finishes (see Task<void>::promise_type::FinalAwaitable in
# task.hpp) -- found in practice (undefined symbol loading the .so) testing
# the first route with `await sleep(...)`, not guessed beforehand.
# There is no way for an already-linked binary to guess these paths on its
# own: they are baked in here, at CMake's own compile time.
target_compile_definitions(lux_script PRIVATE
    LUX_NATIVE_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
    LUX_NATIVE_SCRIPT_LIB="$<TARGET_FILE:lux_script>"
    LUX_NATIVE_LIB="$<TARGET_FILE:lux>")

if(LUX_SQLITE)
    find_path(SQLITE3_INC sqlite3.h)
    find_library(SQLITE3_LIB sqlite3)
    if(SQLITE3_INC AND SQLITE3_LIB)
        target_sources(lux_script PRIVATE src/lux_script/db_sqlite.cpp)
        target_compile_definitions(lux_script PRIVATE LUX_SQLITE)
        target_include_directories(lux_script PRIVATE ${SQLITE3_INC})
        target_link_libraries(lux_script PRIVATE ${SQLITE3_LIB})
        message(STATUS "sqlite module: enabled")
    else()
        message(STATUS "sqlite module: SKIPPED (libsqlite3-dev missing)")
    endif()
endif()

if(LUX_POSTGRES)
    find_path(PQ_INC libpq-fe.h PATH_SUFFIXES postgresql)
    find_library(PQ_LIB pq)
    if(PQ_INC AND PQ_LIB)
        target_sources(lux_script PRIVATE src/lux_script/db_postgres.cpp)
        target_compile_definitions(lux_script PRIVATE LUX_POSTGRES)
        target_include_directories(lux_script PRIVATE ${PQ_INC})
        target_link_libraries(lux_script PRIVATE ${PQ_LIB})
        message(STATUS "postgres module: enabled")
    else()
        message(STATUS "postgres module: SKIPPED (libpq-dev missing)")
    endif()
endif()

if(LUX_MYSQL)
    find_path(MYSQL_INC mysql.h PATH_SUFFIXES mysql mariadb)
    find_library(MYSQL_LIB NAMES mysqlclient mariadb)
    if(MYSQL_INC AND MYSQL_LIB)
        target_sources(lux_script PRIVATE src/lux_script/db_mysql.cpp)
        target_compile_definitions(lux_script PRIVATE LUX_MYSQL)
        target_include_directories(lux_script PRIVATE ${MYSQL_INC})
        target_link_libraries(lux_script PRIVATE ${MYSQL_LIB})
        message(STATUS "mysql module: enabled")
    else()
        message(STATUS "mysql module: SKIPPED (libmysqlclient-dev missing)")
    endif()
endif()

# `pdf` (NATIVE-MODULES.md): built on cairo's PDF surface -- a well-established,
# already-liberally-licensed 2D graphics library with native PDF output, so
# there was no need to hunt for a PDF-specific dependency.
if(LUX_PDF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CAIRO QUIET cairo)
    endif()
    if(CAIRO_FOUND)
        target_sources(lux_script PRIVATE src/lux_script/modules/base_modules/pdf.cpp)
        target_compile_definitions(lux_script PRIVATE LUX_PDF)
        target_include_directories(lux_script PRIVATE ${CAIRO_INCLUDE_DIRS})
        target_link_libraries(lux_script PRIVATE ${CAIRO_LIBRARIES})
        # native_build.cpp compiles each --native .so with a fresh g++
        # invocation that links the WHOLE liblux_script.a (see
        # LUX_NATIVE_SCRIPT_LIB below) -- since pdf.cpp's object
        # file is in that archive and references cairo, that g++ invocation
        # needs cairo's link flags too, or loading ANY generated .so (not
        # just a route using `pdf`) fails with "undefined symbol:
        # cairo_pdf_surface_create_for_stream". Found by the native_route_
        # shadow suite immediately after enabling LUX_PDF, not guessed.
        list(JOIN CAIRO_LDFLAGS " " CAIRO_LDFLAGS_STR)
        target_compile_definitions(lux_script PRIVATE
            LUX_NATIVE_CAIRO_LIBS="${CAIRO_LDFLAGS_STR}")
        message(STATUS "pdf module: enabled")
    else()
        message(STATUS "pdf module: SKIPPED (libcairo2-dev missing)")
    endif()
endif()

# `http` (NATIVE-MODULES.md): outbound HTTP(S) client on libcurl -- an
# explicit, narrow exception to "Lux never links TLS" (see that comment
# at the top of this file): that principle is about not being an INBOUND
# TLS terminator, which does not and cannot extend to outbound calls (there
# is no reverse proxy between Lux and a third-party HTTPS API). libcurl
# brings a mature TLS backend rather than Lux hand-rolling one.
if(LUX_HTTP)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LUX_CURL QUIET libcurl)
    endif()
    if(LUX_CURL_FOUND)
        target_sources(lux_script PRIVATE src/lux_script/modules/base_modules/http.cpp)
        target_compile_definitions(lux_script PRIVATE LUX_HTTP)
        target_include_directories(lux_script PRIVATE ${LUX_CURL_INCLUDE_DIRS})
        target_link_libraries(lux_script PRIVATE ${LUX_CURL_LIBRARIES})
        # Same reason as LUX_NATIVE_CAIRO_LIBS just above: every --native
        # .so links the whole liblux_script.a, so http.cpp's curl
        # references need curl's link flags on EVERY --native build, not
        # only one that happens to use `http`.
        list(JOIN LUX_CURL_LDFLAGS " " LUX_CURL_LDFLAGS_STR)
        target_compile_definitions(lux_script PRIVATE
            LUX_NATIVE_CURL_LIBS="${LUX_CURL_LDFLAGS_STR}")
        message(STATUS "http module: enabled")
    else()
        message(STATUS "http module: SKIPPED (libcurl4-openssl-dev missing)")
    endif()
endif()
target_compile_options(lux_script PRIVATE -Wall -Wextra)

# The Lux binary.
add_executable(lux-bin src/lux_script/main.cpp)
set_target_properties(lux-bin PROPERTIES OUTPUT_NAME lux)
lux_link_script(lux-bin)
# --native (Fase 5, `await`): sin esto, el .so que dlopen() carga en tiempo
# de ejecucion NO comparte los `inline thread_local` de task.hpp
# (lux::detail::current_loop/current_token, que HttpConnection::dispatch()
# deja puestos antes de cada peticion) con este ejecutable -- cada uno se
# lleva su propia copia privada, siempre a null en la del .so. Sin loop,
# SleepAwaitable::await_suspend() resume el acto, sin esperar nada: `await
# sleep(200)` en una ruta nativa respondia en ~3ms en vez de ~200ms,
# encontrado probando la primera ruta con `await` de verdad contra HTTP, no
# adivinado de antemano. -rdynamic exporta los simbolos de este binario al
# tabla dinamica para que el .so resuelva contra ELLOS, no cree los suyos.
target_link_options(lux-bin PRIVATE -rdynamic)

# Frontend development tools.
add_executable(dump_tokens src/lux_script/tools/dump_tokens.cpp)
lux_link_script(dump_tokens)

add_executable(crypto_check src/lux_script/tools/crypto_check.cpp)
lux_link_script(crypto_check)

# ── Tests ─────────────────────────────────────────────────────────────────────
# The suite is a script that drives the binary over the socket, the way it is
# used.  It links nothing from the project, so it tests what gets deployed and
# not an instrumented version of it.
enable_testing()

# Templates are tested separately and in process: compiling and rendering do not
# go through the socket, so the regression script cannot reach them.
add_executable(test_templates tests/templates.cpp)
lux_link_script(test_templates)
add_test(NAME templates COMMAND test_templates)

# --native phase 1: compares Emitter::check_expr/
# check_call against the real compilation, expression by expression. It lives
# here, not in tests/run_tests.sh, because it calls the library directly
# instead of the binary: it is a test of an internal piece not wired up yet.
add_executable(test_check_expr_shadow tests/check_expr_shadow.cpp)
lux_link_script(test_check_expr_shadow)
add_test(NAME check_expr_shadow COMMAND test_check_expr_shadow)

add_executable(test_check_stmt_shadow tests/check_stmt_shadow.cpp)
lux_link_script(test_check_stmt_shadow)
add_test(NAME check_stmt_shadow COMMAND test_check_stmt_shadow)

# The other half of phase 1: not just that check_expr/check_stmt reproduce
# the same diagnostics (above), but that the emitter consuming the IR
# produces bytecode that BEHAVES the same when actually run on the VM.
add_executable(test_emit_ir_shadow tests/emit_ir_shadow.cpp)
lux_link_script(test_emit_ir_shadow)
add_test(NAME emit_ir_shadow COMMAND test_emit_ir_shadow)

# --native phase 2: generates real C++ from the IR, compiles it with the
# system compiler (invokes `g++` as a subprocess) and compares its execution
# against the VM -- the phase's acceptance criterion.
add_executable(test_native_gen_shadow tests/native_gen_shadow.cpp)
lux_link_script(test_native_gen_shadow)
add_test(NAME native_gen_shadow COMMAND test_native_gen_shadow)

# The other half: compile_native() for real (invokes g++, dlopen, dlsym) and
# dispatch inside the VM (VM::start with the native table wired up) give the
# same result as bytecode -- see the comment in tests/native_build_shadow.cpp.
add_executable(test_native_build_shadow tests/native_build_shadow.cpp)
lux_link_script(test_native_build_shadow)
add_test(NAME native_build_shadow COMMAND test_native_build_shadow)

# User classes: generates the C++ for a class and its methods directly
# (without going through compile_native(), which today has nothing to offer
# a class-only program -- see the comment in tests/native_class_shadow.cpp)
# and compares its execution against the VM.
add_executable(test_native_class_shadow tests/native_class_shadow.cpp)
lux_link_script(test_native_class_shadow)
add_test(NAME native_class_shadow COMMAND test_native_class_shadow)

# Phase 4: one HTTP route compiled to native, end to end, through the real
# compile() (parsing, build_routes, compile_native) -- not a Program built by
# hand -- compared against the same route served by bytecode. Needs g++ to
# find <lux/response.hpp> and link liblux_script.a: same macros
# native_build.cpp already uses (see the comment on LUX_NATIVE_INCLUDE_DIR
# on the lux_script target), so there is no need to repeat them here.
add_executable(test_native_route_shadow tests/native_route_shadow.cpp)
lux_link_script(test_native_route_shadow)
# Same reason as lux-bin above, and the same kind of symptom found the same
# way (by testing for real, not guessed beforehand): state.incr()/state.get()
# (phase 5.9) resolve against SharedState::instance(), a function-local
# `static` in natives.cpp -- without -rdynamic, the .so that compile_native()
# dlopen()s here carries its OWN copy (liblux_script.a linked again inside
# the .so), so a bytecode state.incr() and a --native one, in the SAME test,
# incremented DIFFERENT counters -- this test caught it (sequence 1,2,1,1,2
# instead of 1,2,3,3,3) before this comment was written.
target_link_options(test_native_route_shadow PRIVATE -rdynamic)
add_test(NAME native_route_shadow COMMAND test_native_route_shadow)

# The postgres placeholder translator is a pure function: it is tested without a
# server by including the driver's .cpp, to test THE code and not a copy.
#
# Deliberately NOT lux_link_script() here: this test #includes db_postgres.cpp
# directly (see placeholders.cpp), so its own translation unit already
# defines make_postgres_driver() -- a plain link then correctly skips pulling
# THAT archive member out of liblux_script.a a second time (nothing else in
# this test needs it). --whole-archive would force it in regardless and
# collide with the copy this test already carries, since this executable is
# not exercising BuiltinModuleRegistry/module self-registration at all.
if(LUX_POSTGRES AND PQ_INC AND PQ_LIB)
    add_executable(test_placeholders tests/placeholders.cpp)
    target_include_directories(test_placeholders PRIVATE ${PQ_INC})
    target_link_libraries(test_placeholders PRIVATE lux_script ${PQ_LIB})
    add_test(NAME placeholders COMMAND test_placeholders)
endif()

add_test(NAME regression
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:lux-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(regression PROPERTIES TIMEOUT 300)

# The mysql module needs a server, so its suite skips itself —code 77— when
# there is none, instead of turning a machine where it does not apply red.
# The instructions for setting the environment up are in the script's header.
add_test(NAME mysql
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mysql.sh $<TARGET_FILE:lux-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mysql PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# The sqlite type battery needs no server —the schema is created by the .lux
# itself— so it always runs.
add_test(NAME sqlite
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_sqlite.sh $<TARGET_FILE:lux-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(sqlite PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# Same for postgres.
add_test(NAME postgres
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_postgres.sh $<TARGET_FILE:lux-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(postgres PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# The `pdf` native module needs cairo (LUX_PDF) -- same skip-if-missing
# pattern as the DB modules above.
add_test(NAME pdf
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_pdf.sh $<TARGET_FILE:lux-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(pdf PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# The `http` native module needs libcurl (LUX_HTTP) -- same skip-if-missing
# pattern as pdf/sqlite/postgres/mysql above.
add_test(NAME http
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_http.sh $<TARGET_FILE:lux-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(http PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# ── Fuzzing ───────────────────────────────────────────────────────────────────
# No sanitizer flags here on purpose: for ASan/UBSan to be worth anything they
# have to instrument lux_script/lux too, not just these two .cpp files, so
# they go in CMAKE_CXX_FLAGS for the whole configuration -- see fuzz/README.md.
# Each case runs in a child (fuzz/chaos.hpp): a sanitizer abort takes down
# only that child, the campaign carries on.
if(LUX_FUZZ)
    add_executable(fuzz_language fuzz/fuzz_language.cpp)
    lux_link_script(fuzz_language)

    add_executable(fuzz_http fuzz/fuzz_http.cpp)
    target_include_directories(fuzz_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    lux_link_script(fuzz_http)
endif()

# ── Examples ──────────────────────────────────────────────────────────────────
if(LUX_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    file(GLOB LUX_EXAMPLE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
    foreach(src ${LUX_EXAMPLE_SRCS})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE lux)
    endforeach()
endif()
