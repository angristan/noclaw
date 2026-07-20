/*
 * noclaw — The absolute smallest AI assistant. Pure C.
 *
 * CLI entry point with subcommand dispatch.
 * Mirrors nullclaw/zeroclaw/picoclaw command structure.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>

/* ── Test main (when compiled with -DNC_TEST_MAIN) ────────────── */

#ifdef NC_TEST_MAIN

int nc_test_pass = 0;
int nc_test_fail = 0;

int main(void) {
    printf("noclaw test suite\n");
    printf("═════════════════\n\n");

    nc_test_arena();
    nc_test_str();
    nc_test_json();
    nc_test_jwriter();
    nc_test_config();
    nc_test_memory();
    nc_test_http();
    nc_test_tools();
    nc_test_provider();

    printf("\n═════════════════\n");
    printf("Results: %d passed, %d failed\n", nc_test_pass, nc_test_fail);
    return nc_test_fail > 0 ? 1 : 0;
}

#else /* Normal build */

/* ── Usage ────────────────────────────────────────────────────── */

static void print_usage(void) {
    const char *usage =
        "noclaw -- The absolute smallest AI assistant. Pure C.\n"
        "\n"
        "USAGE:\n"
        "  noclaw <command> [options]\n"
        "\n"
        "COMMANDS:\n"
        "  onboard     Initialize workspace and configuration\n"
        "  agent       Start the AI agent (interactive or single message)\n"
        "  gateway     Start the HTTP gateway server\n"
        "  status      Show system status\n"
        "  doctor      Run diagnostics\n"
        "  help        Show this help\n"
        "\n"
        "OPTIONS:\n"
        "  onboard [--api-key KEY] [--provider PROV]\n"
        "  agent [-m MESSAGE]\n"
        "  gateway [--port PORT] [--host HOST]\n"
        "\n"
        "EXAMPLES:\n"
        "  noclaw onboard --api-key sk-... --provider openrouter\n"
        "  noclaw agent -m \"Hello, noclaw!\"\n"
        "  noclaw agent\n"
        "  noclaw gateway --port 8080\n"
        "  noclaw status\n";
    fputs(usage, stdout);
}

/* ── Command dispatch ─────────────────────────────────────────── */

typedef struct {
    const char *name;
    int (*fn)(int argc, char **argv);
} command;

static const command commands[] = {
    { "agent",   nc_cmd_agent   },
    { "gateway", nc_cmd_gateway },
    { "status",  nc_cmd_status  },
    { "onboard", nc_cmd_onboard },
    { "doctor",  nc_cmd_doctor  },
    { NULL, NULL },
};

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char *cmd = argv[1];

    /* Help */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    /* Version */
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("noclaw %s\n", NC_VERSION);
        return 0;
    }

    /* Dispatch */
    for (const command *c = commands; c->name; c++) {
        if (strcmp(cmd, c->name) == 0) {
            return c->fn(argc - 2, argv + 2);
        }
    }

    fprintf(stderr, "Unknown command: %s\n\nRun `noclaw help` for usage.\n", cmd);
    return 1;
}

#endif /* NC_TEST_MAIN */
