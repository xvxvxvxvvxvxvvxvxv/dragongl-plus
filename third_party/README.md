# Third-party components

Vendored so that DragonGL builds (and ships) without any system
development package. Every component keeps its own license file.

| Directory   | Component                    | License                  | Used by            |
|-------------|------------------------------|--------------------------|--------------------|
| `glfw/`     | GLFW 3.4 (window/input)      | zlib/libpng              | client             |
| `volk/`     | volk (Vulkan meta-loader)    | MIT                      | client (Vulkan)    |
| `vulkan/`   | Khronos Vulkan headers       | Apache-2.0 OR MIT        | client (Vulkan)    |
| `miniaudio/`| miniaudio (audio output)     | Public domain / MIT-0    | client (audio)     |
| `stb/`      | stb_truetype, stb_image_write| Public domain / MIT      | client (UI, shots) |
| `wepoll/`   | wepoll (epoll for Windows)   | BSD-2-Clause             | server (Windows)   |
| `fonts/`    | Cinzel, Alegreya Sans        | SIL Open Font License 1.1| client (UI)        |

The fonts are embedded in the client binary at build time
(`tools/embed.cmake`); their OFL license texts ship in `fonts/`.
