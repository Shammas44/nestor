#ifndef NESTOR_PLUGIN_H
#define NESTOR_PLUGIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SDK for writing Nestor custom plugins in C.
// Focuses on loading input variables, accessing env context, and reporting outputs.

// Helper to read the input JSON payload from the path specified in NESTOR_PLUGIN_INPUT.
static char *np_get_input_json(void) {
    char *path = getenv("NESTOR_PLUGIN_INPUT");
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, size, f);
    buf[read_bytes] = '\0';
    fclose(f);
    return buf;
}

// Helper to read the environment variables JSON string from NESTOR_ENV.
static const char *np_get_env_json(void) {
    return getenv("NESTOR_ENV");
}

// Helper to write JSON success payload to stdout.
static void np_success(const char *json_body) {
    if (json_body) {
        printf("%s\n", json_body);
    } else {
        printf("{}\n");
    }
    exit(0);
}

// Helper to write an error code and error message.
static void np_error(int exit_code, const char *err_msg) {
    if (err_msg) {
        fprintf(stderr, "Error: %s\n", err_msg);
    }
    exit(exit_code != 0 ? exit_code : 1);
}

#endif // NESTOR_PLUGIN_H
