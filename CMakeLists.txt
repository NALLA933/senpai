cmake_minimum_required(VERSION 3.14)
project(anonx_cpp LANGUAGES CXX)

# --- Modern C++ ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- Dependencies ---
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)   # needs libsqlite3-dev; exposes SQLite::SQLite3

# nlohmann_json (header-only) — used only inside src/youtube.cpp.
# Prefer a system/vcpkg/conan package; otherwise fetch the release tarball.
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found locally; fetching v3.11.3")
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- Phase 1: data-layer library ---
add_library(anonx_data
  src/database.cpp
)
target_include_directories(anonx_data
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anonx_data
  PUBLIC SQLite::SQLite3 Threads::Threads)
target_compile_options(anonx_data PRIVATE -Wall -Wextra)

# --- Phase 2/3: core library (config, logging, app skeleton, YouTube) ---
add_library(anonx_core
  src/utils.cpp
  src/config.cpp
  src/logger.cpp
  src/app.cpp
  src/timer.cpp
  src/curl_cookie_source.cpp
  src/youtube.cpp
)
target_link_libraries(anonx_core
  PUBLIC anonx_data
  PRIVATE nlohmann_json::nlohmann_json)   # json used only in youtube.cpp
target_compile_options(anonx_core PRIVATE -Wall -Wextra)

# --- Real entrypoint ---
# Links the full Runtime when the Telegram layer is available (see the bottom of
# this file), and falls back to the data-layer skeleton otherwise.
add_executable(anonx src/main.cpp)
target_link_libraries(anonx PRIVATE anonx_core)
target_compile_options(anonx PRIVATE -Wall -Wextra)



# --- Phase 4 + integration: Telegram layer (TDLib JSON interface) ---
# The first four files are Phase 4 (transport, account, routing, assistants); the
# last three are the integration phase — the production BotApi, the MTProto half
# of joining a voice chat, and the object graph that ties every layer together.
set(ANONX_TG_SOURCES
  src/td_client.cpp
  src/telegram_client.cpp
  src/dispatcher.cpp
  src/userbot.cpp
  src/telegram_bot_api.cpp
  src/telegram_assistant_api.cpp
  src/voice_signaling.cpp
  src/runtime.cpp
)

# Production build: compile the Telegram layer against the real TDLib JSON
# library. Off by default so the core project builds without TDLib installed;
# enable with -DANONX_WITH_TDLIB=ON (needs TDLib's CMake package / Td::TdJson).
option(ANONX_WITH_TDLIB "Build the Telegram layer against a real TDLib (Td::TdJson)" OFF)
if(ANONX_WITH_TDLIB)
  find_package(Td REQUIRED)   # provides the imported target Td::TdJson
  add_library(anonx_tg ${ANONX_TG_SOURCES})
  target_link_libraries(anonx_tg
    PUBLIC anonx_core Td::TdJson
    PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(anonx_tg PRIVATE -Wall -Wextra)
  message(STATUS "ANONX_WITH_TDLIB=ON — building anonx_tg against Td::TdJson")
endif()



# --- Phase 5: voice + queue layer ---
# The queue (include/anonx/queue.hpp) and the VoiceTransport interface
# (include/anonx/voice_transport.hpp) are header-only; only the CallManager
# orchestration needs compiling. This library is engine-agnostic — it talks to
# the abstract VoiceTransport, so it links no voice/WebRTC dependency, and it
# uses only the public headers (not even the SQLite data layer), keeping the
# voice logic and its test lightweight.
add_library(anonx_voice
  src/call_manager.cpp
)
target_include_directories(anonx_voice
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anonx_voice PUBLIC Threads::Threads)
target_compile_options(anonx_voice PRIVATE -Wall -Wextra)

# Production build: the real NTgCalls-backed transport. Off by default so the
# project builds without NTgCalls/WebRTC installed; enable with
# -DANONX_WITH_NTGCALLS=ON. The NTgCalls headers are confined to
# ntgcalls_transport.cpp (PIMPL), so only this target pulls the dependency.
# (The CMake target name for NTgCalls varies by install method; adjust the
# find_package/target below to match your NTgCalls build if needed.)
option(ANONX_WITH_NTGCALLS "Build the real NTgCalls voice transport" OFF)
if(ANONX_WITH_NTGCALLS)
  find_package(ntgcalls REQUIRED)   # provides an imported NTgCalls target
  add_library(anonx_voice_ntgcalls src/ntgcalls_transport.cpp)
  target_compile_definitions(anonx_voice_ntgcalls PUBLIC ANONX_WITH_NTGCALLS)
  target_link_libraries(anonx_voice_ntgcalls
    PUBLIC anonx_voice ntgcalls)
  target_compile_options(anonx_voice_ntgcalls PRIVATE -Wall -Wextra)
  message(STATUS "ANONX_WITH_NTGCALLS=ON — building anonx_voice_ntgcalls against NTgCalls")
endif()



# --- Phase 6a/6b: command plugins ---
# The language tables, inline keyboards, permission guards, the playback command
# handlers (6a) and everything that is not playback — auth, blacklist,
# broadcast, sudoers, the inline menus and the host-metric cards (6b). Like the
# voice layer, this library is transport-agnostic: it talks to the abstract
# BotApi (include/anonx/bot_api.hpp) rather than to TDLib, so it links no
# Telegram dependency and its tests run fully offline.
add_library(anonx_plugins
  src/lang.cpp
  src/buttons.cpp
  src/guards.cpp
  src/plugins.cpp
  src/sysinfo.cpp
  src/admin_plugins.cpp
  src/ffmpeg_thumbnail_renderer.cpp
)
target_link_libraries(anonx_plugins
  PUBLIC anonx_core anonx_voice          # Database/Config/YouTube + CallManager
  PRIVATE nlohmann_json::nlohmann_json)  # json used only in lang.cpp
target_compile_options(anonx_plugins PRIVATE -Wall -Wextra)

# src/plugins_router.cpp is the ONLY Phase 6a/6b file that touches the
# dispatcher, so it is compiled into the Telegram targets instead of the library
# above — that is what keeps TDLib out of anonx_plugins and out of its tests.
if(ANONX_WITH_TDLIB)
  target_sources(anonx_tg PRIVATE src/plugins_router.cpp)
  target_link_libraries(anonx_tg PUBLIC anonx_plugins)

  # --- Integration: the real bot binary ---
  # ANONX_WITH_TDLIB switches src/main.cpp from the data-layer skeleton to the
  # full Runtime. Voice is a second, independent switch: with NTgCalls the bot
  # streams for real, without it NullVoiceTransport keeps every other command
  # working (see include/anonx/null_voice_transport.hpp).
  target_compile_definitions(anonx PRIVATE ANONX_WITH_TDLIB)
  target_link_libraries(anonx PRIVATE anonx_tg)
  if(ANONX_WITH_NTGCALLS)
    target_link_libraries(anonx PRIVATE anonx_voice_ntgcalls)
    message(STATUS "anonx: full build — TDLib + NTgCalls")
  else()
    message(STATUS "anonx: TDLib without NTgCalls — streaming disabled")
  endif()
endif()


