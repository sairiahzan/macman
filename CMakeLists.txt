# ============================================================================
#  CMakeLists.txt — Macman Build Configuration
# ============================================================================
#  Builds the macman package manager binary with C++17, libcurl, and
#  bundled nlohmann/json. Installs to /usr/local/bin/.
# ============================================================================

cmake_minimum_required(VERSION 3.16)
project(macman VERSION 1.0.0 LANGUAGES CXX)

# ── C++ Standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler Flags ───────────────────────────────────────────────────────────
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# ── Find Dependencies ───────────────────────────────────────────────────────
find_package(CURL REQUIRED)

# ── Include Directories ─────────────────────────────────────────────────────
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party
    ${CURL_INCLUDE_DIRS}
)

# ── Source Files ─────────────────────────────────────────────────────────────
set(MACMAN_SOURCES
    # Entry point
    main.cpp

    # CLI
    src/cli/argument_parser.cpp

    # Core
    src/core/package.cpp
    src/core/config.cpp
    src/core/database.cpp
    src/core/transaction.cpp

    # Network
    src/net/http_client.cpp
    src/net/downloader.cpp

    # Backend
    src/backend/homebrew_backend.cpp
    src/backend/aur_backend.cpp

    # UI
    src/ui/progress_bar.cpp
)

# ── Build Executable ─────────────────────────────────────────────────────────
add_executable(macman ${MACMAN_SOURCES})

# ── Link Libraries ──────────────────────────────────────────────────────────
target_link_libraries(macman PRIVATE ${CURL_LIBRARIES})

# ── macOS Frameworks ─────────────────────────────────────────────────────────
if(APPLE)
    target_link_libraries(macman PRIVATE "-framework Security" "-framework SystemConfiguration")
endif()

# ── Install Target ──────────────────────────────────────────────────────────
install(TARGETS macman DESTINATION bin)

# Create required directories on install
install(DIRECTORY DESTINATION /usr/local/var/macman)
install(DIRECTORY DESTINATION /usr/local/var/macman/sync)
install(DIRECTORY DESTINATION /usr/local/var/cache/macman)
install(DIRECTORY DESTINATION /usr/local/etc)
