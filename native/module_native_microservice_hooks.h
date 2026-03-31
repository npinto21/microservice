#ifndef P21_MODULE_NATIVE_MICROSERVICE_HOOKS_H
#define P21_MODULE_NATIVE_MICROSERVICE_HOOKS_H

#include "modules/native/module_native_hooks.h"

const NativePackageSpec *p21_module_microservice_find_package(const char *path);
int p21_module_microservice_invoke(
    const char *package_path,
    const char *func_name,
    Value *args,
    int arg_count,
    const ModuleNativeHostApi *host,
    ModuleNativeInvokeResult *result
);

#endif
