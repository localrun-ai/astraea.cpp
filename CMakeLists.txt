cmake_minimum_required(VERSION 3.28)
project(astraea CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---------------------------------------------------------------------------
# Release: optimise for production.
#   -flto=thin                  Clang thin-LTO; cross-TU inlining + dead-strip
#
# Linux x86_64 (Ryzen AI 9 HX 370 / Zen 5 = znver5, ELF/ld.bfd):
#   -march=znver5               full AVX-512 + VNNI + BF16 available on this CPU
#   -fno-plt                    eliminate PLT indirection on every shared-lib call
#   -Wl,-z,now                  eager binding (required by -fno-plt), no lazy PLT
#   -Wl,-z,relro                full RELRO: GOT write-protected after startup
#   Use -march=native only for local benchmarking, never in a committed config.
#
# macOS arm64 (Apple Silicon, Mach-O/ld64):
#   -mcpu=apple-m4               tuned for M4 family; ld64 has no PLT/RELRO flags
#
# ASTRAEA_NATIVE_OPTS gates these so CI runs (Intel / generic x86-64) and
# any other machine can build Release without machine-specific codegen.
# ---------------------------------------------------------------------------
add_compile_options($<$<CONFIG:Release>:-O3>)

option(ASTRAEA_NATIVE_OPTS "Apply machine-specific codegen and -flto=thin in Release builds" ON)
if(ASTRAEA_NATIVE_OPTS)
    add_compile_options($<$<CONFIG:Release>:-flto=thin>)
    add_link_options($<$<CONFIG:Release>:-flto=thin>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        add_compile_options($<$<CONFIG:Release>:-mcpu=apple-m4>)
    else()
        add_compile_options($<$<CONFIG:Release>:-march=znver5>)
        add_compile_options($<$<CONFIG:Release>:-fno-plt>)
        add_link_options($<$<CONFIG:Release>:-Wl,-z,now,-z,relro>)
    endif()
endif()

# ---------------------------------------------------------------------------
# Debug: full sanitisers. -Werror is applied per-target (not globally) so
# it does not bleed into CPM-fetched third-party sources like Drogon.
# ---------------------------------------------------------------------------
add_compile_options($<$<CONFIG:Debug>:-O0>)
add_compile_options($<$<CONFIG:Debug>:-g>)
add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# spdlog compile-time level: Debug build emits every SPDLOG_DEBUG call,
# Release skips them entirely (zero runtime cost) at the cost of less
# verbose production logs. Runtime spdlog::set_level() still gates emission
# above the compile-time floor.
add_compile_definitions($<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>)

# Interface target: warnings + -Werror applied only to our own code.
# Using an INTERFACE target (rather than add_compile_options) prevents
# these flags from bleeding into CPM-fetched packages like Drogon whose
# sources trigger -Wunused-parameter / -Wunused-variable under -Wall.
#
# -Wno-missing-field-initializers is listed AFTER -Wextra here intentionally:
# PRIVATE target_compile_options are emitted before INTERFACE options from
# linked libs, so a suppression in jurisdictions/CMakeLists.txt would be
# overridden by -Wextra arriving later. Keeping both in one INTERFACE target
# guarantees the suppression wins. Route data uses partial designated init
# by design (unset fields default to empty/zero).
#
# Clang 21 splits out -Wmissing-designated-field-initializers from
# -Wmissing-field-initializers; probe before use so older CI compilers
# do not error on an unknown -Wno-* flag.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-missing-designated-field-initializers
    _WNO_MISSING_DESIGNATED_SUPPORTED)
add_library(astraea_warnings INTERFACE)
target_compile_options(astraea_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
    $<$<BOOL:${_WNO_MISSING_DESIGNATED_SUPPORTED}>:-Wno-missing-designated-field-initializers>
    $<$<CONFIG:Debug>:-Werror>
)

find_package(spdlog REQUIRED)

# Tests are opt-out so the CI parity job and any other "core only" build can
# skip Catch2 (which is not declared as a parity-job apt dep). astraea_core
# itself has no test framework dependency.
option(ASTRAEA_BUILD_TESTS "Build Catch2 unit tests under tests/" ON)
if(ASTRAEA_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
endif()

# RE2: prefer CMake config (vcpkg / upstream build) and fall back to pkg-config
# (Debian/Ubuntu libre2-dev ships pkg-config but omits the CMake config).
# Both paths produce the same re2::re2 target.
find_package(re2 CONFIG QUIET)
if(NOT re2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RE2 REQUIRED IMPORTED_TARGET re2)
    add_library(re2::re2 ALIAS PkgConfig::RE2)
endif()

# hiredis is only needed by the RedisCoordinator, which lives in astraea_clients.
# Gated on ASTRAEA_BUILD_CLIENTS so the parity job (clients=OFF) doesn't require
# libhiredis-dev installed. Same pattern as re2: CMake config preferred,
# pkg-config fallback.

# ---------------------------------------------------------------------------
# CPM.cmake — package manager for Drogon and future deps.
# ---------------------------------------------------------------------------
include(cmake/CPM.cmake)

# astraea_clients (and the Drogon + glaze fetches that back it) is opt-out so
# the CI parity job and any other "core only" build can skip the 5-minute
# Drogon clone+build. astraea_core itself has no Drogon dependency.
option(ASTRAEA_BUILD_CLIENTS "Build the HTTP client library and pull in Drogon + glaze" ON)

if(ASTRAEA_BUILD_CLIENTS)
    find_package(hiredis CONFIG QUIET)
    if(NOT hiredis_FOUND)
        # pkg_check_modules(... IMPORTED_TARGET) does not reliably propagate
        # its -L search path through a static-lib link chain (astraea_clients
        # -> final executable), producing a bare -lhiredis with no matching
        # -L and a link failure. find_library() resolves an absolute path
        # instead, sidestepping the propagation issue entirely.
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(HIREDIS REQUIRED hiredis)
        find_library(HIREDIS_LIBRARY NAMES hiredis HINTS ${HIREDIS_LIBRARY_DIRS})
        add_library(hiredis::hiredis INTERFACE IMPORTED)
        target_link_libraries(hiredis::hiredis INTERFACE ${HIREDIS_LIBRARY})
        target_include_directories(hiredis::hiredis INTERFACE ${HIREDIS_INCLUDE_DIRS})
    endif()

    CPMAddPackage(
        NAME drogon
        GITHUB_REPOSITORY drogonframework/drogon
        GIT_TAG v1.9.7
        OPTIONS
            "BUILD_EXAMPLES OFF"
            "BUILD_CTL OFF"
            "BUILD_TESTING OFF"
            "BUILD_DROGON_SHARED OFF"
            # astraea uses its own hiredis_runtime.cpp, never Drogon's bundled
            # nosql redis client. Leaving BUILD_REDIS at its default (ON) makes
            # Drogon link hiredis itself, which leaks a bare -lhiredis (no -L)
            # into every consumer's link line via static-lib propagation.
            "BUILD_REDIS OFF"
        # drogon v1.9.7's RequestStream.h uses std::exception_ptr without
        # including <exception>. libstdc++ (Linux) pulls it in transitively;
        # libc++ (macOS/Apple Clang) does not, so this fails outright there.
        # A CMake script (not sed) so it behaves identically on GNU sed
        # (Linux) and BSD sed (macOS) hosts, which disagree on \n handling.
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/cmake/patch_drogon_request_stream.cmake
    )
    # Mark Drogon as a SYSTEM library (CMake 3.25+) so its headers do not
    # trigger warnings in our TUs. Cleaner than enumerating include dirs.
    if(drogon_ADDED)
        set_property(TARGET drogon PROPERTY SYSTEM ON)
        # Suppress warnings in Drogon's own TUs (CT_APPLICATION_X_JAVASCRIPT
        # deprecated-declarations in HttpUtils.cc). SYSTEM ON only suppresses
        # warnings when we include Drogon headers; this covers Drogon's own
        # compilation units.
        target_compile_options(drogon PRIVATE -Wno-deprecated-declarations)
    endif()

    # glaze: header-only C++23 JSON serializer used by Phase 3 HTTP clients.
    CPMAddPackage(
        NAME glaze
        GITHUB_REPOSITORY stephenberry/glaze
        GIT_TAG v4.4.3
        OPTIONS "glaze_ENABLE_AVX2 OFF"
    )
endif()

# TODO(Phase5): wire mimalloc as process allocator once main.cpp exists.
# Linking into a static archive does not reliably override malloc/new —
# the override requires an explicit TU with <mimalloc-new-delete.h> compiled
# into each final executable, verified by mi_is_in_heap_region().

# Core library — routing + sanitize, zero network deps
add_library(astraea_core STATIC
  src/aho_corasick.cpp
  src/routing.cpp
  src/sanitize.cpp
  src/http_stream_parser.cpp
  src/route_validator_pure.cpp
)
target_include_directories(astraea_core PUBLIC include)
target_link_libraries(astraea_core PUBLIC spdlog::spdlog astraea_warnings)
# RE2 is used only inside src/sanitize.cpp — keep it private so consumers
# of astraea_core don't inherit the include path.
target_link_libraries(astraea_core PRIVATE re2::re2)
# Required so astraea_core can be linked into the pybind11 shared module.
set_target_properties(astraea_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ASTRAEA_BUILD_CLIENTS)
    # HTTP client + pipeline library.
    # Phase 3: embedder, retriever, generator, reranker (async HTTP via Drogon).
    # Phase 5: pipeline (RAGPipeline), anchor (legislation anchor + guidance).
    add_library(astraea_clients STATIC
      src/embedder.cpp
      src/retriever.cpp
      src/generator.cpp
      src/reranker.cpp
      src/pipeline.cpp
      src/anchor.cpp
      src/async_semaphore.cpp
      src/llm_stream.cpp
      src/llm_tcp_pool.cpp
      src/health.cpp
      src/redis_coordinator.cpp
      src/anthropic_client.cpp
      src/hiredis_runtime.cpp
      src/session.cpp
      src/feedback.cpp
      src/request_id.cpp
      src/route_validator_corpus.cpp
    )
    target_link_libraries(astraea_clients PUBLIC astraea_core drogon glaze::glaze astraea_warnings)
    target_link_libraries(astraea_clients PRIVATE hiredis::hiredis)
endif()

option(ASTRAEA_BUILD_BINDINGS "Build pybind11 Python bindings" ON)
if(ASTRAEA_BUILD_BINDINGS)
    add_subdirectory(bindings)
endif()

add_subdirectory(jurisdictions)

# Apps (jurisdiction binaries + spikes). Requires astraea_clients because every
# app pulls in Drogon + the HTTP client wrappers. Skip when clients are off
# (parity CI / lean builds).
option(ASTRAEA_BUILD_APPS "Build apps/ (hello_sse spike + jurisdiction binaries)" ON)
if(ASTRAEA_BUILD_APPS AND ASTRAEA_BUILD_CLIENTS)
    # mimalloc: process allocator for executables. Gated under BUILD_APPS so
    # the parity CI job (apps disabled via BUILD_CLIENTS=OFF) does not need
    # libmimalloc-dev. Each executable links a small TU including
    # <mimalloc-new-delete.h> for the global new/delete override.
    # Prefer CMake config (Gentoo / upstream / vcpkg); fall back to pkg-config
    # (Debian / Ubuntu libmimalloc-dev ships pkg-config but no CMake config).
    find_package(mimalloc CONFIG QUIET)
    if(NOT mimalloc_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(MIMALLOC REQUIRED IMPORTED_TARGET mimalloc)
        if(NOT TARGET mimalloc)
            add_library(mimalloc ALIAS PkgConfig::MIMALLOC)
        endif()
    endif()

    add_subdirectory(apps)
endif()

option(ASTRAEA_BUILD_BENCH "Build microbenchmarks (use prod preset only)" OFF)
if(ASTRAEA_BUILD_BENCH)
    add_subdirectory(tools)
endif()

if(ASTRAEA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
