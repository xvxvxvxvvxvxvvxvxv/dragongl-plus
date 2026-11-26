/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The command line of dragongl_server.
 */

#ifndef DRAGONGL_SERVER_OPTIONS_H
#define DRAGONGL_SERVER_OPTIONS_H

typedef struct {
    int port;
    const char *password_file;
    const char *dm_password_file;
    const char *data_dir;
    const char *state_dir;
    const char *tls_cert;
    const char *tls_key;
    /*DragonGL+ ("Play Solo"): listen on the loopback address only, and
     * shut down cleanly when stop_file appears.*/
    int local;
    const char *stop_file;
    /*DragonGL+: directory of character files of DragonGL+ 2026.10 to
     * import at start-up (legacy_import.h); their new password comes from
     * DRAGONGL_IMPORT_PASSWORD.*/
    const char *import_dir;
} ServerOptions;

/* Parse argv into o (the strings point into argv). -1 to go on,
 * otherwise the exit status: 0 after --help/--version, 2 on an invalid
 * option. */
int parse_options(int argc, char **argv, ServerOptions *o);

#endif
