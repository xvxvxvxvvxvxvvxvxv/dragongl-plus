/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*DragonGL graphical client entry point.
 *
 * Everything (server choice, login, character creation) now happens in
 * the game window; the old terminal prompts are gone. The historical
 * command line still works:  dragongl_client gl|vk [server_ip] [port]
 * opens the "Join a Server" screen pre-filled with that address.*/

#include "app.h"
#include "audio.h"
#include "settings.h"
#include "platform.h"
#include "net.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool app_init(const GfxBackend *gfx);
void app_run(void);
void app_shutdown(void);

static void glfw_error(int code, const char *desc) {
    fprintf(stderr, "[GLFW] error %d: %s\n", code, desc);
}

static void usage(const char *argv0) {
    printf("Dragon GL client\n\n"
           "Usage: %s [options] [gl|vk] [server_ip] [port]\n\n"
           "  gl | --gl          use the OpenGL 3.3 renderer\n"
           "  vk | --vk          use the Vulkan renderer\n"
           "  --fullscreen       start in fullscreen\n"
           "  --windowed         start in a window\n"
           "  server_ip [port]   pre-fill the Join screen\n"
           "  -h, --help         this help\n\n"
           "Settings are stored in %s\n",
           argv0, settings_path());
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    plat_net_init();
    settings_load();

    int backend = g_settings.backend;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "gl") || !strcmp(a, "--gl")) backend = 0;
        else if (!strcmp(a, "vk") || !strcmp(a, "--vk")) backend = 1;
        else if (!strcmp(a, "--fullscreen")) g_settings.fullscreen = true;
        else if (!strcmp(a, "--windowed")) g_settings.fullscreen = false;
        else if (a[0] != '-') {
            /*positional: server ip then port (legacy launcher syntax)*/
            if (atoi(a) > 0 && strchr(a, '.') == NULL && g_settings.last_host[0]) g_settings.last_port = atoi(a);
            else copy_str(g_settings.last_host, a, sizeof(g_settings.last_host));
        }
    }

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        fprintf(stderr, "Cannot initialise the window system (is a graphical session running?).\n");
        return 1;
    }
    audio_init();

    const GfxBackend *gfx = backend == 1 ? &gfx_vk_backend : &gfx_gl_backend;
    if (backend == 1 && !gfx_vk_available()) {
        fprintf(stderr, "[VK] no Vulkan driver found: falling back to OpenGL.\n");
        gfx = &gfx_gl_backend;
    }
    if (!app_init(gfx)) {
        app_shutdown();
        if (gfx == &gfx_vk_backend) {
            fprintf(stderr, "[VK] initialisation failed: falling back to OpenGL.\n");
            g_settings.backend = 0;
            gfx = &gfx_gl_backend;
            if (!app_init(gfx)) {
                fprintf(stderr, "Could not create a window with OpenGL 3.3 either.\n");
                glfwTerminate();
                return 1;
            }
        } else {
            fprintf(stderr, "Could not create an OpenGL 3.3 window. Update your graphics drivers.\n");
            glfwTerminate();
            return 1;
        }
    }
    app_run();
    app_shutdown();
    audio_shutdown();
    glfwTerminate();
    return 0;
}
