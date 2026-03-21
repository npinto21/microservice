#include "context.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static void ms_headers_free(MS_Header *headers, int count) {
    int i;
    if (!headers) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(headers[i].name);
        free(headers[i].value);
    }
    free(headers);
}

static void ms_params_free(MS_Param *params, int count) {
    int i;
    if (!params) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(params[i].name);
        free(params[i].value);
    }
    free(params);
}

static char *ms_copy_text_local(const char *text) {
    size_t len;
    char *out;
    if (!text) {
        return NULL;
    }
    len = strlen(text);
    out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, len + 1);
    return out;
}

static long long ms_now_ms_local(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

int ms_context_poll_cancelled(MS_Context *ctx) {
    if (!ctx) {
        return 0;
    }
    if (!ctx->cancelled && ctx->deadline_ms > 0 && ms_now_ms_local() > ctx->deadline_ms) {
        ctx->cancelled = 1;
        ctx->cancel_reason = "request timeout";
    }
    return ctx->cancelled;
}

static int ms_header_append(MS_Header **headers, int *count, const char *name, const char *value) {
    MS_Header *grown;
    if (!headers || !count || !name || !value) {
        return 0;
    }
    grown = realloc(*headers, (size_t)(*count + 1) * sizeof(MS_Header));
    if (!grown) {
        return 0;
    }
    *headers = grown;
    (*headers)[*count].name = ms_copy_text_local(name);
    (*headers)[*count].value = ms_copy_text_local(value);
    (*count)++;
    return 1;
}

static const char *ms_header_get(MS_Header *headers, int count, const char *name) {
    int i;
    if (!headers || !name) {
        return "";
    }
    for (i = 0; i < count; i++) {
        if (headers[i].name && strcmp(headers[i].name, name) == 0) {
            return headers[i].value ? headers[i].value : "";
        }
    }
    return "";
}

static const char *ms_query_lookup(const char *query, const char *name) {
    static char buffer[1024];
    const char *cursor;
    size_t name_len;
    if (!query || !name) {
        return "";
    }
    name_len = strlen(name);
    cursor = query;
    while (*cursor != '\0') {
        const char *pair_end = strchr(cursor, '&');
        const char *eq = strchr(cursor, '=');
        size_t value_len;
        if (!pair_end) {
            pair_end = cursor + strlen(cursor);
        }
        if (eq && eq < pair_end && (size_t)(eq - cursor) == name_len && strncmp(cursor, name, name_len) == 0) {
            value_len = (size_t)(pair_end - (eq + 1));
            if (value_len >= sizeof(buffer)) {
                value_len = sizeof(buffer) - 1;
            }
            memcpy(buffer, eq + 1, value_len);
            buffer[value_len] = '\0';
            return buffer;
        }
        cursor = *pair_end == '&' ? pair_end + 1 : pair_end;
    }
    return "";
}

void ms_context_init(MS_Context *ctx) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
}

void ms_context_reset(MS_Context *ctx) {
    if (!ctx) {
        return;
    }
    ctx->current = NULL;
    ctx->user_data = NULL;
    ctx->next = NULL;
}

const char *ms_context_header(MS_Context *ctx, const char *name) {
    ms_context_poll_cancelled(ctx);
    if (!ctx || !ctx->req) {
        return "";
    }
    return ms_header_get(ctx->req->headers, ctx->req->header_count, name);
}

const char *ms_context_query(MS_Context *ctx, const char *name) {
    ms_context_poll_cancelled(ctx);
    if (!ctx || !ctx->req) {
        return "";
    }
    return ms_query_lookup(ctx->req->query_string, name);
}

const char *ms_context_param(MS_Context *ctx, const char *name) {
    int i;
    ms_context_poll_cancelled(ctx);
    if (!ctx || !ctx->req || !name) {
        return "";
    }
    for (i = 0; i < ctx->req->param_count; i++) {
        if (ctx->req->params[i].name && strcmp(ctx->req->params[i].name, name) == 0) {
            return ctx->req->params[i].value ? ctx->req->params[i].value : "";
        }
    }
    return "";
}

const char *ms_context_body(MS_Context *ctx) {
    ms_context_poll_cancelled(ctx);
    if (!ctx || !ctx->req || !ctx->req->body) {
        return "{}";
    }
    return ctx->req->body;
}

int ms_context_status(MS_Context *ctx, int status_code) {
    if (ms_context_poll_cancelled(ctx)) {
        return 0;
    }
    if (!ctx || !ctx->res) {
        return 0;
    }
    ctx->res->status_code = status_code;
    return 1;
}

int ms_context_json(MS_Context *ctx, const char *json_body) {
    if (ms_context_poll_cancelled(ctx)) {
        return 0;
    }
    if (!ctx || !ctx->res) {
        return 0;
    }
    free(ctx->res->content_type);
    free(ctx->res->body);
    ctx->res->content_type = ms_copy_text_local("application/json");
    ctx->res->body = ms_copy_text_local(json_body ? json_body : "{}");
    return 1;
}

int ms_context_set_header(MS_Context *ctx, const char *name, const char *value) {
    if (ms_context_poll_cancelled(ctx)) {
        return 0;
    }
    if (!ctx || !ctx->res) {
        return 0;
    }
    return ms_header_append(&ctx->res->headers, &ctx->res->header_count, name, value);
}

int ms_context_is_cancelled(MS_Context *ctx) {
    return ms_context_poll_cancelled(ctx);
}

const char *ms_context_cancel_reason(MS_Context *ctx) {
    if (!ctx || !ctx->cancel_reason) {
        return "";
    }
    return ctx->cancel_reason;
}

long long ms_context_deadline_ms(MS_Context *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->deadline_ms;
}

const char *ms_context_request_id(MS_Context *ctx) {
    if (!ctx || !ctx->request_id) {
        return "";
    }
    return ctx->request_id;
}

const char *ms_context_trace_id(MS_Context *ctx) {
    if (!ctx || !ctx->trace_id) {
        return "";
    }
    return ctx->trace_id;
}

const char *ms_context_span_id(MS_Context *ctx) {
    if (!ctx || !ctx->span_id) {
        return "";
    }
    return ctx->span_id;
}

const char *ms_context_correlation_id(MS_Context *ctx) {
    if (!ctx || !ctx->correlation_id) {
        return "";
    }
    return ctx->correlation_id;
}

void ms_request_free(MS_Request *req) {
    if (!req) {
        return;
    }
    free(req->method);
    free(req->path);
    free(req->http_version);
    free(req->query_string);
    free(req->body);
    ms_headers_free(req->headers, req->header_count);
    ms_params_free(req->params, req->param_count);
    free(req);
}

void ms_response_free(MS_Response *res) {
    if (!res) {
        return;
    }
    free(res->content_type);
    free(res->body);
    ms_headers_free(res->headers, res->header_count);
    free(res);
}
