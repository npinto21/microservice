#include "router.h"

#include <stdlib.h>
#include <string.h>

static char *ms_copy_text_n_local(const char *text, size_t len) {
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static void ms_request_clear_params(MS_Request *req) {
    int i;
    if (!req || !req->params) {
        return;
    }
    for (i = 0; i < req->param_count; i++) {
        free(req->params[i].name);
        free(req->params[i].value);
    }
    free(req->params);
    req->params = NULL;
    req->param_count = 0;
}

static int ms_request_add_param(MS_Request *req, const char *name, size_t name_len, const char *value, size_t value_len) {
    MS_Param *grown;
    if (!req || !name || !value) {
        return 0;
    }
    grown = realloc(req->params, (size_t)(req->param_count + 1) * sizeof(MS_Param));
    if (!grown) {
        return 0;
    }
    req->params = grown;
    req->params[req->param_count].name = ms_copy_text_n_local(name, name_len);
    req->params[req->param_count].value = ms_copy_text_n_local(value, value_len);
    req->param_count++;
    return 1;
}

static int ms_path_match_with_params(const char *pattern, const char *path, MS_Request *req) {
    const char *p_cursor = pattern;
    const char *v_cursor = path;
    while (*p_cursor != '\0' && *v_cursor != '\0') {
        const char *p_next;
        const char *v_next;
        size_t p_len;
        size_t v_len;

        while (*p_cursor == '/') p_cursor++;
        while (*v_cursor == '/') v_cursor++;

        p_next = strchr(p_cursor, '/');
        v_next = strchr(v_cursor, '/');
        p_next = p_next ? p_next : p_cursor + strlen(p_cursor);
        v_next = v_next ? v_next : v_cursor + strlen(v_cursor);
        p_len = (size_t)(p_next - p_cursor);
        v_len = (size_t)(v_next - v_cursor);

        if (p_len == 0 && v_len == 0) {
            break;
        }
        if (p_len == 0 || v_len == 0) {
            return 0;
        }

        if (p_cursor[0] == ':') {
            if (!ms_request_add_param(req, p_cursor + 1, p_len - 1, v_cursor, v_len)) {
                return 0;
            }
        } else if (!(p_len == v_len && strncmp(p_cursor, v_cursor, p_len) == 0)) {
            return 0;
        }

        p_cursor = p_next;
        v_cursor = v_next;
    }

    while (*p_cursor == '/') p_cursor++;
    while (*v_cursor == '/') v_cursor++;
    return *p_cursor == '\0' && *v_cursor == '\0';
}

int ms_router_add_route(MS_Route **head, MS_HttpMethod method, const char *path, MS_HandlerFn handler, void *userdata) {
    MS_Route *route;
    if (!head || !path || !handler) {
        return 0;
    }
    route = calloc(1, sizeof(MS_Route));
    if (!route) {
        return 0;
    }
    route->method = method;
    route->path = strdup(path);
    route->handler = handler;
    route->handler_userdata = userdata;
    route->next = *head;
    *head = route;
    return 1;
}

MS_Route *ms_router_match(MS_Route *head, const char *path, MS_Request *req) {
    MS_Route *route = head;
    while (route) {
        if (route->path && path && strcmp(route->path, path) == 0) {
            return route;
        }
        if (route->path && path && req && strchr(route->path, ':')) {
            ms_request_clear_params(req);
            if (ms_path_match_with_params(route->path, path, req)) {
                return route;
            }
        }
        route = route->next;
    }
    return NULL;
}
