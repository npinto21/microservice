#ifndef P21_MICROSERVICE_MIDDLEWARE_H
#define P21_MICROSERVICE_MIDDLEWARE_H

#include "microservice.h"

int ms_middleware_append(MS_Middleware **head, MS_Middleware *middleware);
MS_Middleware *ms_middleware_clone_chain(MS_Middleware *head);
int ms_middleware_execute_chain(MS_Middleware *head, MS_Context *ctx);
int ms_middleware_chain_has_name(MS_Middleware *head, const char *name);
void ms_middleware_attach_chain(MS_Middleware **head, MS_Middleware *chain);

#endif
