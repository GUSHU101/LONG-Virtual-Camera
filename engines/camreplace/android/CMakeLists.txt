# Android-side native components for Camera Replace.
#
# Built with Android NDK r29 clang, targeting arm64-v8a, API 21+.

# Android Studio/SDK still distributes CMake 3.22 broadly. This project uses
# no newer language feature, so keep clean-checkout payload builds reproducible
# without requiring a separately installed host CMake.
cmake_minimum_required(VERSION 3.22)
project(camera_replace_android CXX C ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Base flags for EVERY target we own. Third-party libraries (shadowhook)
# do not inherit these — we don't use add_compile_options globally because it
# crushes their visibility=hidden + version-script machinery and leaves their
# shared library with only two exported symbols.
set(CR_BASE_CXX_FLAGS
    -Wall -Wextra
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections
    -fno-exceptions
    -fno-rtti
    -Os
    # OBF()'d format strings are runtime-decoded from compile-time
    # constants, but the compiler can't see through the template
    # decryption to verify the format. Suppress the warning rather
    # than fall back to plaintext for every LOGI/LOGE/printf line.
    -Wno-format-security
    -Wno-format-nonliteral
)
set(CR_BASE_LINK_FLAGS -Wl,--gc-sections -Wl,-s)

function(cr_apply_defaults tgt)
    target_compile_options(${tgt} PRIVATE ${CR_BASE_CXX_FLAGS})
    target_link_options   (${tgt} PRIVATE ${CR_BASE_LINK_FLAGS})
endfunction()

function(cr_enable_public_mode tgt)
    target_include_directories(${tgt} PRIVATE "${CMAKE_SOURCE_DIR}/../common")
endfunction()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)

# --- shadowhook (third-party inline-hook library) ---------------------------
# Upstream builds shadowhook as a SHARED library. We want it STATIC so our
# libcr_camhook.so doesn't depend on another .so at runtime — cameraserver's
# linker won't search /data/local/tmp for friends-of-friends.
#
# Build its sources directly into a static target here (mirrors upstream's
# CMake wiring but flipped to STATIC).
set(_SH_ROOT ${CMAKE_SOURCE_DIR}/../third_party/shadowhook/shadowhook/src/main/cpp)
file(GLOB _SH_SRC
    ${_SH_ROOT}/*.c
    ${_SH_ROOT}/arch/arm64/*.c
    ${_SH_ROOT}/arch/arm64/*.S
    ${_SH_ROOT}/common/*.c
    ${_SH_ROOT}/third_party/xdl/*.c
    ${_SH_ROOT}/third_party/bsd/*.c
    ${_SH_ROOT}/third_party/lss/*.c)
add_library(shadowhook_static STATIC ${_SH_SRC})
target_include_directories(shadowhook_static PUBLIC
    ${_SH_ROOT}
    ${_SH_ROOT}/include
    ${_SH_ROOT}/arch/arm64
    ${_SH_ROOT}/common
    ${_SH_ROOT}/third_party/xdl
    ${_SH_ROOT}/third_party/bsd
    ${_SH_ROOT}/third_party/lss)
target_link_libraries(shadowhook_static PUBLIC log)
target_compile_options(shadowhook_static PRIVATE -w)        # upstream uses -Weverything
set_target_properties(shadowhook_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_library(cr_secure_channel STATIC
    ${CMAKE_SOURCE_DIR}/../common/secure_channel/SecureSession.cpp
    ${CMAKE_SOURCE_DIR}/../common/secure_channel/LocalTicketAuth.cpp
    ${CMAKE_SOURCE_DIR}/../common/secure_channel/vendor/monocypher/monocypher.c
)
target_include_directories(cr_secure_channel PUBLIC
    ${CMAKE_SOURCE_DIR}/../common/secure_channel
    ${CMAKE_SOURCE_DIR}/../common
)
set_target_properties(cr_secure_channel PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(cr_secure_channel PUBLIC log)
cr_apply_defaults(cr_secure_channel)
cr_enable_public_mode(cr_secure_channel)
# SecureSession translates std::vector allocation failures into Status values
# at noexcept boundaries; its catch blocks require exceptions for this target.
target_compile_options(cr_secure_channel PRIVATE -fexceptions)

# --- cr_feed (static multicall payload) --------------------------------------
# Release host selects one authenticated H.264 video lane and one explicit
# opt-in PCM audio lane. The upstream raw receiver and opt-in diagnostic feed
# variants remain buildable for compatibility/regression work, but are not a
# second product engine or user-selectable runtime.
option(CR_FEED_ENABLE_RTMP_DIRECT
       "Build the legacy direct phone-side RTMP listener" OFF)
option(CR_FEED_USE_THREADED
       "Use the upstream experimental threaded tcp_h264 receiver" OFF)

set(_cr_feed_tcp_h264_src feed/src/tcp_h264_feed.cpp)
if(CR_FEED_USE_THREADED)
    set(_cr_feed_tcp_h264_src feed/src/tcp_h264_feed_v2.cpp)
endif()
set(_cr_feed_optional_src)
if(CR_FEED_ENABLE_RTMP_DIRECT)
    list(APPEND _cr_feed_optional_src feed/src/rtmp_h264_feed.cpp)
endif()

add_library(cr_feed STATIC
    feed/src/feed_main.cpp
    feed/src/secure_channel_reader.cpp
    ${_cr_feed_tcp_h264_src}
    feed/src/tcp_nv21_feed.cpp
    ${_cr_feed_optional_src}
    feed/src/tcp_pcm_feed.cpp
)
target_include_directories(cr_feed PUBLIC feed/include)
target_include_directories(cr_feed PRIVATE
    ${CMAKE_SOURCE_DIR}/../host/src
    ${CMAKE_SOURCE_DIR}/../common)
target_link_libraries(cr_feed PRIVATE cr_secure_channel log android mediandk)
set_target_properties(cr_feed PROPERTIES POSITION_INDEPENDENT_CODE ON)
cr_apply_defaults(cr_feed)
cr_enable_public_mode(cr_feed)
# std::vector / std::string in tcp_h264_feed need exceptions on (RTTI off).
target_compile_options(cr_feed PRIVATE -fexceptions)
if(CR_FEED_ENABLE_RTMP_DIRECT)
    target_compile_definitions(cr_feed PRIVATE CR_FEED_ENABLE_RTMP_DIRECT=1)
endif()

# --- cr_feed_proc (standalone) ----------------------------------------------
# A thin main() that dlopens? No — it links directly to cr_feed for simplicity,
# so the helper is self-contained. Rpath not needed since we don't load
# libcr_feed.so as SHARED at runtime for the helper (we link it), but we
# still build it as SHARED because cameraserver will dlopen it in sprint 4.
# --- cr_feed_proc executable ------------------------------------------------
add_executable(cr_feed_proc
    feed/src/cr_feed_proc.cpp
)
target_include_directories(cr_feed_proc PRIVATE
    ${CMAKE_SOURCE_DIR}/../host/src
    security
    feed/include)
cr_apply_defaults(cr_feed_proc)
cr_enable_public_mode(cr_feed_proc)
target_compile_options(cr_feed_proc PRIVATE -fexceptions)
target_link_libraries(cr_feed_proc PRIVATE cr_feed cr_secure_channel log android mediandk)

# --- cr_injector (PIE executable) -------------------------------------------
add_executable(cr_injector
    injector/src/main.cpp
    injector/src/ptrace_arm64.cpp
    injector/src/sepolicy.cpp
)
target_include_directories(cr_injector PRIVATE
    injector/src
    ${CMAKE_SOURCE_DIR}/../host/src
    security
    feed/include)
cr_apply_defaults(cr_injector)
cr_enable_public_mode(cr_injector)
# std::string inside StringPack needs exceptions enabled.
target_compile_options(cr_injector PRIVATE -fexceptions)
target_link_libraries(cr_injector PRIVATE cr_feed cr_secure_channel log android mediandk)

# --- libcr_hooks.so (camera + audio) -----------------------------------------
# Two scalers compiled in side-by-side; hook_main.cpp picks one at init
# time via the CR_BLIT_SCALER env (default = bilinear, set =lanczos for
# the sprint-E Lanczos2 path).
add_library(cr_hooks SHARED
    camhook/src/hook_main.cpp
    camhook/src/nv21_blit.c
    camhook/src/nv21_blit_lanczos.c
    camhook/src/jpeg_encode.c
    audhook/src/audhook_main.cpp
)
target_include_directories(cr_hooks PRIVATE
    ${CMAKE_SOURCE_DIR}/../common
    ${CMAKE_SOURCE_DIR}/../third_party/shadowhook/shadowhook/src/main/cpp/include
    ${CMAKE_SOURCE_DIR}/../host/src
    ${CMAKE_SOURCE_DIR}/security
    feed/include
    feed/src)
target_link_libraries(cr_hooks PRIVATE shadowhook_static cr_secure_channel log dl android)
set_target_properties(cr_hooks PROPERTIES OUTPUT_NAME cr_hooks)
cr_apply_defaults(cr_hooks)
cr_enable_public_mode(cr_hooks)
target_compile_options(cr_hooks PRIVATE -fexceptions)
# --- libcr_audhook.so --------------------------------------------------------
# Mic substitution: hooks StreamInHalHidl::read inside audioserver and
# overwrites the post-HAL buffer with PCM from /data/cr/audio.
# std::vector for the multi-hook stubs list — same reason cr_feed flips it.
