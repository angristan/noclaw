/*
 * Built-in tool implementations: shell, file_read, file_write, memory.
 * Each tool is a vtable instance matching the nc_tool interface.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* ── Helper: extract string from tool args JSON ───────────────── */

static bool extract_json_string(const char *json, const char *key,
                                char *out, size_t out_cap) {
    if (!json || !key || !out || out_cap == 0) return false;

    nc_arena a;
    nc_arena_init(&a, strlen(json) * 2 + 256);
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) { nc_arena_free(&a); return false; }

    nc_json *val = nc_json_get(root, key);
    nc_str s = nc_json_str(val, "");
    if (s.len == 0) { nc_arena_free(&a); return false; }

    size_t max_bytes = s.len < out_cap - 1 ? s.len : out_cap - 1;
    size_t cplen = nc_utf8_safe_prefix(s.ptr, s.len, max_bytes);
    memcpy(out, s.ptr, cplen);
    out[cplen] = '\0';
    nc_arena_free(&a);
    return true;
}

/* ── Shell tool ───────────────────────────────────────────────── */

static bool shell_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char command[2048];

    if (!extract_json_string(args_json, "command", command, sizeof(command))) {
        nc_strlcpy(out, "error: missing 'command' argument", out_cap);
        return false;
    }

    /* Security: workspace scoping */
    if (cfg->workspace_only) {
        /* Validate workspace_dir has no shell-dangerous chars */
        const char *ws = cfg->workspace_dir;
        for (const char *c = ws; *c; c++) {
            if (*c == '\'' || *c == '\\' || *c == '"' || *c == '$' ||
                *c == '`' || *c == '!' || *c == ';' || *c == '|' ||
                *c == '&' || *c == '\n') {
                nc_strlcpy(out, "error: workspace path contains unsafe characters", out_cap);
                return false;
            }
        }
        /* Prepend cd to workspace */
        char full_cmd[4096];
        snprintf(full_cmd, sizeof(full_cmd), "cd '%s' && %s", cfg->workspace_dir, command);
        nc_strlcpy(command, full_cmd, sizeof(command));
    }

    /* Execute and capture output */
    FILE *fp = popen(command, "r");
    if (!fp) {
        nc_strlcpy(out, "error: failed to execute command", out_cap);
        return false;
    }

    size_t total = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp) && total < out_cap - 1) {
        size_t n = strlen(buf);
        if (total + n >= out_cap - 1) n = out_cap - 1 - total;
        memcpy(out + total, buf, n);
        total += n;
    }
    out[total] = '\0';
    int status = pclose(fp);

    if (status != 0) {
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "\n[exit code: %d]", WEXITSTATUS(status));
        size_t sl = strlen(suffix);
        if (total + sl < out_cap) {
            memcpy(out + total, suffix, sl + 1);
        }
    }

    return status == 0;
}

nc_tool nc_tool_shell(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "shell",
            .description = "Execute a shell command and return its output.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"The shell command to execute\"}},\"required\":[\"command\"]}",
        },
        .ctx = (void *)cfg,
        .execute = shell_execute,
        .free = NULL,
    };
}

/* ── File read tool ───────────────────────────────────────────── */

static bool file_read_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024];

    if (!extract_json_string(args_json, "path", path, sizeof(path))) {
        nc_strlcpy(out, "error: missing 'path' argument", out_cap);
        return false;
    }

    /* Security: workspace scoping */
    if (cfg->workspace_only) {
        if (path[0] == '/') {
            nc_strlcpy(out, "error: absolute paths not allowed in workspace mode", out_cap);
            return false;
        }
        char full_path[2048];
        nc_path_join(full_path, sizeof(full_path), cfg->workspace_dir, path);
        nc_strlcpy(path, full_path, sizeof(path));
    }

    /* Check for path traversal (/../ components, or leading ../) */
    {
        const char *p = path;
        while (*p) {
            if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
                nc_strlcpy(out, "error: path traversal not allowed", out_cap);
                return false;
            }
            /* Advance to next path component */
            const char *sl = strchr(p, '/');
            if (!sl) break;
            p = sl + 1;
        }
    }

    size_t len;
    char *content = nc_read_file(path, &len);
    if (!content) {
        snprintf(out, out_cap, "error: cannot read file '%s'", path);
        return false;
    }

    size_t cplen = len < out_cap - 1 ? len : out_cap - 1;
    memcpy(out, content, cplen);
    out[cplen] = '\0';
    free(content);
    return true;
}

nc_tool nc_tool_file_read(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "file_read",
            .description = "Read the contents of a file.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path to read\"}},\"required\":[\"path\"]}",
        },
        .ctx = (void *)cfg,
        .execute = file_read_execute,
        .free = NULL,
    };
}

/* ── File write tool ──────────────────────────────────────────── */

static bool file_write_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024], content[8192];

    if (!extract_json_string(args_json, "path", path, sizeof(path))) {
        nc_strlcpy(out, "error: missing 'path' argument", out_cap);
        return false;
    }
    if (!extract_json_string(args_json, "content", content, sizeof(content))) {
        nc_strlcpy(out, "error: missing 'content' argument", out_cap);
        return false;
    }

    /* Security: workspace scoping */
    if (cfg->workspace_only) {
        if (path[0] == '/') {
            nc_strlcpy(out, "error: absolute paths not allowed in workspace mode", out_cap);
            return false;
        }
        char full_path[2048];
        nc_path_join(full_path, sizeof(full_path), cfg->workspace_dir, path);
        nc_strlcpy(path, full_path, sizeof(path));
    }

    /* Check for path traversal (/../ components) */
    {
        const char *p = path;
        while (*p) {
            if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
                nc_strlcpy(out, "error: path traversal not allowed", out_cap);
                return false;
            }
            const char *sl = strchr(p, '/');
            if (!sl) break;
            p = sl + 1;
        }
    }

    if (nc_write_file(path, content, strlen(content))) {
        snprintf(out, out_cap, "Written %zu bytes to %s", strlen(content), path);
        return true;
    } else {
        snprintf(out, out_cap, "error: failed to write '%s'", path);
        return false;
    }
}

nc_tool nc_tool_file_write(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "file_write",
            .description = "Write content to a file.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}},\"required\":[\"path\",\"content\"]}",
        },
        .ctx = (void *)cfg,
        .execute = file_write_execute,
        .free = NULL,
    };
}

/* ── Memory store tool ────────────────────────────────────────── */

static bool memory_store_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    nc_memory *mem = (nc_memory *)self->ctx;
    if (!mem) {
        nc_strlcpy(out, "error: memory not configured", out_cap);
        return false;
    }

    char key[256], content[4096];
    if (!extract_json_string(args_json, "key", key, sizeof(key))) {
        nc_strlcpy(out, "error: missing 'key' argument", out_cap);
        return false;
    }
    if (!extract_json_string(args_json, "content", content, sizeof(content))) {
        nc_strlcpy(out, "error: missing 'content' argument", out_cap);
        return false;
    }

    if (mem->store(mem, key, content)) {
        snprintf(out, out_cap, "Stored memory: %s", key);
        return true;
    }
    nc_strlcpy(out, "error: failed to store memory", out_cap);
    return false;
}

nc_tool nc_tool_memory_store(void *mem_ctx) {
    return (nc_tool){
        .def = {
            .name = "memory_store",
            .description = "Store a piece of information in long-term memory.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key\"},\"content\":{\"type\":\"string\",\"description\":\"Content to remember\"}},\"required\":[\"key\",\"content\"]}",
        },
        .ctx = mem_ctx,
        .execute = memory_store_execute,
        .free = NULL,
    };
}

/* ── Memory recall tool ───────────────────────────────────────── */

static bool memory_recall_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    nc_memory *mem = (nc_memory *)self->ctx;
    if (!mem) {
        nc_strlcpy(out, "error: memory not configured", out_cap);
        return false;
    }

    char query[1024];
    if (!extract_json_string(args_json, "query", query, sizeof(query))) {
        nc_strlcpy(out, "error: missing 'query' argument", out_cap);
        return false;
    }

    if (mem->recall(mem, query, out, out_cap)) {
        return true;
    }
    nc_strlcpy(out, "No matching memories found.", out_cap);
    return true;
}

nc_tool nc_tool_memory_recall(void *mem_ctx) {
    return (nc_tool){
        .def = {
            .name = "memory_recall",
            .description = "Search long-term memory for relevant information.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}",
        },
        .ctx = mem_ctx,
        .execute = memory_recall_execute,
        .free = NULL,
    };
}

#ifdef NC_TEST
void nc_test_tools(void) {
    const char *json = "{\"content\":\"\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x91\x8b\"}";
    char full[16];
    char truncated[5];

    NC_ASSERT(extract_json_string(json, "content", full, sizeof(full)),
              "tool args extract UTF-8");
    NC_ASSERT(strcmp(full, "\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x91\x8b") == 0,
              "tool args preserve UTF-8");
    NC_ASSERT(extract_json_string(json, "content", truncated, sizeof(truncated)),
              "tool args truncate UTF-8");
    NC_ASSERT(strcmp(truncated, "\xe4\xbd\xa0") == 0,
              "tool args truncate at UTF-8 boundary");
}
#endif
