#ifndef P21_MICROSERVICE_H
#define P21_MICROSERVICE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Package singleton safety rule:
 *
 * The Pinto21 package is only a namespace/API surface. Operational runtime
 * state must never live in mutable package-global variables.
 *
 * Every server/group/context/middleware instance must own its own state
 * explicitly through handles and heap allocations. This prevents accidental
 * cross-talk when more than one package or application imports and uses the
 * microservice module in the same process.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MS_HTTP_GET = 0,
    MS_HTTP_POST,
    MS_HTTP_PUT,
    MS_HTTP_DELETE,
    MS_HTTP_HEAD,
    MS_HTTP_OPTIONS,
    MS_HTTP_METHOD_COUNT
} MS_HttpMethod;

typedef struct MS_Middleware MS_Middleware;
typedef struct MS_Route MS_Route;
typedef struct MS_Server MS_Server;
typedef struct MS_Context MS_Context;
typedef struct MS_Request MS_Request;
typedef struct MS_Response MS_Response;
typedef struct MS_Metrics MS_Metrics;
typedef struct MS_RouteMetric MS_RouteMetric;
typedef struct MS_Tracer MS_Tracer;
typedef struct MS_Group MS_Group;
typedef struct MS_LogSink MS_LogSink;
typedef struct MS_Header MS_Header;
typedef struct MS_Param MS_Param;

typedef int (*MS_HandlerFn)(void *userdata, MS_Context *ctx);
typedef int (*MS_MiddlewareFn)(void *userdata, MS_Context *ctx);

typedef struct {
    int enabled;
    int failure_threshold;
    int reset_after_seconds;
} MS_CircuitBreakerConfig;

typedef struct {
    int request_timeout_seconds;
    int downstream_timeout_seconds;
    int retries;
    int backoff_ms;
    MS_CircuitBreakerConfig circuit_breaker;
} MS_ResilienceConfig;

typedef struct {
    int enabled;
    const char **origins;
    int origin_count;
    const char **methods;
    int method_count;
} MS_CorsConfig;

typedef struct {
    int port;
    const char *host;
    size_t max_body_size;
    int timeout_seconds;
    int request_timeout_seconds;
    int read_timeout_seconds;
    int write_timeout_seconds;
    int idle_timeout_seconds;
    int shutdown_timeout_seconds;
    int workers;
    MS_CorsConfig cors;
    MS_ResilienceConfig resilience;
} MS_Config;

struct MS_RouteMetric {
    char *method;
    char *route;
    uint64_t requests_total;
    uint64_t errors_total;
    uint64_t cancelled_total;
    uint64_t duration_le_10ms;
    uint64_t duration_le_50ms;
    uint64_t duration_le_100ms;
    uint64_t duration_le_500ms;
    uint64_t duration_le_1000ms;
    uint64_t duration_gt_1000ms;
    MS_RouteMetric *next;
};

struct MS_Metrics {
    uint64_t http_requests_total;
    uint64_t http_errors_total;
    uint64_t active_requests;
    uint64_t http_cancelled_total;
    uint64_t http_requests_by_method[MS_HTTP_METHOD_COUNT];
    uint64_t http_responses_by_status_class[6];
    MS_RouteMetric *routes;
};

struct MS_Middleware {
    char *name;
    MS_MiddlewareFn fn;
    void *userdata;
    MS_Middleware *next;
};

struct MS_Route {
    MS_HttpMethod method;
    char *path;
    MS_HandlerFn handler;
    void *handler_userdata;
    MS_Middleware *middlewares;
    MS_Route *next;
};

struct MS_Request {
    char *method;
    char *path;
    char *http_version;
    char *query_string;
    char *body;
    MS_Header *headers;
    int header_count;
    MS_Param *params;
    int param_count;
    int keep_alive_requested;
};

struct MS_Response {
    int status_code;
    char *content_type;
    char *body;
    MS_Header *headers;
    int header_count;
    int close_connection;
};

struct MS_Tracer {
    int enabled;
    char *otlp_endpoint;
    char *traces_endpoint;
    char *logs_endpoint;
    char *protocol;
    char *traces_protocol;
    char *logs_protocol;
    char *headers;
    char *traces_headers;
    char *logs_headers;
    char *service_name;
    int timeout_ms;
    int traces_timeout_ms;
    int logs_timeout_ms;
    int retry_count;
    int backoff_ms;
    int export_logs;
};

struct MS_Header {
    char *name;
    char *value;
};

struct MS_Param {
    char *name;
    char *value;
};

struct MS_Context {
    MS_Request *req;
    MS_Response *res;
    MS_Middleware *current;
    void *user_data;
    struct MS_Context *next;
    long long deadline_ms;
    int cancelled;
    const char *cancel_reason;
    const char *request_id;
    const char *trace_id;
    const char *span_id;
    const char *correlation_id;
};

struct MS_Server {
    int port;
    char *host;
    MS_Middleware *middlewares;
    MS_Route *routes[MS_HTTP_METHOD_COUNT];
    MS_Config config;
    MS_Metrics *metrics;
    MS_Tracer *tracer;
    int metrics_enabled;
    int tracing_enabled;
    int health_enabled;
    int pprof_enabled;
    int ready;
    int listen_fd;
    int running;
    int stop_requested;
    int active_connections;
    long long shutdown_deadline_ms;
    char *last_error;
};

struct MS_Group {
    MS_Server *server;
    char *prefix;
    MS_Middleware *middlewares;
};

typedef struct {
    void *handle;
    const char *kind;
    const char *error;
} MS_Result;

MS_Server *ms_server_new(const MS_Config *config);
void ms_server_free(MS_Server *server);
MS_Group *ms_server_group(MS_Server *server, const char *prefix);
void ms_group_free(MS_Group *group);
int ms_server_use(MS_Server *server, MS_Middleware *middleware);
int ms_group_use(MS_Group *group, MS_Middleware *middleware);
int ms_server_route(MS_Server *server, MS_HttpMethod method, const char *path, MS_HandlerFn handler, void *userdata);
int ms_group_route(MS_Group *group, MS_HttpMethod method, const char *path, MS_HandlerFn handler, void *userdata);
int ms_server_enable_metrics(MS_Server *server);
int ms_server_enable_tracing(MS_Server *server);
int ms_server_enable_health(MS_Server *server);
int ms_server_enable_pprof(MS_Server *server);
int ms_server_start(MS_Server *server);
int ms_server_stop(MS_Server *server);
const char *ms_server_last_error(MS_Server *server);
MS_Route *ms_router_match(MS_Route *head, const char *path, MS_Request *req);

MS_Middleware *ms_middleware_logger(void);
MS_Middleware *ms_middleware_cors(const MS_CorsConfig *config);
MS_Middleware *ms_middleware_compress(void);
MS_Middleware *ms_middleware_rate_limit(int window_seconds, int max_requests);
void ms_middleware_free(MS_Middleware *middleware);

void ms_log_info(const char *message, const char *json_fields);
void ms_log_error(const char *message, const char *json_fields);

const char *ms_context_header(MS_Context *ctx, const char *name);
const char *ms_context_query(MS_Context *ctx, const char *name);
const char *ms_context_param(MS_Context *ctx, const char *name);
const char *ms_context_body(MS_Context *ctx);
int ms_context_status(MS_Context *ctx, int status_code);
int ms_context_json(MS_Context *ctx, const char *json_body);
int ms_context_set_header(MS_Context *ctx, const char *name, const char *value);
int ms_context_is_cancelled(MS_Context *ctx);
int ms_context_poll_cancelled(MS_Context *ctx);
const char *ms_context_cancel_reason(MS_Context *ctx);
long long ms_context_deadline_ms(MS_Context *ctx);
const char *ms_context_request_id(MS_Context *ctx);
const char *ms_context_trace_id(MS_Context *ctx);
const char *ms_context_span_id(MS_Context *ctx);
const char *ms_context_correlation_id(MS_Context *ctx);
void ms_request_free(MS_Request *req);
void ms_response_free(MS_Response *res);

#ifdef __cplusplus
}
#endif

#endif
