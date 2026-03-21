#ifndef P21_MICROSERVICE_OBSERVABILITY_H
#define P21_MICROSERVICE_OBSERVABILITY_H

#include "microservice.h"

typedef struct {
    const char *trace_id;
    const char *span_id;
    const char *request_id;
    const char *correlation_id;
    const char *ip;
    const char *user_agent;
} MS_LogContext;

void ms_log_json(const char *level, const char *message, const char *fields_json, const MS_LogContext *ctx);
void ms_trace_request(const char *name,
                      const char *method,
                      const char *route,
                      const char *path,
                      int status_code,
                      long long duration_ms,
                      const MS_LogContext *ctx,
                      const MS_Tracer *tracer);

#endif
