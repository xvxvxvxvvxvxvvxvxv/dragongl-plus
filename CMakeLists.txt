cmake_minimum_required(VERSION 3.16)
# --- C compiler selection (L4) ----------------------------------------
# The compiler is a CHOICE, not a force: -DRAGONGL_USE_CLANG=ON (default)
# picks clang only if the user did not already choose one
# (-DCMAKE_C_COMPILER=... or $CC wins), and never for cross builds.
option(DRAGONGL_USE_CLANG "Default to clang as the C compiler (overridable)" ON)
if(DRAGONGL_USE_CLANG AND NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC}
   AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    find_program(DRAGONGL_CLANG clang)
    if(DRAGONGL_CLANG)
        set(CMAKE_C_COMPILER clang)
    endif()
endif()
project(DragonGL VERSION 2026.10.27.01 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- Options ------------------------------------------------------------
option(DRAGONGL_BUILD_CLIENT "Build the graphical client" ON)
option(DRAGONGL_BUILD_SERVER "Build the game server" ON)
option(DRAGONGL_SYSTEM_GLFW "Use the system GLFW (pkg-config) instead of third_party/glfw" OFF)
option(DRAGONGL_PORTABLE "Self-contained binaries (static runtime on Windows, embedded assets)" OFF)

if(WIN32)
    set(DRAGONGL_WINDOWS ON)
endif()

# --- Global definitions & warnings --------------------------------------
if(NOT WIN32)
    # strcasestr & friends are GNU extensions on glibc.
    add_compile_definitions(_GNU_SOURCE _DEFAULT_SOURCE)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wformat -Wformat-security
                        -Wno-unused-parameter -Wno-missing-field-initializers)
    if(NOT WIN32)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        add_compile_options(-fPIE -fstack-protector-strong)
        add_link_options(-pie -Wl,-z,relro,-z,now)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O2)
        if(NOT WIN32)
            add_compile_options(-D_FORTIFY_SOURCE=2)
        endif()
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g)
    endif()
endif()

if(WIN32)
    # The code base speaks POSIX: on Windows every translation unit gets the
    # compatibility layer first (sockets, mkdir/fsync/rename, rand...).
    # (applied per target below, so third-party libraries such as GLFW
    # are compiled exactly as upstream intends)
    set(DRAGONGL_FORCE_INCLUDE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/platform.h)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 _USE_MATH_DEFINES)
    if(DRAGONGL_PORTABLE OR MINGW)
        add_link_options(-static -static-libgcc)
    endif()
endif()

find_package(Threads REQUIRED)
set(COMMON_LIBS Threads::Threads)
if(NOT WIN32)
    list(APPEND COMMON_LIBS m)
endif()
if(WIN32)
    list(APPEND COMMON_LIBS ws2_32 bcrypt shell32)
endif()

include_directories(include src/shared)

set(SHARED_SRCS
    src/shared/map.c
    src/shared/game.c
    src/shared/net.c
    src/shared/rules.c
    src/shared/cJSON.c
    src/shared/data_loader.c
    src/shared/platform.c
)
set_source_files_properties(src/shared/cJSON.c PROPERTIES COMPILE_FLAGS "-Wno-keyword-macro")
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/shared/cJSON.c PROPERTIES COMPILE_FLAGS "")
endif()

# --- 1. Server ------------------------------------------------------------
if(DRAGONGL_BUILD_SERVER)
    set(SERVER_SRCS
        src/server/main_server.c
        src/server/server_net.c
        src/server/ai.c
        src/server/aoe.c
        src/server/combat_log.c
        src/server/pathfinding.c
        src/server/server_combat.c
        src/server/server_spawn.c
        src/server/server_world.c
        src/server/server_commands.c
        src/server/server_ui_sync.c
        src/server/spell_router.c
        src/server/tombstone.c
    )
    if(WIN32)
        list(APPEND SERVER_SRCS third_party/wepoll/wepoll.c)
    endif()
    add_executable(dragongl_server ${SERVER_SRCS} ${SHARED_SRCS})
    target_include_directories(dragongl_server PRIVATE src/server third_party/wepoll)
    target_link_libraries(dragongl_server ${COMMON_LIBS})
    target_compile_options(dragongl_server PRIVATE ${DRAGONGL_FORCE_INCLUDE})
endif()

# --- 2. Client ------------------------------------------------------------
if(DRAGONGL_BUILD_CLIENT)
    # GLFW: bundled static build (no runtime dependency; X11 + Wayland are
    # loaded dynamically by GLFW itself) unless the system one is requested.
    if(DRAGONGL_SYSTEM_GLFW)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW REQUIRED glfw3)
        set(GLFW_TARGET ${GLFW_LIBRARIES})
        set(GLFW_INC ${GLFW_INCLUDE_DIRS})
    else()
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
        set(GLFW_TARGET glfw)
        target_compile_options(glfw PRIVATE -w)
        set(GLFW_INC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw/include)
    endif()

    # Embedded assets: fonts and SPIR-V shaders become C arrays.
    set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${GEN_DIR})
    set(EMBED_SRCS "")
    function(dragongl_embed in name)
        set(out ${GEN_DIR}/${name}.c)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -DIN=${in} -DOUT=${out} -DNAME=${name}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS ${in} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
            COMMENT "Embedding ${name}")
        set(EMBED_SRCS ${EMBED_SRCS} ${out} PARENT_SCOPE)
    endfunction()
    dragongl_embed(${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/AlegreyaSans-Regular.ttf asset_font_body)
    dragongl_embed(${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/AlegreyaSans-Bold.ttf asset_font_bold)
    dragongl_embed(${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Cinzel.ttf asset_font_title)
    # window icon (raw RGBA, 64x64 and 32x32)
    dragongl_embed(${CMAKE_CURRENT_SOURCE_DIR}/res/icon64.rgba asset_icon64)
    dragongl_embed(${CMAKE_CURRENT_SOURCE_DIR}/res/icon32.rgba asset_icon32)

    # Vulkan shaders: compiled with glslc when available, otherwise the
    # pre-built SPIR-V in shaders/spv/ is used (so no Vulkan SDK is needed
    # to build the game).
    find_program(GLSLC_EXECUTABLE glslc)
    foreach(sh scene.vert scene.frag ui.vert ui.frag post.vert post.frag)
        string(REPLACE "." "_" shn ${sh})
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh})
        set(prebuilt ${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/${sh}.spv)
        if(GLSLC_EXECUTABLE)
            set(spv ${GEN_DIR}/${sh}.spv)
            add_custom_command(
                OUTPUT ${spv}
                COMMAND ${GLSLC_EXECUTABLE} -O ${src} -o ${spv}
                COMMAND ${CMAKE_COMMAND} -E copy ${spv} ${prebuilt}
                DEPENDS ${src}
                COMMENT "Compiling shader ${sh}")
        else()
            set(spv ${prebuilt})
        endif()
        dragongl_embed(${spv} asset_spv_${shn})
    endforeach()

    set(CLIENT_SRCS
        src/client/main_client.c
        src/client/app.c
        src/client/screens.c
        src/client/hud.c
        src/client/ui.c
        src/client/ui_icons.c
        src/client/scene.c
        src/client/scene_world.c
        src/client/scene_models.c
        src/client/fx.c
        src/client/mesh.c
        src/client/textures.c
        src/client/path.c
        src/client/gfx_gl.c
        src/client/gfx_vk.c
        src/client/audio.c
        src/client/music.c
        src/client/session.c
        src/client/settings.c
        src/client/net_thread.c
        src/client/client_state.c
        src/client/client_tome.c
        third_party/volk/volk.c
        third_party/miniaudio/miniaudio.c
    )
    # Third-party code: build quietly.
    set_source_files_properties(third_party/miniaudio/miniaudio.c third_party/volk/volk.c
        PROPERTIES COMPILE_OPTIONS "-w")
    if(WIN32)
        # executable icon + version information
        enable_language(RC)
        list(APPEND CLIENT_SRCS res/dragongl.rc)
    endif()
    add_executable(dragongl_client ${CLIENT_SRCS} ${SHARED_SRCS} ${EMBED_SRCS})
    target_include_directories(dragongl_client PRIVATE
        src/client
        ${GLFW_INC}
        third_party/stb
        third_party/volk
        third_party/vulkan
        third_party/miniaudio)
    # volk loads libvulkan / vulkan-1.dll at run time: no link dependency.
    target_compile_definitions(dragongl_client PRIVATE VK_NO_PROTOTYPES GLFW_INCLUDE_NONE
        # Only raw playback is used: the synthesizer generates every sound.
        MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER
        MA_NO_NODE_GRAPH MA_NO_ENGINE MA_NO_WAV MA_NO_MP3 MA_NO_FLAC)
    if(WIN32)
        target_compile_definitions(dragongl_client PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    endif()
    target_link_libraries(dragongl_client ${GLFW_TARGET} ${COMMON_LIBS})
    target_compile_options(dragongl_client PRIVATE ${DRAGONGL_FORCE_INCLUDE})
    if(NOT WIN32)
        target_link_libraries(dragongl_client ${CMAKE_DL_LIBS})
    else()
        # GUI subsystem: no console window behind the game.
        set_target_properties(dragongl_client PROPERTIES WIN32_EXECUTABLE ON)
        target_link_options(dragongl_client PRIVATE -mwindows)
    endif()
endif()

# --- Installation ---------------------------------------------------------
include(GNUInstallDirs)

if(DRAGONGL_BUILD_SERVER)
    install(TARGETS dragongl_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(DRAGONGL_BUILD_CLIENT)
    install(TARGETS dragongl_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(NOT WIN32)
    install(PROGRAMS dragongl-server dragongl-client dragongl_reset_world
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Game data (read-only JSON datasets). The mutable world state lives in
# the user's data/state dir, never in the package.
install(FILES data/bestiary.json data/items.json data/spells.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/data)

if(NOT WIN32)
    set(MAN_PAGES
        man/dragongl_server.1
        man/dragongl_client.1
        man/dragongl-server.1
        man/dragongl-client.1
        man/dragongl_reset_world.1
        man/analyze_balance.1
        man/generate_pdf_map.1
    )
    install(FILES ${MAN_PAGES} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

install(PROGRAMS tools/analyze_balance.py tools/generate_pdf_map.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools)

install(PROGRAMS tools/web/build.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools/web)
install(FILES
    tools/web/menu.html
    tools/web/bestiary.html
    tools/web/items.html
    tools/web/spells.html
    tools/web/README.md
    tools/web/_core.js
    tools/web/_design.css
    tools/web/_editor-shell.html
    tools/web/_mod-bestiary.js
    tools/web/_mod-items.js
    tools/web/_mod-spells.js
    tools/web/_menu.html
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools/web)
install(FILES third_party/fonts/OFL-Cinzel.txt third_party/fonts/OFL-AlegreyaSans.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/dragongl/fonts)
