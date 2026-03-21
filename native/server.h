#ifndef P21_MICROSERVICE_SERVER_H
#define P21_MICROSERVICE_SERVER_H

#include "microservice.h"

MS_Result ms_server_result(MS_Server *server, const char *error);
MS_Result ms_group_result(MS_Group *group, const char *error);

#endif
