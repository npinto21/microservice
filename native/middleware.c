#include "middleware.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    MS_CorsConfig cors;
    int window_seconds;
    int max_requests;
} MS_MiddlewareConfig;

static int ms_logger_middleware_fn(void *userdata, MS_Context *ctx) {
    (void)userdata;
    (void)ctx;
    return 1;
}

static int ms_cors_middleware_fn(void *userdata, MS_Context *ctx) {
    MS_MiddlewareConfig *config = (MS_MiddlewareConfig *)userdata;
    if (!ctx || !ctx->res || !config || !config->cors.enabled) {
        return 1;
    }
    ms_context_set_header(ctx, "Access-Control-Allow-Origin",
                          (config->cors.origin_count > 0 && config->cors.origins && config->cors.origins[0])
                              ? config->cors.origins[0]
                              : "*");
    ms_context_set_header(ctx, "Access-Control-Allow-Methods",
                          (config->cors.method_count > 0 && config->cors.methods && config->cors.methods[0])
                              ? config->cors.methods[0]
                              : "GET,POST,PUT,DELETE,OPTIONS");
    ms_context_set_header(ctx, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    return 1;
}

static int ms_compress_middleware_fn(void *userdata, MS_Context *ctx) {
    (void)userdata;
    (void)ctx;
    return 1;
}

static int ms_rate_limit_middleware_fn(void *userdata, MS_Context *ctx) {
    (void)userdata;
    (void)ctx;
    return 1;
}

static MS_Middleware *ms_middleware_new(const char *name) {
    MS_Middleware *middleware = calloc(1, sizeof(MS_Middleware));
    if (!middleware) {
        return NULL;
    }
    if (name) {
        middleware->name = strdup(name);
    }
    return middleware;
}

static void ms_cors_config_copy(MS_CorsConfig *dst, const MS_CorsConfig *src) {
    int i;
    if (!dst || !src) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    dst->enabled = src->enabled;
    dst->origin_count = src->origin_count;
    dst->method_count = src->method_count;
    if (src->origin_count > 0 && src->origins) {
        dst->origins = calloc((size_t)src->origin_count, sizeof(char *));
        for (i = 0; i < src->origin_count; i++) {
            ((char **)dst->origins)[i] = src->origins[i] ? strdup(src->origins[i]) : NULL;
        }
    }
    if (src->method_count > 0 && src->methods) {
        dst->methods = calloc((size_t)src->method_count, sizeof(char *));
        for (i = 0; i < src->method_count; i++) {
            ((char **)dst->methods)[i] = src->methods[i] ? strdup(src->methods[i]) : NULL;
        }
    }
}

static void ms_cors_config_free(MS_CorsConfig *config) {
    int i;
    if (!config) {
        return;
    }
    for (i = 0; i < config->origin_count; i++) {
        free((char *)config->origins[i]);
    }
    for (i = 0; i < config->method_count; i++) {
        free((char *)config->methods[i]);
    }
    free((char **)config->origins);
    free((char **)config->methods);
}

static void *ms_middleware_userdata_clone(const MS_Middleware *middleware) {
    MS_MiddlewareConfig *copy;
    const MS_MiddlewareConfig *source;
    if (!middleware || !middleware->userdata) {
        return NULL;
    }
    source = (const MS_MiddlewareConfig *)middleware->userdata;
    copy = calloc(1, sizeof(MS_MiddlewareConfig));
    if (!copy) {
        return NULL;
    }
    copy->window_seconds = source->window_seconds;
    copy->max_requests = source->max_requests;
    ms_cors_config_copy(&copy->cors, &source->cors);
    return copy;
}

int ms_middleware_append(MS_Middleware **head, MS_Middleware *middleware) {
    MS_Middleware *cursor;
    if (!head || !middleware) {
        return 0;
    }
    if (!*head) {
        *head = middleware;
        return 1;
    }
    cursor = *head;
    while (cursor->next) {
        cursor = cursor->next;
    }
    cursor->next = middleware;
    return 1;
}

void ms_middleware_attach_chain(MS_Middleware **head, MS_Middleware *chain) {
    MS_Middleware *cursor;
    if (!head || !chain) {
        return;
    }
    if (!*head) {
        *head = chain;
        return;
    }
    cursor = *head;
    while (cursor->next) {
        cursor = cursor->next;
    }
    cursor->next = chain;
}

MS_Middleware *ms_middleware_clone_chain(MS_Middleware *head) {
    MS_Middleware *clone_head = NULL;
    MS_Middleware *clone_tail = NULL;
    while (head) {
        MS_Middleware *copy = ms_middleware_new(head->name);
        if (!copy) {
            ms_middleware_free(clone_head);
            return NULL;
        }
        copy->fn = head->fn;
        copy->userdata = ms_middleware_userdata_clone(head);
        if (!clone_head) {
            clone_head = copy;
            clone_tail = copy;
        } else {
            clone_tail->next = copy;
            clone_tail = copy;
        }
        head = head->next;
    }
    return clone_head;
}

int ms_middleware_execute_chain(MS_Middleware *head, MS_Context *ctx) {
    while (head) {
        if (ms_context_poll_cancelled(ctx)) {
            return 0;
        }
        if (head->fn && !head->fn(head->userdata, ctx)) {
            return 0;
        }
        if (ms_context_poll_cancelled(ctx)) {
            return 0;
        }
        head = head->next;
    }
    return 1;
}

int ms_middleware_chain_has_name(MS_Middleware *head, const char *name) {
    while (head) {
        if (head->name && name && strcmp(head->name, name) == 0) {
            return 1;
        }
        head = head->next;
    }
    return 0;
}

MS_Middleware *ms_middleware_logger(void) {
    MS_Middleware *middleware = ms_middleware_new("logger");
    if (middleware) {
        middleware->fn = ms_logger_middleware_fn;
    }
    return middleware;
}

MS_Middleware *ms_middleware_cors(const MS_CorsConfig *config) {
    MS_Middleware *middleware = ms_middleware_new("cors");
    MS_MiddlewareConfig *state;
    if (!middleware) {
        return NULL;
    }
    state = calloc(1, sizeof(MS_MiddlewareConfig));
    if (config) {
        ms_cors_config_copy(&state->cors, config);
    }
    middleware->fn = ms_cors_middleware_fn;
    middleware->userdata = state;
    return middleware;
}

MS_Middleware *ms_middleware_compress(void) {
    MS_Middleware *middleware = ms_middleware_new("compress");
    if (middleware) {
        middleware->fn = ms_compress_middleware_fn;
    }
    return middleware;
}

MS_Middleware *ms_middleware_rate_limit(int window_seconds, int max_requests) {
    MS_Middleware *middleware = ms_middleware_new("rate_limit");
    MS_MiddlewareConfig *state;
    if (!middleware) {
        return NULL;
    }
    state = calloc(1, sizeof(MS_MiddlewareConfig));
    state->window_seconds = window_seconds;
    state->max_requests = max_requests;
    middleware->fn = ms_rate_limit_middleware_fn;
    middleware->userdata = state;
    return middleware;
}

void ms_middleware_free(MS_Middleware *middleware) {
    while (middleware) {
        MS_Middleware *next = middleware->next;
        free(middleware->name);
        if (middleware->userdata) {
            ms_cors_config_free(&((MS_MiddlewareConfig *)middleware->userdata)->cors);
        }
        free(middleware->userdata);
        free(middleware);
        middleware = next;
    }
}
