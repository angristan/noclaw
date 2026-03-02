<p align="center">
  <img src="logo.png" alt="noclaw" width="200">
</p>

<h1 align="center">noclaw</h1>

<p align="center">
  <strong>No overhead. No runtime. No compromise. 100% C. 100% Agnostic.</strong><br>
  <strong>88 KB binary. 324 KB RAM. Runs on anything with a pulse.</strong>
</p>

The absolute smallest fully autonomous AI assistant infrastructure. Until someone [does one in asm I guess](https://github.com/Sh4d1/claw)

## Benchmark Snapshot

|                       | [OpenClaw](https://github.com/openclaw/openclaw) | [NanoBot](https://github.com/HKUDS/nanobot) | [PicoClaw](https://github.com/sipeed/picoclaw) | [ZeroClaw](https://github.com/zeroclaw-labs/zeroclaw) | [NullClaw](https://github.com/nullclaw/nullclaw) | **NoClaw**           |
| --------------------- | ------------------------------------------------ | ------------------------------------------- | ---------------------------------------------- | ----------------------------------------------------- | ------------------------------------------------ | -------------------- |
| **Language**          | TypeScript                                       | Python                                      | Go                                             | Rust                                                  | Zig                                              | **C**                |
| **RAM**               | > 1 GB                                           | > 100 MB                                    | < 10 MB                                        | < 5 MB                                                | ~1 MB\*                                          | **324 KB**           |
| **Startup (0.8 GHz)** | > 500 s                                          | > 30 s                                      | < 1 s                                          | < 10 ms                                               | < 8 ms                                           | **idk man**          |
| **Binary Size**       | ~28 MB (dist)                                    | 363 MB (installed)                          | ~8 MB                                          | 3.4 MB                                                | 678 KB                                           | **88 KB**\*\*        |
| **Build Deps**        | node, pnpm, 1,219 pkgs                           | pip, 103 pkgs                               | go, 82 modules                                 | rustc, cargo, 737 crates                              | zig, libsqlite3                                  | **cc, libbearssl**   |
| **Runtime Deps**      | node + npm                                       | python + pip                                | libc                                           | libc                                                  | libc + curl                                      | **0**                |
| **Source Files**      | ~400+                                            | ~54                                         | ~129                                           | ~120                                                  | ~110                                             | **~14**              |
| **Cost**              | Mac Mini $599                                    | Linux SBC ~$50                              | Linux Board $10                                | Any $10 hardware                                      | Any $5 hardware                                  | **Any 50¢ hardware** |

> noclaw numbers measured on arm64 Linux (Debian bookworm). RAM is peak RSS with `make musl` (static musl build).
>
> _\*nullclaw's ~1 MB RAM excludes TLS: it shells out to `curl` for every HTTP request, so TLS memory is charged to the child process. noclaw does TLS in-process._
>
> _\*\*88 KB dynamic release build (macOS arm64). Static musl build (`make musl`) is ~270 KB, includes all deps (BearSSL, musl libc) — zero runtime dependencies._

## Features

- **2 providers.** OpenAI-compatible (OpenRouter, etc.) and Anthropic. Adding a new one is filling in a vtable struct.
- **4 channels.** CLI, Telegram, Discord, Slack. Channels are a vtable, add more as needed.
- **5 tools.** shell, file_read, file_write, memory_store, memory_recall. Same pattern, add more as needed.
- **Flat-file memory with keyword search.** The LLM is the ranker. No SQL engine, no embeddings, no vector DB.
- **HTTP gateway.** /health, /pair, /webhook. Pairing codes, bearer tokens, 127.0.0.1 by default.
- **324 KB peak RSS.** Less memory than a favicon.
- **Fast startup.** I'm sure it's very fast. I don't have a 0.8Ghz CPU to test and idk why it's become a reference point on the other benchmarks. But yeah, very fast.
- **Single binary.** `scp` it to any Linux box and run it. Zero runtime deps with static musl.

## Quick Start

```sh
git clone https://github.com/angristan/noclaw.git
cd noclaw
make release

# Setup
./noclaw onboard --api-key sk-... --provider openrouter

# Chat
./noclaw agent -m "Hello, noclaw!"

# Interactive mode
./noclaw agent

# Start gateway
./noclaw gateway

# Check status
./noclaw status

# Diagnostics
./noclaw doctor
```

## Architecture

Every subsystem is a **function-pointer vtable** -- swap implementations at build or runtime.

| Subsystem     | Interface     | Ships with                                                | Extend                        |
| ------------- | ------------- | --------------------------------------------------------- | ----------------------------- |
| **AI Models** | `nc_provider` | OpenAI-compatible, Anthropic                              | Any OpenAI-compatible API     |
| **Channels**  | `nc_channel`  | CLI, Telegram, Discord, Slack                             | Any chat platform             |
| **Memory**    | `nc_memory`   | Flat-file (keyword search)                                | Custom backends               |
| **Tools**     | `nc_tool`     | shell, file_read, file_write, memory_store, memory_recall | Any capability                |
| **Gateway**   | HTTP server   | /health, /pair, /webhook                                  | Tunnel integration            |

### Security (mirrors nullclaw/zeroclaw)

| #   | Item                         | Status | How                                                                                                             |
| --- | ---------------------------- | ------ | --------------------------------------------------------------------------------------------------------------- |
| 1   | Gateway not publicly exposed | Done   | Binds `127.0.0.1` by default. Refuses `0.0.0.0` without explicit opt-in.                                        |
| 2   | Pairing required             | Done   | 6-char code on startup. Exchange via `POST /pair` for bearer token.                                             |
| 3   | Filesystem scoped            | Done   | `workspace_only=true` by default. Absolute paths rejected. Path traversal blocked.                              |
| 4   | Workspace restriction        | Done   | Tools enforce workspace boundaries. `..` path components rejected. Shell injection on workspace path prevented. |

## Configuration

Config: `~/.noclaw/config.json` (created by `onboard`)

```json
{
  "api_key": "sk-...",
  "default_provider": "openrouter",
  "default_model": "anthropic/claude-sonnet-4",
  "default_temperature": 0.7,
  "gateway": {
    "port": 3000,
    "host": "127.0.0.1",
    "require_pairing": true,
    "allow_public_bind": false
  },
  "memory": {
    "backend": "flat",
    "auto_save": true
  },
  "autonomy": {
    "level": "supervised",
    "workspace_only": true,
    "max_actions_per_hour": 20
  },
  "heartbeat": {
    "enabled": false,
    "interval_minutes": 30
  }
}
```

Environment variable overrides: `NOCLAW_API_KEY`, `NOCLAW_PROVIDER`, `NOCLAW_MODEL`, `NOCLAW_TEMPERATURE`, `NOCLAW_GATEWAY_PORT`, `NOCLAW_GATEWAY_HOST`, `NOCLAW_WORKSPACE`, `NOCLAW_BASE_URL`.

## Gateway API

| Endpoint   | Method | Auth                            | Description                                |
| ---------- | ------ | ------------------------------- | ------------------------------------------ |
| `/health`  | GET    | None                            | Health check (always public)               |
| `/pair`    | POST   | `X-Pairing-Code` header         | Exchange one-time code for bearer token    |
| `/webhook` | POST   | `Authorization: Bearer <token>` | Send message: `{"message": "your prompt"}` |

## Commands

| Command          | Description               |
| ---------------- | ------------------------- |
| `onboard`        | Quick setup (default)     |
| `agent -m "..."` | Single message mode       |
| `agent`          | Interactive chat mode     |
| `gateway`        | Start HTTP gateway server |
| `status`         | Show system status        |
| `doctor`         | Run diagnostics           |

## Development

```sh
make release     # Release build (~80-88 KB dynamic)
make musl        # Static musl build (324 KB RSS, zero deps, Linux only)
make debug       # Debug build with ASan/UBSan
make test        # Run test suite (87 tests)
make size        # Size report
make clean       # Clean build artifacts
```

### Source Layout

```
src/
  main.c       CLI entry point + subcommand dispatch
  nc.h         Single header: all types, vtables, interfaces
  arena.c      Arena allocator (bump-pointer, no individual frees)
  util.c       String utils, path joining, file I/O, logging
  json.c       Recursive-descent JSON parser + writer (zero deps)
  config.c     Config loader (~/.noclaw/config.json)
  http.c       HTTP client (platform TLS)
  provider.c   Provider vtable: OpenAI-compatible + Anthropic
  channel.c    Channel vtable: CLI, Telegram, Discord, Slack
  tools.c      Tool vtable: shell, file_read, file_write, memory
  memory.c     Memory vtable: flat-file keyword search
  agent.c      Agent loop: conversation history + tool dispatch
  gateway.c    HTTP gateway server: /health, /pair, /webhook
  commands.c   CLI commands: agent, gateway, status, onboard, doctor
```

### Project Stats

```
Language:       C11
Source files:   14
Lines of code:  ~5,350
Tests:          87
Binary:         ~270 KB static musl (Linux), ~88 KB dynamic (macOS arm64)
Peak RSS:       324 KB (Linux musl), ~1.9 MB (Linux glibc), ~5.6 MB (macOS)
Startup:        idk man (no 0.8 GHz test hardware)
Dependencies:   none (static musl) or libc+bearssl (dynamic)
```

## Why C

Zig (nullclaw) got it down to 678 KB. C gets it to ~270 KB static, ~88 KB dynamic. 14 files, ~5,350 lines.

The biggest win isn't the language, it's musl. glibc costs ~1.3 MB RSS before your code even runs (dynamic linker, locale data, malloc arena pre-allocation). musl costs ~200 KB. When your whole program fits in 324 KB RSS, the libc _is_ the program. Static musl also means the binary runs on any Linux 2.6.39+. `scp` and go.

BearSSL's default `br_ssl_client_init_full` links all 45 cipher suites. We wrote a custom `ssl_client_init_minimal` that only configures 4 (ECDHE+AES-GCM), which lets `--gc-sections` strip ~64 KB of 3DES/ChaCha20/CBC/CCM code the linker can now prove is dead.

Nullclaw shells out to `curl` for HTTP (`http_util.zig` literally spawns `curl -s -X POST`). Their ~1 MB RAM number doesn't include TLS because that's in the child process. noclaw does TLS in-process. On Linux that's BearSSL with a 16 KB mono I/O buffer; on macOS it's SecureTransport. One thing that cost us hours: `br_sslio_write` is buffered, so you _must_ call `br_sslio_flush()` before reading the response or the request just sits there. SecureTransport doesn't have this problem.

Memory management is a chunk-based arena. Linked list of chunks, old chunks never move, pointers stay valid. `nc_arena_reset()` rewinds without freeing so the next turn reuses the same pages. An earlier version used `realloc` to grow a flat buffer. Worked on glibc (extends in-place), segfaulted on musl (relocates). Classic.

Architecture is function-pointer vtables, C's version of Zig vtable interfaces or Rust `dyn Trait`. A provider is a struct with a `chat` function pointer. A tool is a struct with an `execute` function pointer. Swap implementations, done.

## License

MIT
