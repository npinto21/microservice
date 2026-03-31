#include "modules/native/core_native_packages.h"
#include "module_native_microservice_hooks.h"

#include <string.h>

static const NativeFuncSpec MICROSERVICE_MODULE_FUNCS[] = {
    { "server_new", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "server_group", 2, { TYPE_OBJ, TYPE_STRING }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "apply_middleware", 2, { TYPE_OBJ, TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "route_get", 3, { TYPE_OBJ, TYPE_STRING, TYPE_FUNC }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "route_post", 3, { TYPE_OBJ, TYPE_STRING, TYPE_FUNC }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "route_put", 3, { TYPE_OBJ, TYPE_STRING, TYPE_FUNC }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "route_head", 3, { TYPE_OBJ, TYPE_STRING, TYPE_FUNC }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "route_options", 3, { TYPE_OBJ, TYPE_STRING, TYPE_FUNC }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "route_delete", 3, { TYPE_OBJ, TYPE_STRING, TYPE_FUNC }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "enable_metrics", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "enable_tracing", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "enable_health", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "enable_pprof", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "server_start", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "server_stop", 1, { TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "server_state", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "middleware_logger", 0, { TYPE_ANY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "middleware_cors", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "middleware_compress", 0, { TYPE_ANY }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "middleware_rate_limit", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "log_info", 2, { TYPE_STRING, TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "log_error", 2, { TYPE_STRING, TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "test_sleep_ms", 1, { TYPE_INT }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "ctx_header", 2, { TYPE_OBJ, TYPE_STRING }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
    { "ctx_query", 2, { TYPE_OBJ, TYPE_STRING }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
    { "ctx_param", 2, { TYPE_OBJ, TYPE_STRING }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
    { "ctx_body", 1, { TYPE_OBJ }, 1, TYPE_OBJ, 1, NATIVE_FUNC_NONE },
    { "ctx_status", 2, { TYPE_OBJ, TYPE_INT }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "ctx_json", 2, { TYPE_OBJ, TYPE_OBJ }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "ctx_set_header", 3, { TYPE_OBJ, TYPE_STRING, TYPE_STRING }, 0, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "ctx_is_cancelled", 1, { TYPE_OBJ }, 1, TYPE_BOOL, 1, NATIVE_FUNC_NONE },
    { "ctx_cancel_reason", 1, { TYPE_OBJ }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
    { "ctx_deadline_ms", 1, { TYPE_OBJ }, 1, TYPE_INT, 1, NATIVE_FUNC_NONE },
    { "ctx_request_id", 1, { TYPE_OBJ }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
    { "ctx_trace_id", 1, { TYPE_OBJ }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
    { "ctx_span_id", 1, { TYPE_OBJ }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
    { "ctx_correlation_id", 1, { TYPE_OBJ }, 1, TYPE_STRING, 1, NATIVE_FUNC_NONE },
};

static const NativePackageSpec MICROSERVICE_MODULE_PACKAGE = {
    "microservice_native",
    MICROSERVICE_MODULE_FUNCS,
    (int)(sizeof(MICROSERVICE_MODULE_FUNCS) / sizeof(MICROSERVICE_MODULE_FUNCS[0]))
};

const NativePackageSpec *p21_module_microservice_find_package(const char *path) {
    if (strcmp(path, "microservice_native") == 0) {
        return &MICROSERVICE_MODULE_PACKAGE;
    }
    return NULL;
}

const ModuleNativeRegistryProvider *p21_module_native_registry_provider(void) {
    static const ModuleNativeRegistryProvider provider = {
        "microservice",
        p21_module_microservice_find_package
    };
    return &provider;
}
