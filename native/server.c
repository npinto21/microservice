#include "server.h"
#include "context.h"
#include "middleware.h"
#include "observability.h"
#include "router.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * Important isolation rule:
 *
 * This module must not rely on mutable file-level or package-level globals for
 * request-processing state. Each MS_Server returned by ms_server_new() is an
 * independent instance, and every child object created from it must remain
 * attached to that instance only.
 */

#define MS_MAX_HEADER_BYTES 16384
#define MS_MAX_REQUEST_LINE_BYTES 8192

static char *ms_copy_text(const char *text) {
    size_t len;
    char *out;
    if (!text) {
        return NULL;
    }
    len = strlen(text);
    out = malloc(len + 1);
    memcpy(out, text, len + 1);
    return out;
}

static char *ms_copy_env_server(const char *name) {
    const char *text = getenv(name);
    if (!text || text[0] == '\0') {
        return NULL;
    }
    return ms_copy_text(text);
}

static int ms_env_int_server(const char *name, int fallback) {
    const char *text = getenv(name);
    char *end = NULL;
    long value;
    if (!text || text[0] == '\0') {
        return fallback;
    }
    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 0) {
        return fallback;
    }
    return (int)value;
}

static void ms_route_metrics_free(MS_RouteMetric *metric) {
    while (metric) {
        MS_RouteMetric *next = metric->next;
        free(metric->method);
        free(metric->route);
        free(metric);
        metric = next;
    }
}

static void ms_server_set_error(MS_Server *server, const char *message) {
    if (!server) {
        return;
    }
    free(server->last_error);
    server->last_error = ms_copy_text(message ? message : "");
}

static void ms_server_set_errno(MS_Server *server, const char *prefix) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s: %s", prefix, strerror(errno));
    ms_server_set_error(server, buffer);
}

static char *ms_copy_text_n(const char *text, size_t len) {
    char *out = malloc(len + 1);
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static long long ms_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

static int ms_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static const char *ms_request_header_value(const MS_Request *req, const char *name) {
    int i;
    if (!req || !name) {
        return "";
    }
    for (i = 0; i < req->header_count; i++) {
        if (req->headers[i].name && strcmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value ? req->headers[i].value : "";
        }
    }
    return "";
}

static int ms_header_exists(const MS_Request *req, const char *name) {
    return ms_request_header_value(req, name)[0] != '\0';
}

static void ms_generate_request_id(char *buffer, size_t size, int client_fd) {
    unsigned long long now = (unsigned long long)ms_now_ms();
    snprintf(buffer, size, "%016llx%08x", now, client_fd);
}

static void ms_generate_trace_context(const MS_Request *req, char *trace_id, size_t trace_size, char *span_id, size_t span_size) {
    const char *traceparent = ms_request_header_value(req, "traceparent");
    if (traceparent && strlen(traceparent) >= 55) {
        snprintf(trace_id, trace_size, "%.*s", 32, traceparent + 3);
        snprintf(span_id, span_size, "%.*s", 16, traceparent + 36);
        return;
    }
    snprintf(trace_id, trace_size, "%016llx%016llx",
             (unsigned long long)ms_now_ms(),
             (unsigned long long)(uintptr_t)req);
    snprintf(span_id, span_size, "%016llx", (unsigned long long)(uintptr_t)req);
}

static const char *ms_pick_correlation_id(const MS_Request *req) {
    const char *value = ms_request_header_value(req, "x-correlation-id");
    if (value && value[0] != '\0') {
        return value;
    }
    value = ms_request_header_value(req, "X-Correlation-Id");
    if (value && value[0] != '\0') {
        return value;
    }
    value = ms_request_header_value(req, "x-request-id");
    if (value && value[0] != '\0') {
        return value;
    }
    value = ms_request_header_value(req, "X-Request-Id");
    if (value && value[0] != '\0') {
        return value;
    }
    return "";
}

static const char *ms_pick_request_id(const MS_Request *req) {
    const char *value = ms_request_header_value(req, "x-request-id");
    if (value && value[0] != '\0') {
        return value;
    }
    value = ms_request_header_value(req, "X-Request-Id");
    if (value && value[0] != '\0') {
        return value;
    }
    return "";
}

static void ms_build_traceparent(const char *trace_id, const char *span_id, char *buffer, size_t size) {
    snprintf(buffer, size, "00-%s-%s-01", trace_id ? trace_id : "", span_id ? span_id : "");
}

static int ms_is_http_token_char(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
           c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
           c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

static int ms_content_length_from_buffer(const char *buffer, size_t header_size, size_t *content_length) {
    const char *cursor = buffer;
    const char *end = buffer + header_size;
    *content_length = 0;
    while (cursor < end) {
        const char *line_end = strstr(cursor, "\r\n");
        if (!line_end || line_end > end) {
            break;
        }
        if ((size_t)(line_end - cursor) > 15 && strncmp(cursor, "Content-Length:", 15) == 0) {
            const char *value = cursor + 15;
            const char *scan;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            if (value >= line_end) {
                return -1;
            }
            scan = value;
            while (scan < line_end) {
                if (*scan < '0' || *scan > '9') {
                    return -1;
                }
                scan++;
            }
            *content_length = (size_t)strtoull(value, NULL, 10);
            return 1;
        }
        cursor = line_end + 2;
    }
    return 0;
}

static int ms_should_keep_alive(const MS_Request *req) {
    const char *connection;
    if (!req || !req->http_version) {
        return 0;
    }
    connection = ms_request_header_value(req, "Connection");
    if (connection[0] == '\0') {
        connection = ms_request_header_value(req, "connection");
    }
    if (strcmp(req->http_version, "HTTP/1.1") == 0) {
        return !(connection[0] != '\0' && strcasecmp(connection, "close") == 0);
    }
    if (strcmp(req->http_version, "HTTP/1.0") == 0) {
        return connection[0] != '\0' && strcasecmp(connection, "keep-alive") == 0;
    }
    return 0;
}

static int ms_read_request_buffer(MS_Server *server, int client_fd, char **out_buffer) {
    struct pollfd pfd;
    char *buffer = NULL;
    size_t capacity = 0;
    size_t used = 0;
    size_t header_size = 0;
    size_t content_length = 0;
    int have_headers = 0;
    int timeout_ms;
    *out_buffer = NULL;
    timeout_ms = (server->config.read_timeout_seconds > 0 ? server->config.read_timeout_seconds : 10) * 1000;
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    while (!server->stop_requested) {
        if (server->shutdown_deadline_ms > 0 && ms_now_ms() > server->shutdown_deadline_ms) {
            free(buffer);
            return 0;
        }
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready == 0) {
            free(buffer);
            return 0;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            return -1;
        }
        if ((pfd.revents & POLLIN) == 0) {
            free(buffer);
            return -1;
        }
        if (used + 4096 + 1 > capacity) {
            capacity = capacity == 0 ? 8192 : capacity * 2;
            buffer = realloc(buffer, capacity);
            if (!buffer) {
                return -1;
            }
        }
        {
            ssize_t received = recv(client_fd, buffer + used, capacity - used - 1, 0);
            if (received == 0) {
                break;
            }
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                free(buffer);
                return -1;
            }
            used += (size_t)received;
            buffer[used] = '\0';
        }
        if (!have_headers) {
            char *headers_end = strstr(buffer, "\r\n\r\n");
            if (!headers_end && used > MS_MAX_HEADER_BYTES) {
                free(buffer);
                return 3;
            }
            if (headers_end) {
                have_headers = 1;
                header_size = (size_t)((headers_end - buffer) + 4);
                if (header_size > MS_MAX_HEADER_BYTES) {
                    free(buffer);
                    return 3;
                }
                {
                    int content_length_status = ms_content_length_from_buffer(buffer, header_size, &content_length);
                    if (content_length_status < 0) {
                        free(buffer);
                        return 3;
                    }
                }
                if (content_length > server->config.max_body_size && server->config.max_body_size > 0) {
                    *out_buffer = buffer;
                    return 2;
                }
            }
        }
        if (have_headers) {
            if (used >= header_size + content_length) {
                *out_buffer = buffer;
                return 1;
            }
        }
    }
    if (buffer) {
        buffer[used] = '\0';
    }
    *out_buffer = buffer;
    return buffer ? 1 : -1;
}

static const char *ms_reason_phrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

static int ms_status_class_index(int status_code) {
    if (status_code >= 100 && status_code < 600) {
        return status_code / 100;
    }
    return 0;
}

static const char *ms_method_name(MS_HttpMethod method) {
    switch (method) {
        case MS_HTTP_GET: return "GET";
        case MS_HTTP_POST: return "POST";
        case MS_HTTP_PUT: return "PUT";
        case MS_HTTP_DELETE: return "DELETE";
        case MS_HTTP_HEAD: return "HEAD";
        case MS_HTTP_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

static MS_RouteMetric *ms_metrics_get_route(MS_Server *server, const char *method, const char *route) {
    MS_RouteMetric *cursor;
    if (!server || !server->metrics || !method || !route) {
        return NULL;
    }
    cursor = server->metrics->routes;
    while (cursor) {
        if (strcmp(cursor->method, method) == 0 && strcmp(cursor->route, route) == 0) {
            return cursor;
        }
        cursor = cursor->next;
    }
    cursor = calloc(1, sizeof(MS_RouteMetric));
    if (!cursor) {
        return NULL;
    }
    cursor->method = ms_copy_text(method);
    cursor->route = ms_copy_text(route);
    cursor->next = server->metrics->routes;
    server->metrics->routes = cursor;
    return cursor;
}

static void ms_metrics_observe_route(MS_RouteMetric *metric, long long duration_ms, int status_code, int cancelled) {
    if (!metric) {
        return;
    }
    metric->requests_total++;
    if (status_code >= 400) {
        metric->errors_total++;
    }
    if (cancelled) {
        metric->cancelled_total++;
    }
    if (duration_ms <= 10) {
        metric->duration_le_10ms++;
    } else if (duration_ms <= 50) {
        metric->duration_le_50ms++;
    } else if (duration_ms <= 100) {
        metric->duration_le_100ms++;
    } else if (duration_ms <= 500) {
        metric->duration_le_500ms++;
    } else if (duration_ms <= 1000) {
        metric->duration_le_1000ms++;
    } else {
        metric->duration_gt_1000ms++;
    }
}

static int ms_add_header(MS_Header **headers, int *count, const char *name, const char *value) {
    MS_Header *grown;
    if (!headers || !count || !name || !value) {
        return 0;
    }
    grown = realloc(*headers, (size_t)(*count + 1) * sizeof(MS_Header));
    if (!grown) {
        return 0;
    }
    *headers = grown;
    (*headers)[*count].name = ms_copy_text(name);
    (*headers)[*count].value = ms_copy_text(value);
    (*count)++;
    return 1;
}

static MS_Request *ms_parse_request(const char *buffer) {
    const char *line_end;
    const char *method_end;
    const char *path_end;
    const char *headers_start;
    const char *body_start;
    const char *cursor;
    const char *query_at;
    MS_Request *req;

    if (!buffer) {
        return NULL;
    }

    line_end = strstr(buffer, "\r\n");
    if (!line_end) {
        return NULL;
    }
    if ((size_t)(line_end - buffer) > MS_MAX_REQUEST_LINE_BYTES) {
        return NULL;
    }
    method_end = strchr(buffer, ' ');
    if (!method_end || method_end >= line_end) {
        return NULL;
    }
    path_end = strchr(method_end + 1, ' ');
    if (!path_end || path_end >= line_end) {
        return NULL;
    }
    if (strncmp(path_end + 1, "HTTP/1.1", 8) != 0 && strncmp(path_end + 1, "HTTP/1.0", 8) != 0) {
        return NULL;
    }
    {
        const char *scan = buffer;
        while (scan < method_end) {
            if (!ms_is_http_token_char(*scan)) {
                return NULL;
            }
            scan++;
        }
    }
    if (*(method_end + 1) != '/' && *(method_end + 1) != '*') {
        return NULL;
    }

    req = calloc(1, sizeof(MS_Request));
    if (!req) {
        return NULL;
    }

    req->method = ms_copy_text_n(buffer, (size_t)(method_end - buffer));
    req->http_version = ms_copy_text_n(path_end + 1, (size_t)(line_end - (path_end + 1)));
    query_at = memchr(method_end + 1, '?', (size_t)(path_end - (method_end + 1)));
    if (query_at) {
        req->path = ms_copy_text_n(method_end + 1, (size_t)(query_at - (method_end + 1)));
        req->query_string = ms_copy_text_n(query_at + 1, (size_t)(path_end - (query_at + 1)));
    } else {
        req->path = ms_copy_text_n(method_end + 1, (size_t)(path_end - (method_end + 1)));
        req->query_string = ms_copy_text("");
    }

    headers_start = line_end + 2;
    body_start = strstr(headers_start, "\r\n\r\n");
    if (!body_start) {
        ms_request_free(req);
        return NULL;
    }
    cursor = headers_start;
    while (cursor < body_start) {
        const char *header_end = strstr(cursor, "\r\n");
        const char *colon;
        if (!header_end || header_end > body_start) {
            break;
        }
        if (header_end == cursor) {
            break;
        }
        colon = memchr(cursor, ':', (size_t)(header_end - cursor));
        if (!colon || colon == cursor) {
            ms_request_free(req);
            return NULL;
        }
        {
            const char *scan = cursor;
            const char *value_start = colon + 1;
            char *header_name;
            char *header_value;
            while (scan < colon) {
                if (!ms_is_http_token_char(*scan)) {
                    ms_request_free(req);
                    return NULL;
                }
                scan++;
            }
            while (value_start < header_end && (*value_start == ' ' || *value_start == '\t')) {
                value_start++;
            }
            header_name = ms_copy_text_n(cursor, (size_t)(colon - cursor));
            header_value = value_start < header_end
                ? ms_copy_text_n(value_start, (size_t)(header_end - value_start))
                : ms_copy_text("");
            if (!ms_add_header(&req->headers, &req->header_count, header_name, header_value)) {
                free(header_name);
                free(header_value);
                ms_request_free(req);
                return NULL;
            }
            free(header_name);
            free(header_value);
        }
        cursor = header_end + 2;
    }
    body_start += 4;
    req->body = ms_copy_text(body_start);
    if (strcmp(req->http_version, "HTTP/1.1") == 0 && !ms_header_exists(req, "Host") && !ms_header_exists(req, "host")) {
        ms_request_free(req);
        return NULL;
    }
    req->keep_alive_requested = ms_should_keep_alive(req);
    return req;
}

static MS_Response *ms_response_new(void) {
    MS_Response *res = calloc(1, sizeof(MS_Response));
    if (!res) {
        return NULL;
    }
    res->status_code = 200;
    res->content_type = ms_copy_text("application/json");
    res->body = ms_copy_text("{}");
    res->close_connection = 1;
    return res;
}

static void ms_response_set_body(MS_Response *res, const char *content_type, const char *body) {
    if (!res) {
        return;
    }
    free(res->content_type);
    free(res->body);
    res->content_type = ms_copy_text(content_type ? content_type : "application/json");
    res->body = ms_copy_text(body ? body : "");
}

static void ms_metrics_render(MS_Server *server, char *buffer, size_t size) {
    int used = 0;
    int i;
    MS_RouteMetric *route_metric;
    static const char *status_class_labels[] = {"unknown", "1xx", "2xx", "3xx", "4xx", "5xx"};

    if (!buffer || size == 0) {
        return;
    }

    used += snprintf(buffer + used, size - (size_t)used,
             "# TYPE http_requests_total counter\n"
             "http_requests_total %llu\n"
             "# TYPE http_errors_total counter\n"
             "http_errors_total %llu\n"
             "# TYPE http_requests_in_progress gauge\n"
             "http_requests_in_progress %llu\n"
             "# TYPE http_requests_cancelled_total counter\n"
             "http_requests_cancelled_total %llu\n"
             "# TYPE active_connections gauge\n"
             "active_connections %d\n"
             "# TYPE http_requests_by_method_total counter\n",
             (unsigned long long)(server && server->metrics ? server->metrics->http_requests_total : 0),
             (unsigned long long)(server && server->metrics ? server->metrics->http_errors_total : 0),
             (unsigned long long)(server && server->metrics ? server->metrics->active_requests : 0),
             (unsigned long long)(server && server->metrics ? server->metrics->http_cancelled_total : 0),
             server ? server->active_connections : 0);
    for (i = 0; server && server->metrics && i < MS_HTTP_METHOD_COUNT && used < (int)size; i++) {
        used += snprintf(buffer + used, size - (size_t)used,
                         "http_requests_by_method_total{method=\"%s\"} %llu\n",
                         ms_method_name((MS_HttpMethod)i),
                         (unsigned long long)server->metrics->http_requests_by_method[i]);
    }
    used += snprintf(buffer + used, size - (size_t)used,
                     "# TYPE http_responses_by_status_class_total counter\n");
    for (i = 0; server && server->metrics && i < 6 && used < (int)size; i++) {
        used += snprintf(buffer + used, size - (size_t)used,
                         "http_responses_by_status_class_total{class=\"%s\"} %llu\n",
                         status_class_labels[i],
                         (unsigned long long)server->metrics->http_responses_by_status_class[i]);
    }
    used += snprintf(buffer + used, size - (size_t)used,
                     "# TYPE http_requests_by_route_total counter\n"
                     "# TYPE http_errors_by_route_total counter\n"
                     "# TYPE http_cancelled_by_route_total counter\n"
                     "# TYPE http_request_duration_bucket_total counter\n");
    for (route_metric = server && server->metrics ? server->metrics->routes : NULL;
         route_metric && used < (int)size;
         route_metric = route_metric->next) {
        used += snprintf(buffer + used, size - (size_t)used,
                         "http_requests_by_route_total{method=\"%s\",route=\"%s\"} %llu\n"
                         "http_errors_by_route_total{method=\"%s\",route=\"%s\"} %llu\n"
                         "http_cancelled_by_route_total{method=\"%s\",route=\"%s\"} %llu\n"
                         "http_request_duration_bucket_total{method=\"%s\",route=\"%s\",le=\"10\"} %llu\n"
                         "http_request_duration_bucket_total{method=\"%s\",route=\"%s\",le=\"50\"} %llu\n"
                         "http_request_duration_bucket_total{method=\"%s\",route=\"%s\",le=\"100\"} %llu\n"
                         "http_request_duration_bucket_total{method=\"%s\",route=\"%s\",le=\"500\"} %llu\n"
                         "http_request_duration_bucket_total{method=\"%s\",route=\"%s\",le=\"1000\"} %llu\n"
                         "http_request_duration_bucket_total{method=\"%s\",route=\"%s\",le=\"+Inf\"} %llu\n",
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->requests_total,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->errors_total,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->cancelled_total,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->duration_le_10ms,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->duration_le_50ms,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->duration_le_100ms,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->duration_le_500ms,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->duration_le_1000ms,
                         route_metric->method, route_metric->route, (unsigned long long)route_metric->duration_gt_1000ms);
    }
}

static int ms_send_all(int client_fd, const char *buffer, size_t len, int timeout_ms, long long shutdown_deadline_ms) {
    size_t sent = 0;
    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLOUT;
    while (sent < len) {
        ssize_t wrote;
        int ready;
        if (shutdown_deadline_ms > 0 && ms_now_ms() > shutdown_deadline_ms) {
            return 0;
        }
        ready = poll(&pfd, 1, timeout_ms);
        if (ready == 0) {
            return 0;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if ((pfd.revents & POLLOUT) == 0) {
            return 0;
        }
        wrote = send(client_fd, buffer + sent, len - sent, 0);
        if (wrote < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return 0;
        }
        sent += (size_t)wrote;
    }
    return 1;
}

static int ms_send_response(MS_Server *server, int client_fd, const MS_Response *res, int suppress_body) {
    char header[4096];
    size_t body_len;
    int timeout_ms;
    int used;
    int i;
    if (!res) {
        return 0;
    }
    timeout_ms = ((server && server->config.write_timeout_seconds > 0) ? server->config.write_timeout_seconds : 10) * 1000;
    body_len = (!suppress_body && res->body) ? strlen(res->body) : 0;
    used = snprintf(header, sizeof(header),
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: %s\r\n",
                    res->status_code ? res->status_code : 200,
                    ms_reason_phrase(res->status_code ? res->status_code : 200),
                    res->content_type ? res->content_type : "application/json",
                    body_len,
                    res->close_connection ? "close" : "keep-alive");
    for (i = 0; i < res->header_count && used < (int)sizeof(header) - 4; i++) {
        used += snprintf(header + used, sizeof(header) - (size_t)used,
                         "%s: %s\r\n",
                         res->headers[i].name ? res->headers[i].name : "",
                         res->headers[i].value ? res->headers[i].value : "");
    }
    used += snprintf(header + used, sizeof(header) - (size_t)used, "\r\n");
    if (!ms_send_all(client_fd, header, (size_t)used, timeout_ms, server ? server->shutdown_deadline_ms : 0)) {
        return 0;
    }
    if (!suppress_body && body_len > 0 &&
        !ms_send_all(client_fd, res->body, body_len, timeout_ms, server ? server->shutdown_deadline_ms : 0)) {
        return 0;
    }
    return 1;
}

static int ms_path_exists_for_method(MS_Route *routes, const char *path) {
    MS_Request probe;
    MS_Route *match;
    int exists;
    int i;
    memset(&probe, 0, sizeof(probe));
    match = ms_router_match(routes, path, &probe);
    exists = match != NULL;
    for (i = 0; i < probe.param_count; i++) {
        free(probe.params[i].name);
        free(probe.params[i].value);
    }
    free(probe.params);
    return exists;
}

static void ms_append_allow_method(char *buffer, size_t size, const char *method, int *first) {
    if (!buffer || !method || !first) {
        return;
    }
    if (!*first) {
        strncat(buffer, ", ", size - strlen(buffer) - 1);
    }
    strncat(buffer, method, size - strlen(buffer) - 1);
    *first = 0;
}

static void ms_build_allow_header(MS_Server *server, const char *path, char *buffer, size_t size) {
    int first = 1;
    int has_get = ms_path_exists_for_method(server->routes[MS_HTTP_GET], path);
    if (!buffer || size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (has_get) {
        ms_append_allow_method(buffer, size, "GET", &first);
        ms_append_allow_method(buffer, size, "HEAD", &first);
    }
    if (ms_path_exists_for_method(server->routes[MS_HTTP_POST], path)) {
        ms_append_allow_method(buffer, size, "POST", &first);
    }
    if (ms_path_exists_for_method(server->routes[MS_HTTP_PUT], path)) {
        ms_append_allow_method(buffer, size, "PUT", &first);
    }
    if (ms_path_exists_for_method(server->routes[MS_HTTP_DELETE], path)) {
        ms_append_allow_method(buffer, size, "DELETE", &first);
    }
    if (ms_path_exists_for_method(server->routes[MS_HTTP_HEAD], path) && !has_get) {
        ms_append_allow_method(buffer, size, "HEAD", &first);
    }
    if (ms_path_exists_for_method(server->routes[MS_HTTP_OPTIONS], path)) {
        ms_append_allow_method(buffer, size, "OPTIONS", &first);
    } else {
        ms_append_allow_method(buffer, size, "OPTIONS", &first);
    }
}

static MS_HttpMethod ms_method_from_text(const char *method) {
    if (!method) return MS_HTTP_GET;
    if (strcmp(method, "GET") == 0) return MS_HTTP_GET;
    if (strcmp(method, "POST") == 0) return MS_HTTP_POST;
    if (strcmp(method, "PUT") == 0) return MS_HTTP_PUT;
    if (strcmp(method, "DELETE") == 0) return MS_HTTP_DELETE;
    if (strcmp(method, "HEAD") == 0) return MS_HTTP_HEAD;
    if (strcmp(method, "OPTIONS") == 0) return MS_HTTP_OPTIONS;
    return MS_HTTP_GET;
}

static int ms_context_is_timed_out(MS_Context *ctx) {
    if (!ctx) {
        return 0;
    }
    if (ctx->deadline_ms > 0 && ms_now_ms() > ctx->deadline_ms) {
        ctx->cancelled = 1;
        ctx->cancel_reason = "request timeout";
        return 1;
    }
    return 0;
}

static int ms_context_should_abort(MS_Server *server, MS_Context *ctx) {
    if (!ctx) {
        return 0;
    }
    if (ms_context_is_timed_out(ctx)) {
        return 1;
    }
    if (server && server->stop_requested) {
        ctx->cancelled = 1;
        ctx->cancel_reason = "server shutting down";
        return 1;
    }
    if (server && server->shutdown_deadline_ms > 0 && ms_now_ms() > server->shutdown_deadline_ms) {
        ctx->cancelled = 1;
        ctx->cancel_reason = "shutdown timeout";
        return 1;
    }
    return 0;
}

static int ms_handle_single_request(MS_Server *server, int client_fd, const struct sockaddr_in *client_addr, int *out_keep_alive) {
    char *buffer = NULL;
    MS_Request *req;
    MS_Response *res;
    MS_Context ctx;
    MS_Route *route;
    MS_HttpMethod method;
    int execute_handler = 1;
    int should_log = 0;
    int suppress_body = 0;
    int keep_alive = 0;
    char log_fields[512];
    char allow[256];
    char request_id[32];
    char correlation_id[64];
    char trace_id[40];
    char span_id[24];
    char traceparent[96];
    char ip[INET_ADDRSTRLEN];
    const char *user_agent;
    const char *route_label = "<unmatched>";
    MS_LogContext log_ctx;
    MS_RouteMetric *route_metric = NULL;
    char metrics_body[512];
    long long request_started_ms = ms_now_ms();
    long long duration_ms;
    int read_status = ms_read_request_buffer(server, client_fd, &buffer);

    if (read_status == 0) {
        res = ms_response_new();
        if (res) {
            res->status_code = server && server->stop_requested ? 503 : 408;
            free(res->body);
            res->body = ms_copy_text(server && server->stop_requested
                ? "{\"error\":\"server shutting down\"}"
                : "{\"error\":\"request timeout\"}");
            ms_send_response(server, client_fd, res, 0);
            ms_response_free(res);
        }
        if (out_keep_alive) *out_keep_alive = 0;
        return 0;
    }
    if (read_status == 3) {
        res = ms_response_new();
        if (res) {
            res->status_code = 400;
            free(res->body);
            res->body = ms_copy_text("{\"error\":\"invalid request headers\"}");
            ms_send_response(server, client_fd, res, 0);
            ms_response_free(res);
        }
        if (out_keep_alive) *out_keep_alive = 0;
        return 0;
    }
    if (read_status < 0 || !buffer) {
        if (out_keep_alive) *out_keep_alive = 0;
        return 0;
    }

    req = ms_parse_request(buffer);
    free(buffer);
    if (!req) {
        res = ms_response_new();
        if (res) {
            res->status_code = 400;
            free(res->body);
            res->body = ms_copy_text("{\"error\":\"bad request\"}");
            ms_send_response(server, client_fd, res, 0);
            ms_response_free(res);
        }
        if (out_keep_alive) *out_keep_alive = 0;
        return 0;
    }

    res = ms_response_new();
    if (!res) {
        ms_request_free(req);
        if (out_keep_alive) *out_keep_alive = 0;
        return 0;
    }
    keep_alive = req->keep_alive_requested && !server->stop_requested;
    res->close_connection = !keep_alive;

    ms_context_init(&ctx);
    ctx.req = req;
    ctx.res = res;
    ctx.deadline_ms = ms_now_ms() + (long long)(server->config.request_timeout_seconds > 0 ? server->config.request_timeout_seconds : 30) * 1000LL;
    if (ms_pick_request_id(req)[0] != '\0') {
        snprintf(request_id, sizeof(request_id), "%s", ms_pick_request_id(req));
    } else {
        ms_generate_request_id(request_id, sizeof(request_id), client_fd);
    }
    if (ms_pick_correlation_id(req)[0] != '\0') {
        snprintf(correlation_id, sizeof(correlation_id), "%s", ms_pick_correlation_id(req));
    } else {
        correlation_id[0] = '\0';
    }
    ctx.request_id = request_id;
    ms_generate_trace_context(req, trace_id, sizeof(trace_id), span_id, sizeof(span_id));
    ctx.trace_id = trace_id;
    ctx.span_id = span_id;
    ctx.correlation_id = correlation_id;
    ms_build_traceparent(trace_id, span_id, traceparent, sizeof(traceparent));
    if (client_addr && inet_ntop(AF_INET, &client_addr->sin_addr, ip, sizeof(ip))) {
        /* already filled */
    } else {
        snprintf(ip, sizeof(ip), "127.0.0.1");
    }
    user_agent = ms_request_header_value(req, "User-Agent");
    log_ctx.trace_id = trace_id;
    log_ctx.span_id = span_id;
    log_ctx.request_id = request_id;
    log_ctx.correlation_id = correlation_id;
    log_ctx.ip = ip;
    log_ctx.user_agent = user_agent;
    ms_context_set_header(&ctx, "traceparent", traceparent);
    if (ms_pick_request_id(req)[0] != '\0') {
        ms_context_set_header(&ctx, "X-Request-Id", request_id);
    }
    if (ms_pick_correlation_id(req)[0] != '\0') {
        ms_context_set_header(&ctx, "X-Correlation-Id", correlation_id);
    }
    should_log = ms_middleware_chain_has_name(server->middlewares, "logger");
    method = ms_method_from_text(req->method);
    if (server->metrics) {
        server->metrics->http_requests_total++;
        server->metrics->active_requests++;
        if (method >= 0 && method < MS_HTTP_METHOD_COUNT) {
            server->metrics->http_requests_by_method[method]++;
        }
    }

    if (read_status == 2 || (server->config.max_body_size > 0 && req->body && strlen(req->body) > server->config.max_body_size)) {
        res->status_code = 413;
        ms_response_set_body(res, "application/json", "{\"error\":\"payload too large\"}");
        ms_send_response(server, client_fd, res, 0);
        if (server->metrics) {
            server->metrics->http_errors_total++;
            server->metrics->http_responses_by_status_class[4]++;
            if (server->metrics->active_requests > 0) {
                server->metrics->active_requests--;
            }
        }
        ms_request_free(req);
        ms_response_free(res);
        return 1;
    }

    route = ms_router_match(server->routes[method], req->path, req);
    if (!route && method == MS_HTTP_HEAD) {
        route = ms_router_match(server->routes[MS_HTTP_GET], req->path, req);
        suppress_body = 1;
    }
    if (route) {
        route_label = route->path ? route->path : "<route>";
        should_log = should_log || ms_middleware_chain_has_name(route->middlewares, "logger");
        if (ms_context_should_abort(server, &ctx)) {
            execute_handler = 0;
        }
        if (execute_handler && !ms_middleware_execute_chain(server->middlewares, &ctx)) {
            execute_handler = 0;
        }
        if (execute_handler && ms_context_should_abort(server, &ctx)) {
            execute_handler = 0;
        }
        if (execute_handler && !ms_middleware_execute_chain(route->middlewares, &ctx)) {
            execute_handler = 0;
        }
        if (execute_handler && ms_context_should_abort(server, &ctx)) {
            execute_handler = 0;
        }
        if (execute_handler) {
            route->handler(route->handler_userdata, &ctx);
        }
        if (ms_context_should_abort(server, &ctx)) {
            res->status_code = (ctx.cancel_reason && strcmp(ctx.cancel_reason, "request timeout") == 0) ? 408 : 503;
            free(res->body);
            res->body = ms_copy_text(res->status_code == 408
                ? "{\"error\":\"request timeout\"}"
                : "{\"error\":\"server shutting down\"}");
        }
    } else if (method == MS_HTTP_OPTIONS) {
        route_label = "/options";
        ms_build_allow_header(server, req->path ? req->path : "", allow, sizeof(allow));
        res->status_code = 204;
        ms_context_set_header(&ctx, "Allow", allow);
        ms_response_set_body(res, "application/json", "");
    } else if (server->metrics_enabled && strcmp(req->path ? req->path : "", "/metrics") == 0) {
        route_label = "/metrics";
        res->status_code = 200;
        ms_metrics_render(server, metrics_body, sizeof(metrics_body));
        ms_response_set_body(res, "text/plain; version=0.0.4", metrics_body);
    } else if (server->health_enabled && strcmp(req->path ? req->path : "", "/ready") == 0) {
        route_label = "/ready";
        if (server->ready && !server->stop_requested) {
            res->status_code = 200;
            ms_response_set_body(res, "application/json", "{\"ready\":true}");
        } else {
            res->status_code = 503;
            ms_response_set_body(res, "application/json", "{\"ready\":false}");
        }
    } else if (server->health_enabled && strcmp(req->path ? req->path : "", "/health") == 0) {
        route_label = "/health";
        res->status_code = 200;
        ms_response_set_body(res, "application/json", "{\"ok\":true}");
    } else {
        ms_build_allow_header(server, req->path ? req->path : "", allow, sizeof(allow));
        if (allow[0] != '\0' && strstr(allow, req->method ? req->method : "") == NULL) {
            res->status_code = 405;
            ms_context_set_header(&ctx, "Allow", allow);
            ms_response_set_body(res, "application/json", "{\"error\":\"method not allowed\"}");
        } else {
            res->status_code = 404;
            ms_response_set_body(res, "application/json", "{\"error\":\"not found\"}");
        }
    }

    ms_send_response(server, client_fd, res, suppress_body);
    duration_ms = ms_now_ms() - request_started_ms;
    if (server->metrics) {
        int status_class = ms_status_class_index(res->status_code);
        route_metric = ms_metrics_get_route(server, req->method ? req->method : "UNKNOWN", route_label);
        if (res->status_code >= 400) {
            server->metrics->http_errors_total++;
        }
        if (ctx.cancelled) {
            server->metrics->http_cancelled_total++;
        }
        if (status_class >= 0 && status_class < 6) {
            server->metrics->http_responses_by_status_class[status_class]++;
        } else {
            server->metrics->http_responses_by_status_class[0]++;
        }
        ms_metrics_observe_route(route_metric, duration_ms, res->status_code, ctx.cancelled);
        if (server->metrics->active_requests > 0) {
            server->metrics->active_requests--;
        }
    }
    if (should_log) {
        snprintf(log_fields, sizeof(log_fields),
                 "{\"method\":\"%s\",\"path\":\"%s\",\"route\":\"%s\",\"status\":%d,\"duration_ms\":%lld,\"cancelled\":%s,\"cancel_reason\":\"%s\",\"active_connections\":%d}",
                 req->method ? req->method : "",
                 req->path ? req->path : "",
                 route_label,
                 res->status_code,
                 duration_ms,
                 ctx.cancelled ? "true" : "false",
                 ctx.cancel_reason ? ctx.cancel_reason : "",
                 server->active_connections);
        ms_log_json("info", "request completed", log_fields, &log_ctx);
    }
    if (server && server->tracing_enabled) {
        ms_trace_request("http.request",
                         req->method ? req->method : "",
                         route_label,
                         req->path ? req->path : "",
                         res->status_code,
                         duration_ms,
                         &log_ctx,
                         server ? server->tracer : NULL);
    }
    ms_request_free(req);
    ms_response_free(res);
    if (out_keep_alive) {
        *out_keep_alive = keep_alive && !ctx.cancelled && !server->stop_requested;
    }
    return 1;
}

MS_Result ms_server_result(MS_Server *server, const char *error) {
    MS_Result result;
    result.handle = server;
    result.kind = "ms.server";
    result.error = error;
    return result;
}

MS_Result ms_group_result(MS_Group *group, const char *error) {
    MS_Result result;
    result.handle = group;
    result.kind = "ms.group";
    result.error = error;
    return result;
}

MS_Server *ms_server_new(const MS_Config *config) {
    MS_Server *server = calloc(1, sizeof(MS_Server));
    if (!server) {
        return NULL;
    }
    if (config) {
        server->config = *config;
        server->port = config->port;
        server->host = ms_copy_text(config->host ? config->host : "0.0.0.0");
    } else {
        server->port = 8021;
        server->host = ms_copy_text("0.0.0.0");
    }
    server->metrics = calloc(1, sizeof(MS_Metrics));
    server->tracer = calloc(1, sizeof(MS_Tracer));
    if (server->tracer) {
        const char *service_name = getenv("OTEL_SERVICE_NAME");
        const char *env_otel = getenv("PINTO21_OTEL_ENDPOINT");
        server->tracer->otlp_endpoint = ms_copy_text(env_otel ? env_otel : "");
        server->tracer->traces_endpoint = ms_copy_env_server("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT");
        server->tracer->logs_endpoint = ms_copy_env_server("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT");
        server->tracer->protocol = ms_copy_env_server("OTEL_EXPORTER_OTLP_PROTOCOL");
        server->tracer->traces_protocol = ms_copy_env_server("OTEL_EXPORTER_OTLP_TRACES_PROTOCOL");
        server->tracer->logs_protocol = ms_copy_env_server("OTEL_EXPORTER_OTLP_LOGS_PROTOCOL");
        server->tracer->headers = ms_copy_env_server("OTEL_EXPORTER_OTLP_HEADERS");
        server->tracer->traces_headers = ms_copy_env_server("OTEL_EXPORTER_OTLP_TRACES_HEADERS");
        server->tracer->logs_headers = ms_copy_env_server("OTEL_EXPORTER_OTLP_LOGS_HEADERS");
        server->tracer->service_name = ms_copy_text((service_name && service_name[0] != '\0')
            ? service_name
            : "pinto21-microservice");
        server->tracer->timeout_ms = ms_env_int_server("OTEL_EXPORTER_OTLP_TIMEOUT", 10000);
        server->tracer->traces_timeout_ms = ms_env_int_server("OTEL_EXPORTER_OTLP_TRACES_TIMEOUT",
                                                              server->tracer->timeout_ms);
        server->tracer->logs_timeout_ms = ms_env_int_server("OTEL_EXPORTER_OTLP_LOGS_TIMEOUT",
                                                            server->tracer->timeout_ms);
        server->tracer->retry_count = ms_env_int_server("PINTO21_OTEL_RETRIES", 2);
        server->tracer->backoff_ms = ms_env_int_server("PINTO21_OTEL_BACKOFF_MS", 200);
        server->tracer->export_logs = ms_env_int_server("PINTO21_OTEL_EXPORT_LOGS", 1);
    }
    server->listen_fd = -1;
    server->ready = 0;
    server->shutdown_deadline_ms = 0;
    return server;
}

void ms_server_free(MS_Server *server) {
    int i;
    if (!server) {
        return;
    }
    free(server->host);
    free(server->last_error);
    ms_route_metrics_free(server->metrics ? server->metrics->routes : NULL);
    free(server->metrics);
    if (server->tracer) {
        free(server->tracer->otlp_endpoint);
        free(server->tracer->traces_endpoint);
        free(server->tracer->logs_endpoint);
        free(server->tracer->protocol);
        free(server->tracer->traces_protocol);
        free(server->tracer->logs_protocol);
        free(server->tracer->headers);
        free(server->tracer->traces_headers);
        free(server->tracer->logs_headers);
        free(server->tracer->service_name);
        free(server->tracer);
    }
    for (i = 0; i < MS_HTTP_METHOD_COUNT; i++) {
        MS_Route *route = server->routes[i];
        while (route) {
            MS_Route *next = route->next;
            free(route->path);
            free(route);
            route = next;
        }
    }
    free(server);
}

MS_Group *ms_server_group(MS_Server *server, const char *prefix) {
    MS_Group *group;
    if (!server) {
        return NULL;
    }
    group = calloc(1, sizeof(MS_Group));
    if (!group) {
        return NULL;
    }
    group->server = server;
    group->prefix = ms_copy_text(prefix ? prefix : "");
    return group;
}

void ms_group_free(MS_Group *group) {
    if (!group) {
        return;
    }
    free(group->prefix);
    free(group);
}

int ms_server_use(MS_Server *server, MS_Middleware *middleware) {
    if (!server || !middleware) {
        return 0;
    }
    return ms_middleware_append(&server->middlewares, middleware);
}

int ms_group_use(MS_Group *group, MS_Middleware *middleware) {
    if (!group || !middleware) {
        return 0;
    }
    return ms_middleware_append(&group->middlewares, middleware);
}

int ms_server_route(MS_Server *server, MS_HttpMethod method, const char *path, MS_HandlerFn handler, void *userdata) {
    if (!server || method < 0 || method >= MS_HTTP_METHOD_COUNT) {
        return 0;
    }
    return ms_router_add_route(&server->routes[method], method, path, handler, userdata);
}

int ms_group_route(MS_Group *group, MS_HttpMethod method, const char *path, MS_HandlerFn handler, void *userdata) {
    char *full_path;
    size_t size;
    int ok;
    if (!group || !group->server || !path) {
        return 0;
    }
    size = strlen(group->prefix ? group->prefix : "") + strlen(path) + 1;
    full_path = malloc(size);
    strcpy(full_path, group->prefix ? group->prefix : "");
    strcat(full_path, path);
    ok = ms_server_route(group->server, method, full_path, handler, userdata);
    if (ok && group->middlewares && group->server->routes[method]) {
        ms_middleware_attach_chain(&group->server->routes[method]->middlewares,
                                   ms_middleware_clone_chain(group->middlewares));
    }
    free(full_path);
    return ok;
}

int ms_server_enable_metrics(MS_Server *server) {
    if (!server) return 0;
    server->metrics_enabled = 1;
    return 1;
}

int ms_server_enable_tracing(MS_Server *server) {
    if (!server) return 0;
    server->tracing_enabled = 1;
    if (server->tracer) {
        server->tracer->enabled = 1;
    }
    return 1;
}

int ms_server_enable_health(MS_Server *server) {
    if (!server) return 0;
    server->health_enabled = 1;
    return 1;
}

int ms_server_enable_pprof(MS_Server *server) {
    if (!server) return 0;
    server->pprof_enabled = 1;
    return 1;
}

int ms_server_start(MS_Server *server) {
    struct sockaddr_in addr;
    struct pollfd listen_poll;
    int reuse = 1;
    if (!server) {
        return 0;
    }
    ms_server_set_error(server, NULL);
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        ms_server_set_errno(server, "socket failed");
        return 0;
    }
    if (!ms_set_nonblocking(server->listen_fd)) {
        ms_server_set_errno(server, "nonblocking setup failed");
        close(server->listen_fd);
        server->listen_fd = -1;
        return 0;
    }
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server->port);
    if (inet_pton(AF_INET, server->host ? server->host : "0.0.0.0", &addr.sin_addr) != 1) {
        ms_server_set_error(server, "invalid listen host");
        close(server->listen_fd);
        server->listen_fd = -1;
        return 0;
    }
    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ms_server_set_errno(server, "bind failed");
        close(server->listen_fd);
        server->listen_fd = -1;
        return 0;
    }
    if (listen(server->listen_fd, 128) != 0) {
        ms_server_set_errno(server, "listen failed");
        close(server->listen_fd);
        server->listen_fd = -1;
        return 0;
    }
    server->running = 1;
    server->stop_requested = 0;
    server->ready = 1;
    server->shutdown_deadline_ms = 0;
    listen_poll.fd = server->listen_fd;
    listen_poll.events = POLLIN;
    while (!server->stop_requested) {
        int ready = poll(&listen_poll, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            ms_server_set_errno(server, "listen poll failed");
            server->running = 0;
            server->ready = 0;
            if (server->listen_fd >= 0) {
                close(server->listen_fd);
                server->listen_fd = -1;
            }
            return 0;
        }
        if (ready == 0) {
            continue;
        }
        if (listen_poll.revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                if (server->stop_requested) {
                    break;
                }
                ms_server_set_errno(server, "accept failed");
                server->running = 0;
                server->ready = 0;
                if (server->listen_fd >= 0) {
                    close(server->listen_fd);
                    server->listen_fd = -1;
                }
                return 0;
            }
            ms_set_nonblocking(client_fd);
            server->active_connections++;
            {
                int keep_alive = 0;
                int requests_served = 0;
                do {
                    keep_alive = 0;
                    ms_handle_single_request(server, client_fd, &client_addr, &keep_alive);
                    requests_served++;
                    if (requests_served >= 16) {
                        keep_alive = 0;
                    }
                } while (keep_alive && !server->stop_requested);
            }
            server->active_connections--;
            close(client_fd);
        }
    }
    server->running = 0;
    server->ready = 0;
    server->shutdown_deadline_ms = 0;
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    return 1;
}

int ms_server_stop(MS_Server *server) {
    if (!server) {
        return 0;
    }
    server->stop_requested = 1;
    server->ready = 0;
    server->shutdown_deadline_ms = ms_now_ms() +
        (long long)(server->config.shutdown_timeout_seconds > 0 ? server->config.shutdown_timeout_seconds : 15) * 1000LL;
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    return 1;
}

const char *ms_server_last_error(MS_Server *server) {
    if (!server || !server->last_error) {
        return "";
    }
    return server->last_error;
}
