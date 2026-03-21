#include "observability.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

static long long ms_now_ns(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000000LL + (long long)tv.tv_usec * 1000LL;
}

static char *ms_copy_text_obs(const char *text) {
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

static char *ms_json_escape(const char *text) {
    size_t len = 0;
    const char *cursor;
    char *out;
    char *write;
    if (!text) {
        return ms_copy_text_obs("");
    }
    for (cursor = text; *cursor; cursor++) {
        switch (*cursor) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                len += 2;
                break;
            default:
                len += 1;
                break;
        }
    }
    out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    write = out;
    for (cursor = text; *cursor; cursor++) {
        switch (*cursor) {
            case '\\': *write++ = '\\'; *write++ = '\\'; break;
            case '"': *write++ = '\\'; *write++ = '"'; break;
            case '\n': *write++ = '\\'; *write++ = 'n'; break;
            case '\r': *write++ = '\\'; *write++ = 'r'; break;
            case '\t': *write++ = '\\'; *write++ = 't'; break;
            default: *write++ = *cursor; break;
        }
    }
    *write = '\0';
    return out;
}

static char *ms_otlp_base_endpoint(void) {
    const char *endpoint = getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    if (!endpoint || endpoint[0] == '\0') {
        endpoint = getenv("PINTO21_OTEL_ENDPOINT");
    }
    if (!endpoint || endpoint[0] == '\0') {
        return NULL;
    }
    return ms_copy_text_obs(endpoint);
}

static char *ms_otlp_signal_endpoint(const char *env_name, const char *suffix) {
    const char *signal = getenv(env_name);
    char *base;
    char *full;
    size_t len;
    if (signal && signal[0] != '\0') {
        return ms_copy_text_obs(signal);
    }
    base = ms_otlp_base_endpoint();
    if (!base) {
        return NULL;
    }
    len = strlen(base);
    full = malloc(len + strlen(suffix) + 2);
    if (!full) {
        free(base);
        return NULL;
    }
    strcpy(full, base);
    if (len == 0 || base[len - 1] != '/') {
        strcat(full, "/");
    }
    strcat(full, suffix[0] == '/' ? suffix + 1 : suffix);
    free(base);
    return full;
}

static int ms_env_int(const char *name, int fallback) {
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

static int ms_env_bool(const char *name, int fallback) {
    const char *text = getenv(name);
    if (!text || text[0] == '\0') {
        return fallback;
    }
    if (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 || strcmp(text, "TRUE") == 0 ||
        strcmp(text, "yes") == 0 || strcmp(text, "on") == 0) {
        return 1;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 || strcmp(text, "FALSE") == 0 ||
        strcmp(text, "no") == 0 || strcmp(text, "off") == 0) {
        return 0;
    }
    return fallback;
}

static const char *ms_protocol_or_default(const char *protocol) {
    if (!protocol || protocol[0] == '\0') {
        return "http/json";
    }
    return protocol;
}

static int ms_otlp_protocol_supported(const char *protocol) {
    const char *resolved = ms_protocol_or_default(protocol);
    return strcmp(resolved, "http/json") == 0;
}

static char *ms_copy_env_text(const char *name) {
    const char *text = getenv(name);
    if (!text || text[0] == '\0') {
        return NULL;
    }
    return ms_copy_text_obs(text);
}

static const char *ms_otel_service_name(void) {
    const char *name = getenv("OTEL_SERVICE_NAME");
    if (name && name[0] != '\0') {
        return name;
    }
    return "pinto21-microservice";
}

typedef struct {
    long retry_after_ms;
} MS_OtlpResponseInfo;

static char *ms_trim_copy_range(const char *start, const char *end) {
    size_t len;
    char *out;
    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    len = (size_t)(end - start);
    out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static struct curl_slist *ms_otlp_headers_from_env(struct curl_slist *headers, const char *raw_headers) {
    const char *cursor;
    if (!raw_headers || raw_headers[0] == '\0') {
        return headers;
    }
    cursor = raw_headers;
    while (*cursor) {
        const char *segment_end;
        const char *equals;
        char *key;
        char *value;
        char *header_line;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        segment_end = strchr(cursor, ',');
        if (!segment_end) {
            segment_end = cursor + strlen(cursor);
        }
        equals = memchr(cursor, '=', (size_t)(segment_end - cursor));
        if (!equals) {
            cursor = (*segment_end == ',') ? segment_end + 1 : segment_end;
            continue;
        }
        key = ms_trim_copy_range(cursor, equals);
        value = ms_trim_copy_range(equals + 1, segment_end);
        if (!key || !value || key[0] == '\0' || value[0] == '\0') {
            free(key);
            free(value);
            cursor = (*segment_end == ',') ? segment_end + 1 : segment_end;
            continue;
        }
        header_line = malloc(strlen(key) + strlen(value) + 3);
        if (!header_line) {
            free(key);
            free(value);
            cursor = (*segment_end == ',') ? segment_end + 1 : segment_end;
            continue;
        }
        snprintf(header_line, strlen(key) + strlen(value) + 3, "%s: %s", key, value);
        headers = curl_slist_append(headers, header_line);
        free(header_line);
        free(key);
        free(value);
        cursor = (*segment_end == ',') ? segment_end + 1 : segment_end;
    }
    return headers;
}

static int ms_otlp_should_retry(CURLcode code, long status_code) {
    if (code != CURLE_OK) {
        return code == CURLE_OPERATION_TIMEDOUT ||
               code == CURLE_COULDNT_CONNECT ||
               code == CURLE_SEND_ERROR ||
               code == CURLE_RECV_ERROR ||
               code == CURLE_GOT_NOTHING;
    }
    return status_code == 429 || status_code == 502 || status_code == 503 || status_code == 504;
}

static size_t ms_otlp_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    MS_OtlpResponseInfo *info = (MS_OtlpResponseInfo *)userdata;
    size_t total = size * nitems;
    if (info && total > 13 && strncasecmp(buffer, "Retry-After:", 12) == 0) {
        const char *value = buffer + 12;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        info->retry_after_ms = strtol(value, NULL, 10) * 1000L;
    }
    return total;
}

static int ms_otlp_retry_delay_ms(long retry_after_ms, int backoff_ms, int attempt) {
    int base = backoff_ms > 0 ? backoff_ms : 200;
    int delay = base << attempt;
    int jitter = (int)(ms_now_ns() % 50LL);
    if (retry_after_ms > 0) {
        return (int)retry_after_ms;
    }
    if (delay < 0) {
        delay = base;
    }
    return delay + jitter;
}

static int ms_otlp_post_json(const char *endpoint,
                             const char *json_body,
                             const char *protocol,
                             const char *headers_env,
                             int timeout_ms,
                             int retry_count,
                             int backoff_ms) {
    static int curl_initialized = 0;
    CURL *curl;
    struct curl_slist *headers = NULL;
    CURLcode code = CURLE_OK;
    long status_code = 0;
    int attempt;
    MS_OtlpResponseInfo response_info;
    if (!endpoint || !json_body) {
        return 0;
    }
    if (!ms_otlp_protocol_supported(protocol)) {
        return 0;
    }
    if (!curl_initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            return 0;
        }
        curl_initialized = 1;
    }
    for (attempt = 0; attempt <= retry_count; attempt++) {
        curl = curl_easy_init();
        if (!curl) {
            return 0;
        }
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = ms_otlp_headers_from_env(headers, headers_env);
        curl_easy_setopt(curl, CURLOPT_URL, endpoint);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "pinto21-microservice/1");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_ms > 0 ? timeout_ms : 2000));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(timeout_ms > 0 ? timeout_ms : 2000));
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
        response_info.retry_after_ms = 0;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ms_otlp_header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_info);
        code = curl_easy_perform(curl);
        status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        if (headers) {
            curl_slist_free_all(headers);
            headers = NULL;
        }
        curl_easy_cleanup(curl);
        if (!ms_otlp_should_retry(code, status_code)) {
            return code == CURLE_OK && status_code >= 200 && status_code < 300;
        }
        if (attempt < retry_count) {
            int delay_ms = ms_otlp_retry_delay_ms(response_info.retry_after_ms, backoff_ms, attempt);
            if (delay_ms > 0) {
                usleep((useconds_t)(delay_ms * 1000));
            }
        }
    }
    return 0;
}

static void ms_otlp_export_log(const char *level, const char *message, const char *fields_json, const MS_LogContext *ctx) {
    char *endpoint = ms_otlp_signal_endpoint("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "/v1/logs");
    char *headers = NULL;
    char *service = NULL;
    char *escaped_level = NULL;
    char *escaped_message = NULL;
    char *escaped_trace_id = NULL;
    char *escaped_span_id = NULL;
    char *escaped_request_id = NULL;
    char *escaped_correlation_id = NULL;
    char *escaped_ip = NULL;
    char *escaped_user_agent = NULL;
    char *escaped_fields_json = NULL;
    char *payload = NULL;
    long long now_ns;
    int severity_number = 9;
    int timeout_ms;
    int retry_count;
    int backoff_ms;
    const char *protocol;
    size_t size;

    if (!endpoint) {
        return;
    }
    if (!ms_env_bool("PINTO21_OTEL_EXPORT_LOGS", 1)) {
        free(endpoint);
        return;
    }
    if (level && strcmp(level, "error") == 0) severity_number = 17;
    else if (level && strcmp(level, "warn") == 0) severity_number = 13;
    else if (level && strcmp(level, "debug") == 0) severity_number = 5;
    headers = ms_copy_env_text("OTEL_EXPORTER_OTLP_LOGS_HEADERS");
    if (!headers) {
        headers = ms_copy_env_text("OTEL_EXPORTER_OTLP_HEADERS");
    }
    timeout_ms = ms_env_int("OTEL_EXPORTER_OTLP_LOGS_TIMEOUT",
                            ms_env_int("OTEL_EXPORTER_OTLP_TIMEOUT", 10000));
    retry_count = ms_env_int("PINTO21_OTEL_RETRIES", 2);
    backoff_ms = ms_env_int("PINTO21_OTEL_BACKOFF_MS", 200);
    protocol = getenv("OTEL_EXPORTER_OTLP_LOGS_PROTOCOL");
    if (!protocol || protocol[0] == '\0') {
        protocol = getenv("OTEL_EXPORTER_OTLP_PROTOCOL");
    }

    now_ns = ms_now_ns();
    service = ms_json_escape(ms_otel_service_name());
    escaped_level = ms_json_escape(level ? level : "info");
    escaped_message = ms_json_escape(message ? message : "");
    escaped_trace_id = ms_json_escape((ctx && ctx->trace_id) ? ctx->trace_id : "");
    escaped_span_id = ms_json_escape((ctx && ctx->span_id) ? ctx->span_id : "");
    escaped_request_id = ms_json_escape((ctx && ctx->request_id) ? ctx->request_id : "");
    escaped_correlation_id = ms_json_escape((ctx && ctx->correlation_id) ? ctx->correlation_id : "");
    escaped_ip = ms_json_escape((ctx && ctx->ip) ? ctx->ip : "");
    escaped_user_agent = ms_json_escape((ctx && ctx->user_agent) ? ctx->user_agent : "");
    escaped_fields_json = ms_json_escape(fields_json ? fields_json : "{}");

    size = strlen(service) + strlen(escaped_level) + strlen(escaped_message) +
           strlen(escaped_trace_id) + strlen(escaped_span_id) +
           strlen(escaped_request_id) + strlen(escaped_correlation_id) +
           strlen(escaped_ip) + strlen(escaped_user_agent) +
           strlen(escaped_fields_json) + 2048;
    payload = malloc(size);
    if (payload) {
        snprintf(payload, size,
                 "{\"resourceLogs\":[{\"resource\":{\"attributes\":["
                 "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"%s\"}}"
                 "]},\"scopeLogs\":[{\"scope\":{\"name\":\"pinto21.microservice\"},\"logRecords\":[{"
                 "\"timeUnixNano\":\"%lld\","
                 "\"severityNumber\":%d,"
                 "\"severityText\":\"%s\","
                 "\"body\":{\"stringValue\":\"%s\"},"
                 "\"attributes\":["
                 "{\"key\":\"trace_id\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"span_id\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"request_id\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"correlation_id\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"ip\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"user_agent\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"fields_json\",\"value\":{\"stringValue\":\"%s\"}}"
                 "]}]}]}]}",
                 service,
                 now_ns,
                 severity_number,
                 escaped_level,
                 escaped_message,
                 escaped_trace_id,
                 escaped_span_id,
                 escaped_request_id,
                 escaped_correlation_id,
                 escaped_ip,
                 escaped_user_agent,
                 escaped_fields_json);
        ms_otlp_post_json(endpoint, payload, protocol, headers, timeout_ms, retry_count, backoff_ms);
    }

    free(payload);
    free(service);
    free(escaped_level);
    free(escaped_message);
    free(escaped_trace_id);
    free(escaped_span_id);
    free(escaped_request_id);
    free(escaped_correlation_id);
    free(escaped_ip);
    free(escaped_user_agent);
    free(escaped_fields_json);
    free(headers);
    free(endpoint);
}

void ms_log_json(const char *level, const char *message, const char *fields_json, const MS_LogContext *ctx) {
    fprintf(stdout,
            "{\"level\":\"%s\",\"message\":\"%s\",\"fields\":%s,\"trace_id\":\"%s\",\"span_id\":\"%s\",\"request_id\":\"%s\",\"correlation_id\":\"%s\",\"ip\":\"%s\",\"user_agent\":\"%s\"}\n",
            level ? level : "info",
            message ? message : "",
            fields_json ? fields_json : "{}",
            (ctx && ctx->trace_id) ? ctx->trace_id : "",
            (ctx && ctx->span_id) ? ctx->span_id : "",
            (ctx && ctx->request_id) ? ctx->request_id : "",
            (ctx && ctx->correlation_id) ? ctx->correlation_id : "",
            (ctx && ctx->ip) ? ctx->ip : "",
            (ctx && ctx->user_agent) ? ctx->user_agent : "");
    ms_otlp_export_log(level, message, fields_json, ctx);
}

void ms_log_info(const char *message, const char *json_fields) {
    ms_log_json("info", message, json_fields, NULL);
}

void ms_log_error(const char *message, const char *json_fields) {
    ms_log_json("error", message, json_fields, NULL);
}

void ms_trace_request(const char *name,
                      const char *method,
                      const char *route,
                      const char *path,
                      int status_code,
                      long long duration_ms,
                      const MS_LogContext *ctx,
                      const MS_Tracer *tracer) {
    char *endpoint = NULL;
    char *headers = NULL;
    char *service = NULL;
    char *escaped_name = NULL;
    char *escaped_method = NULL;
    char *escaped_route = NULL;
    char *escaped_path = NULL;
    char *escaped_trace_id = NULL;
    char *escaped_span_id = NULL;
    char *payload = NULL;
    long long end_ns = ms_now_ns();
    long long start_ns = end_ns - (duration_ms * 1000000LL);
    int timeout_ms;
    int retry_count;
    int backoff_ms;
    const char *protocol;
    size_t size;

    fprintf(stdout,
            "{\"signal\":\"trace\",\"name\":\"%s\",\"trace_id\":\"%s\",\"span_id\":\"%s\","
            "\"request_id\":\"%s\",\"correlation_id\":\"%s\",\"http.method\":\"%s\","
            "\"http.route\":\"%s\",\"http.path\":\"%s\",\"http.status_code\":%d,"
            "\"duration_ms\":%lld,\"otel.endpoint\":\"%s\"}\n",
            name ? name : "http.request",
            (ctx && ctx->trace_id) ? ctx->trace_id : "",
            (ctx && ctx->span_id) ? ctx->span_id : "",
            (ctx && ctx->request_id) ? ctx->request_id : "",
            (ctx && ctx->correlation_id) ? ctx->correlation_id : "",
            method ? method : "",
            route ? route : "",
            path ? path : "",
            status_code,
            duration_ms,
            tracer && tracer->traces_endpoint ? tracer->traces_endpoint :
                (tracer && tracer->otlp_endpoint ? tracer->otlp_endpoint : ""));

    endpoint = ms_otlp_signal_endpoint("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT", "/v1/traces");
    if (!endpoint) {
        return;
    }
    if (tracer && tracer->traces_endpoint && tracer->traces_endpoint[0] != '\0') {
        free(endpoint);
        endpoint = ms_copy_text_obs(tracer->traces_endpoint);
    } else if (tracer && tracer->otlp_endpoint && tracer->otlp_endpoint[0] != '\0') {
        free(endpoint);
        endpoint = ms_copy_text_obs(tracer->otlp_endpoint);
    }
    headers = ms_copy_env_text("OTEL_EXPORTER_OTLP_TRACES_HEADERS");
    if (!headers && tracer && tracer->traces_headers && tracer->traces_headers[0] != '\0') {
        headers = ms_copy_text_obs(tracer->traces_headers);
    }
    if (!headers && tracer && tracer->headers && tracer->headers[0] != '\0') {
        headers = ms_copy_text_obs(tracer->headers);
    }
    if (!headers) {
        headers = ms_copy_env_text("OTEL_EXPORTER_OTLP_HEADERS");
    }
    timeout_ms = tracer && tracer->traces_timeout_ms > 0
        ? tracer->traces_timeout_ms
        : ms_env_int("OTEL_EXPORTER_OTLP_TRACES_TIMEOUT",
                     ms_env_int("OTEL_EXPORTER_OTLP_TIMEOUT", 10000));
    retry_count = tracer ? tracer->retry_count : ms_env_int("PINTO21_OTEL_RETRIES", 2);
    backoff_ms = tracer ? tracer->backoff_ms : ms_env_int("PINTO21_OTEL_BACKOFF_MS", 200);
    protocol = tracer && tracer->traces_protocol && tracer->traces_protocol[0] != '\0'
        ? tracer->traces_protocol
        : (tracer && tracer->protocol ? tracer->protocol : getenv("OTEL_EXPORTER_OTLP_PROTOCOL"));

    service = ms_json_escape(tracer && tracer->service_name && tracer->service_name[0] != '\0'
        ? tracer->service_name
        : ms_otel_service_name());
    escaped_name = ms_json_escape(name ? name : "http.request");
    escaped_method = ms_json_escape(method ? method : "");
    escaped_route = ms_json_escape(route ? route : "");
    escaped_path = ms_json_escape(path ? path : "");
    escaped_trace_id = ms_json_escape((ctx && ctx->trace_id) ? ctx->trace_id : "");
    escaped_span_id = ms_json_escape((ctx && ctx->span_id) ? ctx->span_id : "");

    size = strlen(service) + strlen(escaped_name) + strlen(escaped_method) +
           strlen(escaped_route) + strlen(escaped_path) +
           strlen(escaped_trace_id) + strlen(escaped_span_id) + 2048;
    payload = malloc(size);
    if (payload) {
        snprintf(payload, size,
                 "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
                 "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"%s\"}}"
                 "]},\"scopeSpans\":[{\"scope\":{\"name\":\"pinto21.microservice\"},\"spans\":[{"
                 "\"traceId\":\"%s\","
                 "\"spanId\":\"%s\","
                 "\"name\":\"%s\","
                 "\"kind\":2,"
                 "\"startTimeUnixNano\":\"%lld\","
                 "\"endTimeUnixNano\":\"%lld\","
                 "\"attributes\":["
                 "{\"key\":\"http.method\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"http.route\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"http.target\",\"value\":{\"stringValue\":\"%s\"}},"
                 "{\"key\":\"http.status_code\",\"value\":{\"intValue\":\"%d\"}}"
                 "]}]}]}]}",
                 service,
                 escaped_trace_id,
                 escaped_span_id,
                 escaped_name,
                 start_ns,
                 end_ns,
                 escaped_method,
                 escaped_route,
                 escaped_path,
                 status_code);
        ms_otlp_post_json(endpoint, payload, protocol, headers, timeout_ms, retry_count, backoff_ms);
    }

    free(payload);
    free(service);
    free(escaped_name);
    free(escaped_method);
    free(escaped_route);
    free(escaped_path);
    free(escaped_trace_id);
    free(escaped_span_id);
    free(headers);
    free(endpoint);
}
