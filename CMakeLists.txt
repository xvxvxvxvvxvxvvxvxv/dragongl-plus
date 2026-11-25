# Dragon GL - Copyright (C) 2026 Nicola Taibi
# DragonGL+ - fork of Dragon GL
# SPDX-License-Identifier: GPL-3.0-or-later
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
include("${CMAKE_CURRENT_LIST_DIR}/cmake/DragonGL.cmake")
project(DragonGL VERSION ${DRAGONGL_VERSION} LANGUAGES C)
if(WIN32)
    # -z relro/now and PIE are ELF options
    set(DRAGONGL_HARDENING OFF CACHE BOOL "Add hardening flags (off for distribution builds)" FORCE)
endif()

# What to build. A headless server host needs neither Vulkan headers nor
# GLFW: -DDRAGONGL_BUILD_CLIENT=OFF.
option(DRAGONGL_BUILD_SERVER "Build the game server (dragongl_server)" ON)
option(DRAGONGL_BUILD_CLIENT "Build the graphical client (dragongl_client)" ON)
option(DRAGONGL_SYSTEM_GLFW "Use the system GLFW (pkg-config) instead of third_party/glfw" OFF)
option(DRAGONGL_PORTABLE "Self-contained binaries (static libraries, embedded assets)" OFF)
option(DRAGONGL_INSTALL_SYSTEMD_UNIT "Install the systemd unit of the server" ON)
set(DRAGONGL_SYSTEMD_UNITDIR "lib/systemd/system" CACHE PATH
    "Where the systemd unit goes (relative to the prefix, or absolute)")
# The firewalld service of the server (port 8080/tcp): installed only when
# a directory is given, since firewalld reads /usr/lib/firewalld/services
# only (the spec passes %{_prefix}/lib/firewalld/services).
set(DRAGONGL_FIREWALLD_SERVICEDIR "" CACHE PATH
    "Where the firewalld service of the server goes (empty: not installed)")
if(NOT DRAGONGL_BUILD_SERVER AND NOT DRAGONGL_BUILD_CLIENT)
    message(FATAL_ERROR "DRAGONGL_BUILD_SERVER and DRAGONGL_BUILD_CLIENT are both OFF: nothing to build")
endif()

dragongl_compile_settings()
include(GNUInstallDirs)
set(DRAGONGL_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
dragongl_generate_config("${DRAGONGL_GENERATED_DIR}")
message(STATUS "Dragon GL ${DRAGONGL_VERSION} (server: ${DRAGONGL_BUILD_SERVER}, client: ${DRAGONGL_BUILD_CLIENT})")

if(WIN32)
    # The code base speaks POSIX: on Windows every translation unit of the
    # game gets the compatibility layer first (sockets, files, locks...),
    # applied per target so third-party libraries build as their authors
    # intend. src/shared/compat_win32 stands in for the POSIX headers
    # Windows lacks; platform_win32.h maps the calls (wepoll for epoll).
    set(DRAGONGL_FORCE_INCLUDE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/platform.h)
    include_directories(src/shared/compat_win32 third_party/wepoll)
    set(DRAGONGL_WIN32_SRCS src/shared/platform_win32.c third_party/wepoll/wepoll.c)
    # These call Winsock and the CRT directly: no POSIX macros.
    set_source_files_properties(src/shared/platform.c src/shared/platform_win32.c
        third_party/miniaudio/miniaudio.c third_party/volk/volk.c
        PROPERTIES COMPILE_DEFINITIONS DRAGONGL_NO_POSIX_SHIM)
    set_source_files_properties(third_party/wepoll/wepoll.c PROPERTIES
        COMPILE_DEFINITIONS "DRAGONGL_NO_POSIX_SHIM;DRAGONGL_WEPOLL_SOURCE" COMPILE_OPTIONS "-w")
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 _USE_MATH_DEFINES)
    if(DRAGONGL_PORTABLE OR MINGW)
        add_link_options(-static -static-libgcc)
    endif()
endif()
# Static OpenSSL and libsodium of the portable packages, built by
# tools/build_deps.sh into this prefix (lib/, include/, lib/pkgconfig/).
set(DRAGONGL_DEPS_PREFIX "" CACHE PATH "Prefix of the static OpenSSL and libsodium (tools/build_deps.sh)")
if(DRAGONGL_DEPS_PREFIX)
    list(APPEND CMAKE_FIND_ROOT_PATH "${DRAGONGL_DEPS_PREFIX}")
    set(OPENSSL_ROOT_DIR "${DRAGONGL_DEPS_PREFIX}")
    set(ENV{PKG_CONFIG_LIBDIR} "${DRAGONGL_DEPS_PREFIX}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "")
endif()
if(DRAGONGL_PORTABLE)
    set(OPENSSL_USE_STATIC_LIBS ON)
endif()

# --- Dependencies -----------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
# OpenSSL 3: TLS 1.3 between client and server (include/tls_common.h)
find_package(OpenSSL 3.0 REQUIRED)
if(DRAGONGL_BUILD_SERVER)
    # libsodium: Argon2id password hashing and constant-time comparisons
    # (with DRAGONGL_DEPS_PREFIX only the static library is there)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
    if(WIN32 AND DRAGONGL_DEPS_PREFIX)
        add_compile_definitions(SODIUM_STATIC) # no dllimport in sodium.h
    endif()
endif()

include_directories(include src/shared "${DRAGONGL_GENERATED_DIR}")

# --- Targets ----------------------------------------------------------------------
set(COMMON_LIBS Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    list(APPEND COMMON_LIBS ws2_32 crypt32 gdi32 bcrypt advapi32 shell32 user32)
else()
    list(APPEND COMMON_LIBS m)
endif()

# Source lists: cmake/DragonGL.cmake, plus the portability layer of DragonGL+
list(APPEND DRAGONGL_SHARED_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/platform.c")
list(APPEND DRAGONGL_CLIENT_SHARED_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/platform.c")

if(DRAGONGL_BUILD_SERVER)
    set(DRAGONGL_SERVER_EXTRA ${DRAGONGL_WIN32_SRCS})
    add_executable(dragongl_server ${DRAGONGL_SERVER_SRCS} ${DRAGONGL_SHARED_SRCS} ${DRAGONGL_SERVER_EXTRA})
    target_include_directories(dragongl_server PRIVATE src/server third_party/wepoll)
    target_link_libraries(dragongl_server ${COMMON_LIBS} PkgConfig::SODIUM ${DRAGONGL_CJSON_LIBS})
    target_compile_options(dragongl_server PRIVATE ${DRAGONGL_FORCE_INCLUDE})
endif()

if(DRAGONGL_BUILD_CLIENT)
    # GLFW: bundled static build (no runtime dependency; X11 is loaded
    # dynamically by GLFW itself) unless the system one is requested.
    if(DRAGONGL_SYSTEM_GLFW)
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
    set(EMBED_SRCS "")
    function(dragongl_embed in name)
        set(out ${DRAGONGL_GENERATED_DIR}/${name}.c)
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

    # Shaders: shaders/*.vert|frag are the only shader sources of both
    # renderers. Vulkan uses SPIR-V compiled by glslc when it is installed,
    # otherwise the pre-built copies in shaders/spv/ (no Vulkan SDK needed
    # to build the game); OpenGL compiles the GLSL text, embedded as is.
    # shaders/spv/SOURCES.sha256 records which sources the SPIR-V was built
    # from: without glslc, edited shaders and stale SPIR-V stop the build.
    set(SHADER_STAGES scene.vert scene.frag ui.vert ui.frag fullscreen.vert
        bloom_down.frag bloom_up.frag composite.frag fxaa.frag)
    set(SHADER_INCLUDES common.glsl scene_ubo.glsl)
    find_program(GLSLC_EXECUTABLE glslc)
    set(shader_hashes "")
    set(shader_include_paths "")
    foreach(sh ${SHADER_INCLUDES} ${SHADER_STAGES})
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
        file(SHA256 ${src} h)
        string(APPEND shader_hashes "${h}  ${sh}\n")
        string(REPLACE "." "_" shn ${sh})
        dragongl_embed(${src} asset_glsl_${shn})
    endforeach()
    foreach(sh ${SHADER_INCLUDES})
        list(APPEND shader_include_paths ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh})
    endforeach()
    set(shader_stamp ${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/SOURCES.sha256)
    if(GLSLC_EXECUTABLE)
        file(WRITE ${shader_stamp} "${shader_hashes}")
    else()
        set(old_hashes "")
        if(EXISTS ${shader_stamp})
            file(READ ${shader_stamp} old_hashes)
        endif()
        if(NOT old_hashes STREQUAL shader_hashes)
            message(FATAL_ERROR "The shaders in shaders/ were changed after shaders/spv/ was built: "
                "install glslc (shaderc, or the Vulkan SDK) to compile them.")
        endif()
    endif()
    foreach(sh ${SHADER_STAGES})
        string(REPLACE "." "_" shn ${sh})
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh})
        set(prebuilt ${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv/${sh}.spv)
        if(GLSLC_EXECUTABLE)
            set(spv ${DRAGONGL_GENERATED_DIR}/${sh}.spv)
            add_custom_command(
                OUTPUT ${spv}
                COMMAND ${GLSLC_EXECUTABLE} -O -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders ${src} -o ${spv}
                COMMAND ${CMAKE_COMMAND} -E copy ${spv} ${prebuilt}
                DEPENDS ${src} ${shader_include_paths}
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
        src/client/shaders.c
        src/client/audio.c
        src/client/music.c
        src/client/session.c
        src/client/settings.c
        src/client/net_thread.c
        src/client/client_state.c
        src/client/client_tome.c
        src/client/client_tls.c
        src/client/known_servers.c
        src/shared/map.c
        src/shared/map_city.c
        src/shared/map_traps.c
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
    add_executable(dragongl_client ${CLIENT_SRCS} ${DRAGONGL_CLIENT_SHARED_SRCS} ${EMBED_SRCS}
        ${DRAGONGL_WIN32_SRCS})
    target_include_directories(dragongl_client PRIVATE
        src/client
        ${GLFW_INC}
        third_party/stb
        third_party/volk
        third_party/vulkan
        third_party/miniaudio)
    # The client keeps its own warning set: upstream's -Wconversion /
    # -Wshadow / -Wpedantic policy applies to the server and shared code.
    target_compile_options(dragongl_client PRIVATE ${DRAGONGL_FORCE_INCLUDE}
        -Wno-conversion -Wno-sign-conversion -Wno-shadow -Wno-pedantic
        -Wno-unused-parameter -Wno-missing-field-initializers)
    # volk loads libvulkan / vulkan-1.dll at run time: no link dependency.
    target_compile_definitions(dragongl_client PRIVATE VK_NO_PROTOTYPES GLFW_INCLUDE_NONE
        # Only raw playback is used: the synthesizer generates every sound.
        MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER
        MA_NO_NODE_GRAPH MA_NO_ENGINE MA_NO_WAV MA_NO_MP3 MA_NO_FLAC)
    if(WIN32)
        target_compile_definitions(dragongl_client PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    endif()
    target_link_libraries(dragongl_client ${GLFW_TARGET} ${COMMON_LIBS})
    if(NOT WIN32)
        target_link_libraries(dragongl_client ${CMAKE_DL_LIBS})
    else()
        # GUI subsystem: no console window behind the game.
        set_target_properties(dragongl_client PROPERTIES WIN32_EXECUTABLE ON)
        target_link_options(dragongl_client PRIVATE -mwindows)
    endif()
endif()

# --- Installed paths, substituted into the man pages and the systemd unit ----------
set(DRAGONGL_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
set(DRAGONGL_SYSCONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
set(DRAGONGL_PKGDATADIR "${CMAKE_INSTALL_FULL_DATADIR}/dragongl")

# --- Man pages: version and date come from dragongl.spec -------------------------
function(dragongl_man_page name)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/man/${name}.in"
                   "${CMAKE_BINARY_DIR}/man/${name}" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/man/${name}" DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endfunction()

# --- Installation -------------------------------------------------------------------
# Every installed file has one owner in dragongl.spec:
#   dragongl         client, its launcher, man pages, shaders, menu entry and icon
#   dragongl-server  server, launchers, man pages, systemd unit and config
#   dragongl-data    game data (JSON)
#   dragongl-tools   DM and balance tools, Data Studio
if(DRAGONGL_BUILD_SERVER)
    install(TARGETS dragongl_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS dragongl-server dragongl-reset-world DESTINATION ${CMAKE_INSTALL_BINDIR})
    dragongl_man_page(dragongl_server.1)
    dragongl_man_page(dragongl-server.1)
    dragongl_man_page(dragongl-reset-world.1)

    # Game data (read-only JSON). The server writes only to its state
    # directory, never here.
    install(FILES data/bestiary.json data/items.json data/spells.json
            DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/data)

    # DM and balance tools (dm_pdf runs generate_pdf_map.py from here).
    install(PROGRAMS tools/analyze_balance.py tools/analyze_log.py tools/generate_pdf_map.py
            DESTINATION ${CMAKE_INSTALL_DATADIR}/dragongl/tools)
    # The three tools are commands too: dragongl-analyze-balance,
    # dragongl-analyze-log and dragongl-generate-pdf-map, relative symbolic
    # links to the scripts, each with its man page.
    file(RELATIVE_PATH DRAGONGL_TOOLS_FROM_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}"
         "${CMAKE_INSTALL_FULL_DATADIR}/dragongl/tools")
    foreach(pair "dragongl-analyze-balance:analyze_balance.py"
                 "dragongl-analyze-log:analyze_log.py"
                 "dragongl-generate-pdf-map:generate_pdf_map.py")
        string(REPLACE ":" ";" pair "${pair}")
        list(GET pair 0 cmd)
        list(GET pair 1 script)
        install(CODE "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}\")
            file(CREATE_LINK \"${DRAGONGL_TOOLS_FROM_BINDIR}/${script}\"
                 \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/${cmd}\" SYMBOLIC)")
        dragongl_man_page(${cmd}.1)
    endforeach()
    # Data Studio (self-contained HTML workbenches): the pages load the
    # datasets through ../../data/*.json, i.e. <datadir>/dragongl/data.
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

    if(DRAGONGL_INSTALL_SYSTEMD_UNIT)
        configure_file(packaging/dragongl-server.service.in
                       "${CMAKE_BINARY_DIR}/packaging/dragongl-server.service" @ONLY)
        install(FILES "${CMAKE_BINARY_DIR}/packaging/dragongl-server.service"
                DESTINATION ${DRAGONGL_SYSTEMD_UNITDIR})
        # Options of the service, never replaced by an update (the spec marks
        # it %config(noreplace)). It holds no secret: the passwords are
        # credentials of the unit, files the administrator creates.
        install(FILES packaging/dragongl-server.conf
                DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/dragongl)
    endif()
    if(DRAGONGL_FIREWALLD_SERVICEDIR)
        install(FILES packaging/dragongl-server.firewalld.xml
                DESTINATION ${DRAGONGL_FIREWALLD_SERVICEDIR}
                RENAME dragongl-server.xml)
    endif()
endif()

if(DRAGONGL_BUILD_CLIENT)
    install(TARGETS dragongl_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS dragongl-client DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Desktop integration (freedesktop.org): menu entry, icon and AppStream
    # metadata, all named by the application id. The release in the
    # metadata is the version of dragongl.spec.
    set(DRAGONGL_APP_ID io.github.nicolataibi.dragongl)
    install(FILES packaging/${DRAGONGL_APP_ID}.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/${DRAGONGL_APP_ID}.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    configure_file(packaging/${DRAGONGL_APP_ID}.metainfo.xml.in
                   "${CMAKE_BINARY_DIR}/packaging/${DRAGONGL_APP_ID}.metainfo.xml" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/packaging/${DRAGONGL_APP_ID}.metainfo.xml"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    dragongl_man_page(dragongl_client.1)
    dragongl_man_page(dragongl-client.1)
endif()

if(DRAGONGL_BUILD_CLIENT)
    install(FILES third_party/fonts/OFL-Cinzel.txt third_party/fonts/OFL-AlegreyaSans.txt
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/dragongl/fonts)
endif()
