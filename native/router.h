#ifndef P21_MICROSERVICE_ROUTER_H
#define P21_MICROSERVICE_ROUTER_H

#include "microservice.h"

int ms_router_add_route(MS_Route **head, MS_HttpMethod method, const char *path, MS_HandlerFn handler, void *userdata);
MS_Route *ms_router_match(MS_Route *head, const char *path, MS_Request *req);

#endif
