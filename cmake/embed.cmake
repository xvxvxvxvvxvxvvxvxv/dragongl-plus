# Turn a binary file into a C source defining
#   const unsigned char <NAME>[]; const size_t <NAME>_size;
# Usage: cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> -P embed.cmake
file(READ "${IN}" HEX HEX)
string(LENGTH "${HEX}" HEXLEN)
math(EXPR SIZE "${HEXLEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
# Break the (very long) initializer into lines of 32 bytes.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" BYTES "${BYTES}")
file(WRITE "${OUT}"
"/* Generated from ${IN} by cmake/embed.cmake - do not edit. */\n#include <stddef.h>\n"
"#if defined(_MSC_VER)\n__declspec(align(16))\n#endif\n"
"const unsigned char ${NAME}[]\n#if defined(__GNUC__)\n__attribute__((aligned(16)))\n#endif\n= {\n${BYTES}\n0x00 };\n"
"const size_t ${NAME}_size = ${SIZE};\n")
