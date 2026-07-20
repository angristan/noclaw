/*
 * Provider implementations: OpenAI-compatible and Anthropic.
 * Uses nc_http_post (native TLS) for HTTPS.
 *
 * Both providers handle full tool-call round-trips:
 *   - Serialize assistant messages with tool_calls
 *   - Serialize tool-result messages
 *   - Parse tool_calls / tool_use from responses
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Shared helpers ───────────────────────────────────────────── */

typedef struct {
    char api_key[256];
    char api_url[256];
} provider_ctx;

static size_t json_escaped_size(const char *s) {
    if (!s) return 0;

    size_t len = strlen(s);
    size_t size = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t')
                size += 2;
            else if (c < 0x20)
                size += 6;
            else
                size++;
            i++;
            continue;
        }

        size_t n = nc_utf8_sequence_len(s + i, len - i);
        if (n == 0) {
            size += 6; /* malformed byte becomes \ufffd */
            i++;
        } else {
            size += n;
            i += n;
        }
    }
    return size;
}

/* Escape a string for JSON while guaranteeing valid UTF-8 output. */
static int json_escape_into(char *buf, size_t bufsz, int off, const char *s) {
    if (!s || !buf || bufsz <= 10 || off < 0) return off;

    static const char hex[] = "0123456789abcdef";
    size_t len = strlen(s);
    size_t limit = bufsz - 10; /* reserve space for the caller's closing syntax */

    for (size_t i = 0; i < len && (size_t)off < limit;) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            size_t n = nc_utf8_sequence_len(s + i, len - i);
            if (n > 0) {
                if (n > limit - (size_t)off) break;
                memcpy(buf + off, s + i, n);
                off += (int)n;
                i += n;
                continue;
            }

            if (limit - (size_t)off < 6) break;
            memcpy(buf + off, "\\ufffd", 6);
            off += 6;
            i++;
            continue;
        }

        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            if (limit - (size_t)off < 2) break;
            buf[off++] = '\\';
            switch (c) {
                case '"':  buf[off++] = '"';  break;
                case '\\': buf[off++] = '\\'; break;
                case '\n': buf[off++] = 'n';  break;
                case '\r': buf[off++] = 'r';  break;
                default:   buf[off++] = 't';  break;
            }
        } else if (c < 0x20) {
            if (limit - (size_t)off < 6) break;
            buf[off++] = '\\';
            buf[off++] = 'u';
            buf[off++] = '0';
            buf[off++] = '0';
            buf[off++] = hex[c >> 4];
            buf[off++] = hex[c & 0x0f];
        } else {
            buf[off++] = (char)c;
        }
        i++;
    }
    return off;
}

/* Compute buffer size needed for messages. */
static size_t estimate_messages_size(const nc_message *msgs, int count) {
    size_t sz = 512;
    for (int i = 0; i < count; i++) {
        sz += 256; /* per-message overhead */
        sz += json_escaped_size(msgs[i].content);
        /* tool_calls in assistant messages */
        for (int j = 0; j < msgs[i].tool_call_count; j++) {
            sz += 256; /* per-call overhead */
            sz += json_escaped_size(msgs[i].tool_calls[j].arguments);
        }
    }
    return sz;
}

/* ══════════════════════════════════════════════════════════════════
 *  OpenAI-compatible provider (OpenRouter, OpenAI, etc.)
 *
 *  Wire format for tool calls:
 *    Assistant: { "role":"assistant", "content":null,
 *                 "tool_calls":[{"id":"call_x","type":"function",
 *                   "function":{"name":"foo","arguments":"{...}"}}] }
 *    Result:   { "role":"tool", "tool_call_id":"call_x", "content":"..." }
 * ══════════════════════════════════════════════════════════════════ */

/* Build the messages array for OpenAI format */
static int openai_build_messages(char *buf, size_t bufsz,
                                 const nc_message *msgs, int count) {
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "[");

    for (int i = 0; i < count; i++) {
        if ((size_t)off >= bufsz - 10) break;
        if (i > 0) buf[off++] = ',';

        if (msgs[i].tool_call_count > 0) {
            /* Assistant message with tool_calls */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"assistant\",\"content\":");

            /* content can be null or a string */
            if (msgs[i].content && msgs[i].content[0]) {
                buf[off++] = '"';
                off = json_escape_into(buf, bufsz, off, msgs[i].content);
                buf[off++] = '"';
            } else {
                off += snprintf(buf + off, bufsz - (size_t)off, "null");
            }

            off += snprintf(buf + off, bufsz - (size_t)off, ",\"tool_calls\":[");
            for (int j = 0; j < msgs[i].tool_call_count; j++) {
                if (j > 0) buf[off++] = ',';
                const nc_tool_call *tc = &msgs[i].tool_calls[j];
                off += snprintf(buf + off, bufsz - (size_t)off,
                    "{\"id\":\"%s\",\"type\":\"function\","
                    "\"function\":{\"name\":\"%s\",\"arguments\":\"",
                    tc->id, tc->name);
                /* arguments is a JSON string that must be escaped */
                off = json_escape_into(buf, bufsz, off, tc->arguments);
                off += snprintf(buf + off, bufsz - (size_t)off, "\"}}");
            }
            off += snprintf(buf + off, bufsz - (size_t)off, "]}");

        } else if (msgs[i].tool_call_id && msgs[i].tool_call_id[0]) {
            /* Tool result message */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"",
                msgs[i].tool_call_id);
            off = json_escape_into(buf, bufsz, off, msgs[i].content);
            off += snprintf(buf + off, bufsz - (size_t)off, "\"}");

        } else {
            /* Normal message (system/user/assistant) */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"%s\",\"content\":\"", msgs[i].role);
            off = json_escape_into(buf, bufsz, off, msgs[i].content);
            off += snprintf(buf + off, bufsz - (size_t)off, "\"}");
        }
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]");
    return off;
}

/* Parse tool_calls from OpenAI response JSON */
static void openai_parse_tool_calls(nc_json *tc_arr, nc_chat_response *resp) {
    if (!tc_arr || tc_arr->type != NC_JSON_ARRAY) return;

    for (int i = 0; i < tc_arr->array.count && resp->tool_call_count < NC_MAX_TOOL_CALLS; i++) {
        nc_json *tc = &tc_arr->array.items[i];
        nc_json *fn = nc_json_get(tc, "function");
        if (!fn) continue;

        nc_tool_call *out = &resp->tool_calls[resp->tool_call_count];

        nc_str id = nc_json_str(nc_json_get(tc, "id"), "");
        if (id.len > 0) {
            size_t cl = id.len < sizeof(out->id) - 1 ? id.len : sizeof(out->id) - 1;
            memcpy(out->id, id.ptr, cl);
            out->id[cl] = '\0';
        }

        nc_str name = nc_json_str(nc_json_get(fn, "name"), "");
        if (name.len > 0) {
            size_t cl = name.len < sizeof(out->name) - 1 ? name.len : sizeof(out->name) - 1;
            memcpy(out->name, name.ptr, cl);
            out->name[cl] = '\0';
        }

        /* arguments is a JSON string (already escaped in the response) */
        nc_str args = nc_json_str(nc_json_get(fn, "arguments"), "{}");
        if (args.len > 0) {
            size_t cl = args.len < sizeof(out->arguments) - 1 ? args.len : sizeof(out->arguments) - 1;
            memcpy(out->arguments, args.ptr, cl);
            out->arguments[cl] = '\0';
        }

        resp->tool_call_count++;
    }

    resp->has_tool_calls = resp->tool_call_count > 0;
}

static bool openai_chat(nc_provider *self, const nc_chat_request *req, nc_chat_response *resp) {
    provider_ctx *ctx = (provider_ctx *)self->ctx;
    memset(resp, 0, sizeof(*resp));

    /* Build messages JSON */
    size_t msgs_buf_sz = estimate_messages_size(req->messages, req->message_count);
    char *msgs_json = (char *)malloc(msgs_buf_sz);
    if (!msgs_json) return false;
    openai_build_messages(msgs_json, msgs_buf_sz, req->messages, req->message_count);

    /* Build request body */
    size_t body_sz = msgs_buf_sz + 1024 + (req->tools_json ? strlen(req->tools_json) : 0);
    char *body = (char *)malloc(body_sz);
    if (!body) { free(msgs_json); return false; }

    int body_len;
    if (req->tools_json) {
        body_len = snprintf(body, body_sz,
            "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d,\"tools\":%s}",
            req->model, msgs_json, req->temperature,
            req->max_tokens > 0 ? req->max_tokens : 4096,
            req->tools_json);
    } else {
        body_len = snprintf(body, body_sz,
            "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d}",
            req->model, msgs_json, req->temperature,
            req->max_tokens > 0 ? req->max_tokens : 4096);
    }

    /* Headers */
    char auth_hdr[300];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", ctx->api_key);
    const char *headers[] = {
        "Content-Type: application/json",
        auth_hdr,
    };

    /* URL */
    char url[512];
    if (ctx->api_url[0])
        snprintf(url, sizeof(url), "%s/chat/completions", ctx->api_url);
    else
        snprintf(url, sizeof(url), "https://openrouter.ai/api/v1/chat/completions");

    bool result = false;
    nc_http_response http_resp;
    if (!nc_http_post(url, body, (size_t)body_len, headers, 2, &http_resp)) {
        nc_log(NC_LOG_ERROR, "HTTP request failed");
        goto cleanup;
    }

    if (http_resp.status != 200) {
        nc_log(NC_LOG_ERROR, "Provider returned HTTP %d: %.200s", http_resp.status, http_resp.body);
        goto cleanup;
    }

    /* Parse response JSON */
    {
        nc_arena a;
        nc_arena_init(&a, http_resp.body_len * 2 + 1024);
        nc_json *root = nc_json_parse(&a, http_resp.body, http_resp.body_len);

        if (!root) {
            nc_log(NC_LOG_ERROR, "Failed to parse provider response");
            nc_arena_free(&a);
            goto cleanup;
        }

        /* Extract choices[0].message */
        nc_json *choices = nc_json_get(root, "choices");
        if (choices && choices->type == NC_JSON_ARRAY && choices->array.count > 0) {
            nc_json *choice0 = &choices->array.items[0];
            nc_json *message = nc_json_get(choice0, "message");
            if (message) {
                /* Content (may be null when tool_calls present) */
                nc_json *content_node = nc_json_get(message, "content");
                if (content_node && content_node->type == NC_JSON_STRING) {
                    nc_str content = content_node->string;
                    size_t cplen = content.len < sizeof(resp->content) - 1
                        ? content.len : sizeof(resp->content) - 1;
                    memcpy(resp->content, content.ptr, cplen);
                    resp->content[cplen] = '\0';
                }

                /* Tool calls */
                nc_json *tc = nc_json_get(message, "tool_calls");
                openai_parse_tool_calls(tc, resp);
            }
        }

        /* Usage */
        nc_json *usage = nc_json_get(root, "usage");
        if (usage) {
            resp->prompt_tokens = (int)nc_json_num(nc_json_get(usage, "prompt_tokens"), 0);
            resp->completion_tokens = (int)nc_json_num(nc_json_get(usage, "completion_tokens"), 0);
        }

        nc_arena_free(&a);
    }
    result = true;

cleanup:
    nc_http_response_free(&http_resp);
    free(msgs_json);
    free(body);
    return result;
}

static void provider_free(nc_provider *self) {
    free(self->ctx);
    self->ctx = NULL;
}

nc_provider nc_provider_openai(const char *api_key, const char *api_url) {
    provider_ctx *ctx = (provider_ctx *)calloc(1, sizeof(provider_ctx));
    if (!ctx) return (nc_provider){ .name = "openai", .ctx = NULL, .chat = NULL, .free = NULL };
    if (api_key) nc_strlcpy(ctx->api_key, api_key, sizeof(ctx->api_key));
    if (api_url) nc_strlcpy(ctx->api_url, api_url, sizeof(ctx->api_url));

    return (nc_provider){
        .name = "openai",
        .ctx  = ctx,
        .chat = openai_chat,
        .free = provider_free,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  Anthropic provider
 *
 *  Wire format for tool calls:
 *    Assistant: { "role":"assistant",
 *      "content":[{"type":"text","text":"..."},
 *                 {"type":"tool_use","id":"toolu_x","name":"foo","input":{...}}] }
 *    Result:   { "role":"user",
 *      "content":[{"type":"tool_result","tool_use_id":"toolu_x","content":"..."}] }
 *
 *  Tools use "input_schema" instead of "parameters".
 * ══════════════════════════════════════════════════════════════════ */

/* Build Anthropic-format tools directly from the tool defs (not from OpenAI JSON) */
static const char *anthropic_tools_json_from_defs(char *buf, size_t bufsz,
                                                  const nc_chat_request *req) {
    /* We can't easily convert OpenAI tools JSON to Anthropic format by re-parsing,
     * because the parameters are JSON objects embedded as raw text.
     * Instead, the agent should provide Anthropic-native tools.
     * But we don't have access to the nc_tool array here.
     *
     * Pragmatic solution: the tools_json from build_tools_json() in agent.c is
     * OpenAI-format. We convert by replacing the structure:
     *   OpenAI: [{"type":"function","function":{"name":"x","description":"y","parameters":{...}}}]
     *   Anthropic: [{"name":"x","description":"y","input_schema":{...}}]
     *
     * We'll re-parse and rebuild. */
    if (!req->tools_json) return NULL;

    nc_arena scratch;
    nc_arena_init(&scratch, strlen(req->tools_json) * 2 + 1024);
    nc_json *arr = nc_json_parse(&scratch, req->tools_json, strlen(req->tools_json));

    if (!arr || arr->type != NC_JSON_ARRAY) {
        nc_arena_free(&scratch);
        return NULL;
    }

    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "[");
    for (int i = 0; i < arr->array.count; i++) {
        if (i > 0) buf[off++] = ',';
        nc_json *tool = &arr->array.items[i];
        nc_json *fn = nc_json_get(tool, "function");
        if (!fn) continue;
        nc_str name = nc_json_str(nc_json_get(fn, "name"), "");
        nc_str desc = nc_json_str(nc_json_get(fn, "description"), "");

        off += snprintf(buf + off, bufsz - (size_t)off,
            "{\"name\":\"%.*s\",\"description\":\"%.*s\",\"input_schema\":",
            NC_STR_ARG(name), NC_STR_ARG(desc));

        /* The parameters value is a JSON object in the parsed tree.
         * We need to re-serialize it. Since our JSON library doesn't have a writer
         * for nc_json nodes, find the raw text in the original tools_json string.
         *
         * Alternative: find "parameters" in the raw string for this tool. */
        nc_json *params = nc_json_get(fn, "parameters");
        if (params && params->type == NC_JSON_OBJECT) {
            /* Re-build a minimal JSON object from the parsed structure */
            off += snprintf(buf + off, bufsz - (size_t)off, "{\"type\":\"object\",\"properties\":{");
            nc_json *props = nc_json_get(params, "properties");
            if (props && props->type == NC_JSON_OBJECT) {
                for (int k = 0; k < props->object.count; k++) {
                    if (k > 0) buf[off++] = ',';
                    nc_str pname = props->object.keys[k];
                    nc_json *pval = &props->object.vals[k];
                    nc_str ptype = nc_json_str(nc_json_get(pval, "type"), "string");
                    nc_str pdesc = nc_json_str(nc_json_get(pval, "description"), "");
                    off += snprintf(buf + off, bufsz - (size_t)off,
                        "\"%.*s\":{\"type\":\"%.*s\",\"description\":\"%.*s\"}",
                        NC_STR_ARG(pname), NC_STR_ARG(ptype), NC_STR_ARG(pdesc));
                }
            }
            off += snprintf(buf + off, bufsz - (size_t)off, "}");
            /* required */
            nc_json *req_arr = nc_json_get(params, "required");
            if (req_arr && req_arr->type == NC_JSON_ARRAY && req_arr->array.count > 0) {
                off += snprintf(buf + off, bufsz - (size_t)off, ",\"required\":[");
                for (int k = 0; k < req_arr->array.count; k++) {
                    if (k > 0) buf[off++] = ',';
                    nc_str rname = req_arr->array.items[k].string;
                    off += snprintf(buf + off, bufsz - (size_t)off, "\"%.*s\"", NC_STR_ARG(rname));
                }
                off += snprintf(buf + off, bufsz - (size_t)off, "]");
            }
            off += snprintf(buf + off, bufsz - (size_t)off, "}");
        } else {
            off += snprintf(buf + off, bufsz - (size_t)off, "{\"type\":\"object\"}");
        }

        off += snprintf(buf + off, bufsz - (size_t)off, "}");
    }
    off += snprintf(buf + off, bufsz - (size_t)off, "]");

    nc_arena_free(&scratch);
    return buf;
}

/* Build the Anthropic messages array */
static int anthropic_build_messages(char *buf, size_t bufsz,
                                    const nc_message *msgs, int count) {
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "[");

    bool first = true;
    for (int i = 0; i < count; i++) {
        if ((size_t)off >= bufsz - 10) break;
        if (strcmp(msgs[i].role, "system") == 0) continue; /* handled separately */

        if (!first) buf[off++] = ',';
        first = false;

        if (msgs[i].tool_call_count > 0) {
            /* Assistant message with tool_use content blocks */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"assistant\",\"content\":[");

            bool first_block = true;
            /* Text block if content is non-empty */
            if (msgs[i].content && msgs[i].content[0]) {
                off += snprintf(buf + off, bufsz - (size_t)off, "{\"type\":\"text\",\"text\":\"");
                off = json_escape_into(buf, bufsz, off, msgs[i].content);
                off += snprintf(buf + off, bufsz - (size_t)off, "\"}");
                first_block = false;
            }

            /* tool_use blocks */
            for (int j = 0; j < msgs[i].tool_call_count; j++) {
                if (!first_block) buf[off++] = ',';
                first_block = false;
                const nc_tool_call *tc = &msgs[i].tool_calls[j];
                off += snprintf(buf + off, bufsz - (size_t)off,
                    "{\"type\":\"tool_use\",\"id\":\"%s\",\"name\":\"%s\",\"input\":%s}",
                    tc->id, tc->name, tc->arguments);
            }
            off += snprintf(buf + off, bufsz - (size_t)off, "]}");

        } else if (msgs[i].tool_call_id && msgs[i].tool_call_id[0]) {
            /* Tool result — Anthropic uses role=user with tool_result content blocks.
             * Multiple consecutive tool results should be merged into one user message.
             * Scan ahead to find all consecutive tool results. */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"user\",\"content\":[");

            int j = i;
            bool first_result = true;
            while (j < count && msgs[j].tool_call_id && msgs[j].tool_call_id[0]) {
                if (!first_result) buf[off++] = ',';
                first_result = false;
                off += snprintf(buf + off, bufsz - (size_t)off,
                    "{\"type\":\"tool_result\",\"tool_use_id\":\"%s\",\"content\":\"",
                    msgs[j].tool_call_id);
                off = json_escape_into(buf, bufsz, off, msgs[j].content);
                off += snprintf(buf + off, bufsz - (size_t)off, "\"}");
                j++;
            }
            /* Skip the messages we just consumed (outer loop will i++ past them) */
            i = j - 1;

            off += snprintf(buf + off, bufsz - (size_t)off, "]}");

        } else {
            /* Normal user/assistant message */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"%s\",\"content\":\"", msgs[i].role);
            off = json_escape_into(buf, bufsz, off, msgs[i].content);
            off += snprintf(buf + off, bufsz - (size_t)off, "\"}");
        }
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]");
    return off;
}

/* Parse tool_use blocks from Anthropic response content array */
static void anthropic_parse_tool_calls(nc_json *content_arr, nc_chat_response *resp) {
    if (!content_arr || content_arr->type != NC_JSON_ARRAY) return;

    for (int i = 0; i < content_arr->array.count; i++) {
        nc_json *block = &content_arr->array.items[i];
        nc_str btype = nc_json_str(nc_json_get(block, "type"), "");

        if (nc_str_eql(btype, "text")) {
            /* Accumulate text content */
            nc_str text = nc_json_str(nc_json_get(block, "text"), "");
            if (text.len > 0) {
                size_t cur = strlen(resp->content);
                size_t avail = sizeof(resp->content) - cur - 1;
                size_t cplen = text.len < avail ? text.len : avail;
                if (text.ptr && cplen > 0) {
                    memcpy(resp->content + cur, text.ptr, cplen);
                    resp->content[cur + cplen] = '\0';
                }
            }
        }
        else if (nc_str_eql(btype, "tool_use") && resp->tool_call_count < NC_MAX_TOOL_CALLS) {
            nc_tool_call *out = &resp->tool_calls[resp->tool_call_count];

            nc_str id = nc_json_str(nc_json_get(block, "id"), "");
            if (id.len > 0) {
                size_t cl = id.len < sizeof(out->id) - 1 ? id.len : sizeof(out->id) - 1;
                memcpy(out->id, id.ptr, cl);
                out->id[cl] = '\0';
            }

            nc_str name = nc_json_str(nc_json_get(block, "name"), "");
            if (name.len > 0) {
                size_t cl = name.len < sizeof(out->name) - 1 ? name.len : sizeof(out->name) - 1;
                memcpy(out->name, name.ptr, cl);
                out->name[cl] = '\0';
            }

            /* input is a JSON object — we need to serialize it back to a string.
             * Since our JSON lib doesn't have a generic serializer, we find the raw
             * substring in the response body. We store it as-is from the input field.
             * For now, re-build a minimal JSON from the parsed object. */
            nc_json *input = nc_json_get(block, "input");
            if (input && input->type == NC_JSON_OBJECT) {
                /* Build JSON string from the object */
                int aoff = 0;
                aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff, "{");
                for (int k = 0; k < input->object.count; k++) {
                    if (k > 0) out->arguments[aoff++] = ',';
                    nc_str key = input->object.keys[k];
                    nc_json *val = &input->object.vals[k];
                    aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff,
                        "\"%.*s\":", NC_STR_ARG(key));
                    if (val->type == NC_JSON_STRING) {
                        aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff,
                            "\"%.*s\"", NC_STR_ARG(val->string));
                    } else if (val->type == NC_JSON_NUMBER) {
                        if (val->number == (int)val->number)
                            aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff,
                                "%d", (int)val->number);
                        else
                            aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff,
                                "%.6f", val->number);
                    } else if (val->type == NC_JSON_BOOL) {
                        aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff,
                            "%s", val->boolean ? "true" : "false");
                    } else if (val->type == NC_JSON_NULL) {
                        aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff, "null");
                    } else {
                        /* Nested objects/arrays: emit as empty for now */
                        aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff, "null");
                    }
                }
                aoff += snprintf(out->arguments + aoff, sizeof(out->arguments) - (size_t)aoff, "}");
            } else {
                nc_strlcpy(out->arguments, "{}", sizeof(out->arguments));
            }

            resp->tool_call_count++;
        }
    }

    resp->has_tool_calls = resp->tool_call_count > 0;
}

static bool anthropic_chat(nc_provider *self, const nc_chat_request *req, nc_chat_response *resp) {
    provider_ctx *ctx = (provider_ctx *)self->ctx;
    memset(resp, 0, sizeof(*resp));

    /* Build messages JSON (Anthropic format) */
    size_t msgs_buf_sz = estimate_messages_size(req->messages, req->message_count);
    char *msgs_json = (char *)malloc(msgs_buf_sz);
    if (!msgs_json) return false;
    anthropic_build_messages(msgs_json, msgs_buf_sz, req->messages, req->message_count);

    /* Build tools JSON (Anthropic format: input_schema) */
    char *tools_buf = NULL;
    if (req->tools_json) {
        size_t tools_sz = strlen(req->tools_json) * 3 + 4096;
        tools_buf = (char *)malloc(tools_sz);
        if (tools_buf)
            anthropic_tools_json_from_defs(tools_buf, tools_sz, req);
    }

    /* Build request body */
    size_t body_sz = msgs_buf_sz + 2048 + (tools_buf ? strlen(tools_buf) : 0);
    char *body = (char *)malloc(body_sz);
    if (!body) { free(msgs_json); free(tools_buf); return false; }

    int off = 0;
    off += snprintf(body + off, body_sz - (size_t)off,
        "{\"model\":\"%s\",\"max_tokens\":%d,\"temperature\":%.2f,",
        req->model, req->max_tokens > 0 ? req->max_tokens : 4096, req->temperature);

    /* System message (Anthropic puts it at top level, not in messages) */
    for (int i = 0; i < req->message_count; i++) {
        if (strcmp(req->messages[i].role, "system") == 0 && req->messages[i].content) {
            off += snprintf(body + off, body_sz - (size_t)off, "\"system\":\"");
            off = json_escape_into(body, body_sz, off, req->messages[i].content);
            off += snprintf(body + off, body_sz - (size_t)off, "\",");
            break;
        }
    }

    /* Tools */
    if (tools_buf)
        off += snprintf(body + off, body_sz - (size_t)off, "\"tools\":%s,", tools_buf);

    /* Messages */
    off += snprintf(body + off, body_sz - (size_t)off, "\"messages\":%s}", msgs_json);

    /* Headers */
    char auth_hdr[300];
    snprintf(auth_hdr, sizeof(auth_hdr), "x-api-key: %s", ctx->api_key);
    const char *headers[] = {
        "Content-Type: application/json",
        auth_hdr,
        "anthropic-version: 2023-06-01",
    };

    char url[512];
    if (ctx->api_url[0])
        snprintf(url, sizeof(url), "%s/messages", ctx->api_url);
    else
        snprintf(url, sizeof(url), "https://api.anthropic.com/v1/messages");

    bool result = false;
    nc_http_response http_resp;
    if (!nc_http_post(url, body, (size_t)off, headers, 3, &http_resp)) {
        goto cleanup;
    }

    if (http_resp.status != 200) {
        nc_log(NC_LOG_ERROR, "Anthropic returned HTTP %d: %.200s", http_resp.status, http_resp.body);
        goto cleanup;
    }

    /* Parse response */
    {
        nc_arena a;
        nc_arena_init(&a, http_resp.body_len * 2 + 1024);
        nc_json *root = nc_json_parse(&a, http_resp.body, http_resp.body_len);

        if (root) {
            /* Parse all content blocks (text + tool_use) */
            nc_json *content_arr = nc_json_get(root, "content");
            anthropic_parse_tool_calls(content_arr, resp);

            /* Usage */
            nc_json *usage = nc_json_get(root, "usage");
            if (usage) {
                resp->prompt_tokens = (int)nc_json_num(nc_json_get(usage, "input_tokens"), 0);
                resp->completion_tokens = (int)nc_json_num(nc_json_get(usage, "output_tokens"), 0);
            }
        }

        nc_arena_free(&a);
    }
    result = true;

cleanup:
    nc_http_response_free(&http_resp);
    free(msgs_json);
    free(tools_buf);
    free(body);
    return result;
}

nc_provider nc_provider_anthropic(const char *api_key, const char *api_url) {
    provider_ctx *ctx = (provider_ctx *)calloc(1, sizeof(provider_ctx));
    if (!ctx) return (nc_provider){ .name = "anthropic", .ctx = NULL, .chat = NULL, .free = NULL };
    if (api_key) nc_strlcpy(ctx->api_key, api_key, sizeof(ctx->api_key));
    if (api_url) nc_strlcpy(ctx->api_url, api_url, sizeof(ctx->api_url));

    return (nc_provider){
        .name = "anthropic",
        .ctx  = ctx,
        .chat = anthropic_chat,
        .free = provider_free,
    };
}

#ifdef NC_TEST
void nc_test_provider(void) {
    char escaped[128] = {0};
    const char invalid[] = { 'x', (char)0xff, 'y', '\0' };

    int off = json_escape_into(escaped, sizeof(escaped), 0,
                               "r\xc3\xa9sultat: \xe4\xbd\xa0\xe5\xa5\xbd \xf0\x9f\x91\x8b");
    escaped[off] = '\0';
    NC_ASSERT(strcmp(escaped, "r\xc3\xa9sultat: \xe4\xbd\xa0\xe5\xa5\xbd \xf0\x9f\x91\x8b") == 0,
              "provider preserves valid UTF-8");

    off = json_escape_into(escaped, sizeof(escaped), 0, invalid);
    escaped[off] = '\0';
    NC_ASSERT(strcmp(escaped, "x\\ufffdy") == 0,
              "provider replaces malformed UTF-8");

    nc_message msg = {
        .role = "tool",
        .content = "\xe7\xbb\x93\xe6\x9e\x9c \xf0\x9f\x91\x8b",
        .tool_call_id = "call_utf8",
    };
    char messages[512];
    int messages_len = openai_build_messages(messages, sizeof(messages), &msg, 1);

    nc_arena a;
    nc_arena_init(&a, 1024);
    nc_json *arr = nc_json_parse(&a, messages, (size_t)messages_len);
    nc_json *content = arr && arr->type == NC_JSON_ARRAY && arr->array.count == 1
        ? nc_json_get(&arr->array.items[0], "content") : NULL;
    NC_ASSERT(content && nc_str_eql(nc_json_str(content, ""),
                                    "\xe7\xbb\x93\xe6\x9e\x9c \xf0\x9f\x91\x8b"),
              "provider serializes UTF-8 tool result as valid JSON");
    nc_arena_free(&a);
}
#endif
