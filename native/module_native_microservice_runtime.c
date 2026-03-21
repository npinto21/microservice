#include "module_native_microservice_hooks.h"
#include "microservice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    ModuleNativeHostApi host;
    Value handler;
} MSHandlerBinding;

static int ms_runtime_interrupt_probe(void *userdata) {
    return ms_context_poll_cancelled((MS_Context *)userdata);
}

static char *copy_text(const char *text) {
    size_t len = strlen(text);
    char *result = malloc(len + 1);
    memcpy(result, text, len + 1);
    return result;
}

static Value make_void_local(void) {
    Value value;
    value.type = VALUE_VOID;
    return value;
}

static Value make_int_local(long long integer) {
    Value value;
    value.type = VALUE_INT;
    value.as.integer = integer;
    return value;
}

static Value make_bool_local(int boolean) {
    Value value;
    value.type = VALUE_BOOL;
    value.as.boolean = boolean ? 1 : 0;
    return value;
}

static Value make_double_local(double number) {
    Value value;
    value.type = VALUE_DOUBLE;
    value.as.double_value = number;
    return value;
}

static Value make_string_copy_local(const char *string) {
    Value value;
    value.type = VALUE_STRING;
    value.as.string = copy_text(string ? string : "");
    return value;
}

static Value make_array_local(Value *items, int count) {
    Value value;
    value.type = VALUE_ARRAY;
    value.as.array.items = items;
    value.as.array.count = count;
    return value;
}

static Value make_object_local(char **keys, Value *values, int count) {
    Value value;
    value.type = VALUE_OBJECT;
    value.as.object.keys = keys;
    value.as.object.values = values;
    value.as.object.count = count;
    return value;
}

static void *parse_handle_from_object(Value object) {
    int i;
    if (object.type != VALUE_OBJECT) {
        return NULL;
    }
    for (i = 0; i < object.as.object.count; i++) {
        if (strcmp(object.as.object.keys[i], "_handle") == 0 &&
            object.as.object.values[i].type == VALUE_STRING) {
            void *handle = NULL;
            if (sscanf(object.as.object.values[i].as.string, "%p", &handle) == 1) {
                return handle;
            }
            return NULL;
        }
    }
    return NULL;
}

static const char *get_kind_from_object(Value object) {
    int i;
    if (object.type != VALUE_OBJECT) {
        return NULL;
    }
    for (i = 0; i < object.as.object.count; i++) {
        if (strcmp(object.as.object.keys[i], "_kind") == 0 &&
            object.as.object.values[i].type == VALUE_STRING) {
            return object.as.object.values[i].as.string;
        }
    }
    return NULL;
}

static Value make_handle_object(void *handle, const char *kind) {
    char **keys = calloc(2, sizeof(char *));
    Value *values = calloc(2, sizeof(Value));
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%p", handle);
    keys[0] = copy_text("_handle");
    values[0] = make_string_copy_local(buffer);
    keys[1] = copy_text("_kind");
    values[1] = make_string_copy_local(kind);
    return make_object_local(keys, values, 2);
}

static char *json_escape_string(const char *text) {
    size_t len = 0;
    size_t i;
    char *out;
    if (!text) {
        return copy_text("");
    }
    for (i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
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
    len = 0;
    for (i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '\\': out[len++] = '\\'; out[len++] = '\\'; break;
            case '"': out[len++] = '\\'; out[len++] = '"'; break;
            case '\n': out[len++] = '\\'; out[len++] = 'n'; break;
            case '\r': out[len++] = '\\'; out[len++] = 'r'; break;
            case '\t': out[len++] = '\\'; out[len++] = 't'; break;
            default: out[len++] = text[i]; break;
        }
    }
    out[len] = '\0';
    return out;
}

static char *json_stringify_value_local(Value value);

static char *json_stringify_object_local(Value value) {
    int i;
    size_t size = 2;
    char *out;
    if (value.type != VALUE_OBJECT) {
        return copy_text("{}");
    }
    for (i = 0; i < value.as.object.count; i++) {
        char *key = json_escape_string(value.as.object.keys[i]);
        char *inner = json_stringify_value_local(value.as.object.values[i]);
        size += strlen(key) + strlen(inner) + 4;
        if (i + 1 < value.as.object.count) {
            size += 1;
        }
        free(key);
        free(inner);
    }
    out = malloc(size + 1);
    out[0] = '{';
    out[1] = '\0';
    for (i = 0; i < value.as.object.count; i++) {
        char *key = json_escape_string(value.as.object.keys[i]);
        char *inner = json_stringify_value_local(value.as.object.values[i]);
        strcat(out, "\"");
        strcat(out, key);
        strcat(out, "\":");
        strcat(out, inner);
        if (i + 1 < value.as.object.count) {
            strcat(out, ",");
        }
        free(key);
        free(inner);
    }
    strcat(out, "}");
    return out;
}

static char *json_stringify_array_local(Value value) {
    int i;
    size_t size = 2;
    char *out;
    if (value.type != VALUE_ARRAY) {
        return copy_text("[]");
    }
    for (i = 0; i < value.as.array.count; i++) {
        char *inner = json_stringify_value_local(value.as.array.items[i]);
        size += strlen(inner);
        if (i + 1 < value.as.array.count) {
            size += 1;
        }
        free(inner);
    }
    out = malloc(size + 1);
    out[0] = '[';
    out[1] = '\0';
    for (i = 0; i < value.as.array.count; i++) {
        char *inner = json_stringify_value_local(value.as.array.items[i]);
        strcat(out, inner);
        if (i + 1 < value.as.array.count) {
            strcat(out, ",");
        }
        free(inner);
    }
    strcat(out, "]");
    return out;
}

static char *json_stringify_value_local(Value value) {
    char buffer[64];
    switch (value.type) {
        case VALUE_VOID:
            return copy_text("null");
        case VALUE_BOOL:
            return copy_text(value.as.boolean ? "true" : "false");
        case VALUE_INT:
            snprintf(buffer, sizeof(buffer), "%lld", value.as.integer);
            return copy_text(buffer);
        case VALUE_STRING: {
            char *escaped = json_escape_string(value.as.string ? value.as.string : "");
            char *out = malloc(strlen(escaped) + 3);
            sprintf(out, "\"%s\"", escaped);
            free(escaped);
            return out;
        }
        case VALUE_ARRAY:
            return json_stringify_array_local(value);
        case VALUE_OBJECT:
            return json_stringify_object_local(value);
        default:
            return copy_text("null");
    }
}

typedef struct {
    const char *text;
    int pos;
} LocalJsonParser;

static void local_json_skip_spaces(LocalJsonParser *parser) {
    while (parser->text[parser->pos] == ' ' ||
           parser->text[parser->pos] == '\n' ||
           parser->text[parser->pos] == '\r' ||
           parser->text[parser->pos] == '\t') {
        parser->pos++;
    }
}

static int local_json_match_literal(LocalJsonParser *parser, const char *text) {
    size_t len = strlen(text);
    if (strncmp(parser->text + parser->pos, text, len) == 0) {
        parser->pos += (int)len;
        return 1;
    }
    return 0;
}

static Value local_json_parse_value(LocalJsonParser *parser);

static Value local_json_parse_string(LocalJsonParser *parser) {
    char *buffer;
    int capacity = 32;
    int length = 0;
    parser->pos++;
    buffer = malloc((size_t)capacity);
    while (parser->text[parser->pos] != '\0' && parser->text[parser->pos] != '"') {
        char c = parser->text[parser->pos++];
        if (c == '\\') {
            c = parser->text[parser->pos++];
            switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                default: break;
            }
        }
        if (length + 2 >= capacity) {
            capacity *= 2;
            buffer = realloc(buffer, (size_t)capacity);
        }
        buffer[length++] = c;
    }
    if (parser->text[parser->pos] == '"') {
        parser->pos++;
    }
    buffer[length] = '\0';
    {
        Value value = make_string_copy_local(buffer);
        free(buffer);
        return value;
    }
}

static Value local_json_parse_number(LocalJsonParser *parser) {
    int start = parser->pos;
    int seen_dot = 0;
    while (parser->text[parser->pos] == '-' ||
           parser->text[parser->pos] == '+' ||
           parser->text[parser->pos] == '.' ||
           (parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') ||
           parser->text[parser->pos] == 'e' ||
           parser->text[parser->pos] == 'E') {
        if (parser->text[parser->pos] == '.') {
            seen_dot = 1;
        }
        parser->pos++;
    }
    {
        char *slice = copy_text("");
        Value value;
        free(slice);
        slice = malloc((size_t)(parser->pos - start + 1));
        memcpy(slice, parser->text + start, (size_t)(parser->pos - start));
        slice[parser->pos - start] = '\0';
        if (seen_dot || strchr(slice, 'e') || strchr(slice, 'E')) {
            value = make_double_local(strtod(slice, NULL));
        } else {
            value = make_int_local(strtoll(slice, NULL, 10));
        }
        free(slice);
        return value;
    }
}

static Value local_json_parse_array(LocalJsonParser *parser) {
    Value *items = NULL;
    int count = 0;
    parser->pos++;
    local_json_skip_spaces(parser);
    if (parser->text[parser->pos] == ']') {
        parser->pos++;
        return make_array_local(NULL, 0);
    }
    while (parser->text[parser->pos] != '\0') {
        items = realloc(items, (size_t)(count + 1) * sizeof(Value));
        items[count++] = local_json_parse_value(parser);
        local_json_skip_spaces(parser);
        if (parser->text[parser->pos] == ',') {
            parser->pos++;
            local_json_skip_spaces(parser);
            continue;
        }
        if (parser->text[parser->pos] == ']') {
            parser->pos++;
            break;
        }
        break;
    }
    return make_array_local(items, count);
}

static Value local_json_parse_object(LocalJsonParser *parser) {
    char **keys = NULL;
    Value *values = NULL;
    int count = 0;
    parser->pos++;
    local_json_skip_spaces(parser);
    if (parser->text[parser->pos] == '}') {
        parser->pos++;
        return make_object_local(NULL, NULL, 0);
    }
    while (parser->text[parser->pos] != '\0') {
        Value key;
        local_json_skip_spaces(parser);
        if (parser->text[parser->pos] != '"') {
            break;
        }
        key = local_json_parse_string(parser);
        local_json_skip_spaces(parser);
        if (parser->text[parser->pos] != ':') {
            free(key.as.string);
            break;
        }
        parser->pos++;
        local_json_skip_spaces(parser);
        keys = realloc(keys, (size_t)(count + 1) * sizeof(char *));
        values = realloc(values, (size_t)(count + 1) * sizeof(Value));
        keys[count] = key.as.string;
        values[count] = local_json_parse_value(parser);
        count++;
        local_json_skip_spaces(parser);
        if (parser->text[parser->pos] == ',') {
            parser->pos++;
            continue;
        }
        if (parser->text[parser->pos] == '}') {
            parser->pos++;
            break;
        }
        break;
    }
    return make_object_local(keys, values, count);
}

static Value local_json_parse_value(LocalJsonParser *parser) {
    local_json_skip_spaces(parser);
    switch (parser->text[parser->pos]) {
        case '"': return local_json_parse_string(parser);
        case '{': return local_json_parse_object(parser);
        case '[': return local_json_parse_array(parser);
        case 't':
            if (local_json_match_literal(parser, "true")) return make_bool_local(1);
            break;
        case 'f':
            if (local_json_match_literal(parser, "false")) return make_bool_local(0);
            break;
        case 'n':
            if (local_json_match_literal(parser, "null")) return make_void_local();
            break;
        default:
            if (parser->text[parser->pos] == '-' ||
                (parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9')) {
                return local_json_parse_number(parser);
            }
            break;
    }
    return make_void_local();
}

static Value body_parse_json_local(const char *raw) {
    LocalJsonParser parser;
    parser.text = raw ? raw : "";
    parser.pos = 0;
    return local_json_parse_value(&parser);
}

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static char *url_decode_local(const char *text, size_t len) {
    char *out = malloc(len + 1);
    size_t i;
    size_t j = 0;
    for (i = 0; i < len; i++) {
        if (text[i] == '+' ) {
            out[j++] = ' ';
        } else if (text[i] == '%' && i + 2 < len) {
            int hi = hex_to_int(text[i + 1]);
            int lo = hex_to_int(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                out[j++] = text[i];
            }
        } else {
            out[j++] = text[i];
        }
    }
    out[j] = '\0';
    return out;
}

static Value body_parse_form_local(const char *raw) {
    char **keys = NULL;
    Value *values = NULL;
    int count = 0;
    const char *cursor = raw ? raw : "";
    while (*cursor != '\0') {
        const char *pair_end = strchr(cursor, '&');
        const char *eq = strchr(cursor, '=');
        size_t key_len;
        size_t value_len;
        char *decoded_key;
        char *decoded_value;
        if (!pair_end) {
            pair_end = cursor + strlen(cursor);
        }
        if (!eq || eq > pair_end) {
            eq = pair_end;
        }
        key_len = (size_t)(eq - cursor);
        value_len = eq < pair_end ? (size_t)(pair_end - (eq + 1)) : 0;
        decoded_key = url_decode_local(cursor, key_len);
        decoded_value = url_decode_local(eq < pair_end ? eq + 1 : "", value_len);
        keys = realloc(keys, (size_t)(count + 1) * sizeof(char *));
        values = realloc(values, (size_t)(count + 1) * sizeof(Value));
        keys[count] = decoded_key;
        values[count] = make_string_copy_local(decoded_value);
        free(decoded_value);
        count++;
        cursor = *pair_end == '&' ? pair_end + 1 : pair_end;
    }
    return make_object_local(keys, values, count);
}

static int ms_runtime_handler(void *userdata, MS_Context *ctx) {
    MSHandlerBinding *binding = (MSHandlerBinding *)userdata;
    Value ctx_arg;
    Value result_value;
    Value error_value;
    char *error_json;
    if (!binding || !binding->host.call_function) {
        return 0;
    }
    ctx_arg = make_handle_object(ctx, "ms.context");
    result_value = make_void_local();
    error_value = make_void_local();
    if (binding->host.set_interrupt_probe) {
        binding->host.set_interrupt_probe(binding->host.userdata, ms_runtime_interrupt_probe, ctx, "callback interrupted by request cancellation");
    }
    if (!binding->host.call_function(binding->host.userdata, binding->handler, &ctx_arg, 1, &result_value, &error_value)) {
        if (binding->host.clear_interrupt_probe) {
            binding->host.clear_interrupt_probe(binding->host.userdata);
        }
        if (ms_context_is_cancelled(ctx)) {
            binding->host.free_value(error_value);
            binding->host.free_value(ctx_arg);
            binding->host.free_value(result_value);
            return 0;
        }
        ms_context_status(ctx, 500);
        error_json = json_stringify_value_local(error_value);
        ms_context_json(ctx, error_json);
        free(error_json);
        binding->host.free_value(error_value);
        binding->host.free_value(ctx_arg);
        binding->host.free_value(result_value);
        return 0;
    }
    if (binding->host.clear_interrupt_probe) {
        binding->host.clear_interrupt_probe(binding->host.userdata);
    }
    binding->host.free_value(result_value);
    binding->host.free_value(error_value);
    binding->host.free_value(ctx_arg);
    return 1;
}

static long long get_int_field(Value object, const char *name, long long fallback) {
    int i;
    if (object.type != VALUE_OBJECT) {
        return fallback;
    }
    for (i = 0; i < object.as.object.count; i++) {
        if (strcmp(object.as.object.keys[i], name) == 0 &&
            object.as.object.values[i].type == VALUE_INT) {
            return object.as.object.values[i].as.integer;
        }
    }
    return fallback;
}

static const char *get_string_field(Value object, const char *name, const char *fallback) {
    int i;
    if (object.type != VALUE_OBJECT) {
        return fallback;
    }
    for (i = 0; i < object.as.object.count; i++) {
        if (strcmp(object.as.object.keys[i], name) == 0 &&
            object.as.object.values[i].type == VALUE_STRING) {
            return object.as.object.values[i].as.string;
        }
    }
    return fallback;
}

static int add_route(Value *args, MS_HttpMethod method, const ModuleNativeHostApi *host) {
    void *target = parse_handle_from_object(args[0]);
    const char *path = args[1].as.string;
    const char *kind = get_kind_from_object(args[0]);
    MSHandlerBinding *binding;
    if (!target || !path) {
        return 0;
    }
    if (!host || !host->copy_value || !host->free_value || !host->call_function) {
        return 0;
    }
    binding = calloc(1, sizeof(MSHandlerBinding));
    if (!binding) {
        return 0;
    }
    binding->host = *host;
    binding->handler = host->copy_value(args[2]);
    if (kind && strcmp(kind, "ms.group") == 0) {
        return ms_group_route((MS_Group *)target, method, path, ms_runtime_handler, binding);
    }
    return ms_server_route((MS_Server *)target, method, path, ms_runtime_handler, binding);
}

int p21_module_microservice_invoke(
    const char *package_path,
    const char *func_name,
    Value *args,
    int arg_count,
    const ModuleNativeHostApi *host,
    ModuleNativeInvokeResult *result
) {
    (void)arg_count;

    result->handle = NULL;
    result->kind = NULL;
    result->error = NULL;
    result->has_value = 0;
    result->value = make_void_local();

    if (strcmp(package_path, "microservice_native") != 0) {
        return 0;
    }

    if (strcmp(func_name, "server_new") == 0) {
        MS_Config config;
        MS_Server *server;
        memset(&config, 0, sizeof(config));
        config.port = (int)get_int_field(args[0], "port", 8021);
        config.host = get_string_field(args[0], "host", "0.0.0.0");
        config.max_body_size = (size_t)get_int_field(args[0], "max_body_size", 1024 * 1024);
        config.timeout_seconds = (int)get_int_field(args[0], "timeout", 30);
        config.request_timeout_seconds = (int)get_int_field(args[0], "request_timeout", config.timeout_seconds);
        config.read_timeout_seconds = (int)get_int_field(args[0], "read_timeout", config.timeout_seconds);
        config.write_timeout_seconds = (int)get_int_field(args[0], "write_timeout", config.timeout_seconds);
        config.idle_timeout_seconds = (int)get_int_field(args[0], "idle_timeout", 60);
        config.shutdown_timeout_seconds = (int)get_int_field(args[0], "shutdown_timeout", 15);
        config.workers = (int)get_int_field(args[0], "workers", 1);
        config.resilience.request_timeout_seconds = (int)get_int_field(args[0], "request_timeout", config.request_timeout_seconds);
        config.resilience.downstream_timeout_seconds = (int)get_int_field(args[0], "downstream_timeout", 3);
        config.resilience.retries = (int)get_int_field(args[0], "retries", 0);
        config.resilience.backoff_ms = (int)get_int_field(args[0], "backoff_ms", 0);
        server = ms_server_new(&config);
        if (!server) {
            result->error = "could not allocate microservice server";
            return 1;
        }
        result->has_value = 1;
        result->value = make_handle_object(server, "ms.server");
        return 1;
    }

    if (strcmp(func_name, "server_group") == 0) {
        MS_Group *group = ms_server_group((MS_Server *)parse_handle_from_object(args[0]), args[1].as.string);
        if (!group) {
            result->error = "could not create route group";
            return 1;
        }
        result->has_value = 1;
        result->value = make_handle_object(group, "ms.group");
        return 1;
    }

    if (strcmp(func_name, "apply_middleware") == 0) {
        void *target = parse_handle_from_object(args[0]);
        MS_Middleware *middleware = (MS_Middleware *)parse_handle_from_object(args[1]);
        int i;
        if (!target || !middleware) {
            result->error = "apply_middleware requires a target and middleware handle";
            return 1;
        }
        for (i = 0; i < args[0].as.object.count; i++) {
            if (strcmp(args[0].as.object.keys[i], "_kind") == 0 &&
                args[0].as.object.values[i].type == VALUE_STRING &&
                strcmp(args[0].as.object.values[i].as.string, "ms.group") == 0) {
                result->has_value = 1;
                result->value = make_int_local(ms_group_use((MS_Group *)target, middleware));
                return 1;
            }
        }
        result->has_value = 1;
        result->value = make_int_local(ms_server_use((MS_Server *)target, middleware));
        return 1;
    }

    if (strcmp(func_name, "route_get") == 0) {
        result->has_value = 1;
        result->value = make_int_local(add_route(args, MS_HTTP_GET, host));
        return 1;
    }
    if (strcmp(func_name, "route_post") == 0) {
        result->has_value = 1;
        result->value = make_int_local(add_route(args, MS_HTTP_POST, host));
        return 1;
    }
    if (strcmp(func_name, "route_put") == 0) {
        result->has_value = 1;
        result->value = make_int_local(add_route(args, MS_HTTP_PUT, host));
        return 1;
    }
    if (strcmp(func_name, "route_head") == 0) {
        result->has_value = 1;
        result->value = make_int_local(add_route(args, MS_HTTP_HEAD, host));
        return 1;
    }
    if (strcmp(func_name, "route_options") == 0) {
        result->has_value = 1;
        result->value = make_int_local(add_route(args, MS_HTTP_OPTIONS, host));
        return 1;
    }
    if (strcmp(func_name, "route_delete") == 0) {
        result->has_value = 1;
        result->value = make_int_local(add_route(args, MS_HTTP_DELETE, host));
        return 1;
    }

    if (strcmp(func_name, "enable_metrics") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_server_enable_metrics((MS_Server *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "enable_tracing") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_server_enable_tracing((MS_Server *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "enable_health") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_server_enable_health((MS_Server *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "enable_pprof") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_server_enable_pprof((MS_Server *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "server_start") == 0) {
        MS_Server *server = (MS_Server *)parse_handle_from_object(args[0]);
        int ok = ms_server_start(server);
        if (!ok) {
            result->error = ms_server_last_error(server);
            return 1;
        }
        result->has_value = 1;
        result->value = make_int_local(ok);
        return 1;
    }
    if (strcmp(func_name, "server_stop") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_server_stop((MS_Server *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "server_state") == 0) {
        MS_Server *server = (MS_Server *)parse_handle_from_object(args[0]);
        char **keys = calloc(21, sizeof(char *));
        Value *values = calloc(21, sizeof(Value));
        keys[0] = copy_text("running");
        values[0] = make_bool_local(server && server->running);
        keys[1] = copy_text("ready");
        values[1] = make_bool_local(server && server->ready);
        keys[2] = copy_text("stopping");
        values[2] = make_bool_local(server && server->stop_requested);
        keys[3] = copy_text("draining");
        values[3] = make_bool_local(server && server->stop_requested && server->active_connections > 0);
        keys[4] = copy_text("active_connections");
        values[4] = make_int_local(server ? server->active_connections : 0);
        keys[5] = copy_text("shutdown_deadline_ms");
        values[5] = make_int_local(server ? server->shutdown_deadline_ms : 0);
        keys[6] = copy_text("accepting");
        values[6] = make_bool_local(server && server->listen_fd >= 0 && !server->stop_requested);
        keys[7] = copy_text("stopped");
        values[7] = make_bool_local(server && !server->running && server->stop_requested);
        keys[8] = copy_text("metrics_enabled");
        values[8] = make_bool_local(server && server->metrics_enabled);
        keys[9] = copy_text("tracing_enabled");
        values[9] = make_bool_local(server && server->tracing_enabled);
        keys[10] = copy_text("otlp_endpoint");
        values[10] = make_string_copy_local((server && server->tracer && server->tracer->otlp_endpoint) ? server->tracer->otlp_endpoint : "");
        keys[11] = copy_text("traces_endpoint");
        values[11] = make_string_copy_local((server && server->tracer && server->tracer->traces_endpoint) ? server->tracer->traces_endpoint : "");
        keys[12] = copy_text("logs_endpoint");
        values[12] = make_string_copy_local((server && server->tracer && server->tracer->logs_endpoint) ? server->tracer->logs_endpoint : "");
        keys[13] = copy_text("otlp_timeout_ms");
        values[13] = make_int_local(server && server->tracer ? server->tracer->timeout_ms : 0);
        keys[14] = copy_text("otlp_retry_count");
        values[14] = make_int_local(server && server->tracer ? server->tracer->retry_count : 0);
        keys[15] = copy_text("otlp_backoff_ms");
        values[15] = make_int_local(server && server->tracer ? server->tracer->backoff_ms : 0);
        keys[16] = copy_text("otlp_service_name");
        values[16] = make_string_copy_local((server && server->tracer && server->tracer->service_name) ? server->tracer->service_name : "");
        keys[17] = copy_text("otlp_logs_enabled");
        values[17] = make_bool_local(server && server->tracer && server->tracer->export_logs);
        keys[18] = copy_text("otlp_protocol");
        values[18] = make_string_copy_local((server && server->tracer && server->tracer->protocol) ? server->tracer->protocol : "http/json");
        keys[19] = copy_text("otlp_traces_protocol");
        values[19] = make_string_copy_local((server && server->tracer && server->tracer->traces_protocol) ? server->tracer->traces_protocol : "");
        keys[20] = copy_text("otlp_logs_protocol");
        values[20] = make_string_copy_local((server && server->tracer && server->tracer->logs_protocol) ? server->tracer->logs_protocol : "");
        result->has_value = 1;
        result->value = make_object_local(keys, values, 21);
        return 1;
    }

    if (strcmp(func_name, "middleware_logger") == 0) {
        result->has_value = 1;
        result->value = make_handle_object(ms_middleware_logger(), "ms.middleware");
        return 1;
    }
    if (strcmp(func_name, "middleware_cors") == 0) {
        MS_CorsConfig config;
        memset(&config, 0, sizeof(config));
        config.enabled = (int)get_int_field(args[0], "enabled", 1);
        result->has_value = 1;
        result->value = make_handle_object(ms_middleware_cors(&config), "ms.middleware");
        return 1;
    }
    if (strcmp(func_name, "middleware_compress") == 0) {
        result->has_value = 1;
        result->value = make_handle_object(ms_middleware_compress(), "ms.middleware");
        return 1;
    }
    if (strcmp(func_name, "middleware_rate_limit") == 0) {
        int window_seconds = (int)get_int_field(args[0], "window", 60);
        int max_requests = (int)get_int_field(args[0], "max", 100);
        result->has_value = 1;
        result->value = make_handle_object(ms_middleware_rate_limit(window_seconds, max_requests), "ms.middleware");
        return 1;
    }

    if (strcmp(func_name, "log_info") == 0) {
        char *fields_json = json_stringify_value_local(args[1]);
        ms_log_info(args[0].as.string, fields_json);
        free(fields_json);
        result->has_value = 1;
        result->value = make_int_local(1);
        return 1;
    }
    if (strcmp(func_name, "log_error") == 0) {
        char *fields_json = json_stringify_value_local(args[1]);
        ms_log_error(args[0].as.string, fields_json);
        free(fields_json);
        result->has_value = 1;
        result->value = make_int_local(1);
        return 1;
    }
    if (strcmp(func_name, "test_sleep_ms") == 0) {
        if (args[0].type == VALUE_INT && args[0].as.integer > 0) {
            usleep((useconds_t)(args[0].as.integer * 1000));
        }
        result->has_value = 1;
        result->value = make_int_local(1);
        return 1;
    }

    if (strcmp(func_name, "ctx_header") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_header((MS_Context *)parse_handle_from_object(args[0]), args[1].as.string));
        return 1;
    }
    if (strcmp(func_name, "ctx_query") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_query((MS_Context *)parse_handle_from_object(args[0]), args[1].as.string));
        return 1;
    }
    if (strcmp(func_name, "ctx_param") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_param((MS_Context *)parse_handle_from_object(args[0]), args[1].as.string));
        return 1;
    }
    if (strcmp(func_name, "ctx_body") == 0) {
        MS_Context *ctx = (MS_Context *)parse_handle_from_object(args[0]);
        const char *raw = ms_context_body(ctx);
        const char *content_type = ms_context_header(ctx, "Content-Type");
        char **keys = calloc(5, sizeof(char *));
        Value *values = calloc(5, sizeof(Value));
        keys[0] = copy_text("raw");
        values[0] = make_string_copy_local(raw);
        keys[1] = copy_text("content_type");
        values[1] = make_string_copy_local(content_type);
        keys[2] = copy_text("length");
        values[2] = make_int_local((long long)strlen(raw));
        keys[3] = copy_text("json");
        if (content_type && strstr(content_type, "application/json") != NULL && raw && raw[0] != '\0') {
            values[3] = body_parse_json_local(raw);
        } else {
            values[3] = make_void_local();
        }
        keys[4] = copy_text("form");
        if (content_type && strstr(content_type, "application/x-www-form-urlencoded") != NULL && raw && raw[0] != '\0') {
            values[4] = body_parse_form_local(raw);
        } else {
            values[4] = make_object_local(NULL, NULL, 0);
        }
        result->has_value = 1;
        result->value = make_object_local(keys, values, 5);
        return 1;
    }
    if (strcmp(func_name, "ctx_status") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_context_status((MS_Context *)parse_handle_from_object(args[0]), (int)args[1].as.integer));
        return 1;
    }
    if (strcmp(func_name, "ctx_json") == 0) {
        char *json_body = json_stringify_value_local(args[1]);
        result->has_value = 1;
        result->value = make_int_local(ms_context_json((MS_Context *)parse_handle_from_object(args[0]), json_body));
        free(json_body);
        return 1;
    }
    if (strcmp(func_name, "ctx_set_header") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_context_set_header((MS_Context *)parse_handle_from_object(args[0]), args[1].as.string, args[2].as.string));
        return 1;
    }
    if (strcmp(func_name, "ctx_is_cancelled") == 0) {
        Value value;
        value.type = VALUE_BOOL;
        value.as.boolean = ms_context_is_cancelled((MS_Context *)parse_handle_from_object(args[0])) ? 1 : 0;
        result->has_value = 1;
        result->value = value;
        return 1;
    }
    if (strcmp(func_name, "ctx_cancel_reason") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_cancel_reason((MS_Context *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "ctx_deadline_ms") == 0) {
        result->has_value = 1;
        result->value = make_int_local(ms_context_deadline_ms((MS_Context *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "ctx_request_id") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_request_id((MS_Context *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "ctx_trace_id") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_trace_id((MS_Context *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "ctx_span_id") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_span_id((MS_Context *)parse_handle_from_object(args[0])));
        return 1;
    }
    if (strcmp(func_name, "ctx_correlation_id") == 0) {
        result->has_value = 1;
        result->value = make_string_copy_local(ms_context_correlation_id((MS_Context *)parse_handle_from_object(args[0])));
        return 1;
    }

    result->error = "unknown microservice native function";
    return 1;
}

const ModuleNativeRuntimeProvider *p21_module_native_runtime_provider(void) {
    static const ModuleNativeRuntimeProvider provider = {
        "microservice",
        p21_module_microservice_invoke
    };
    return &provider;
}
