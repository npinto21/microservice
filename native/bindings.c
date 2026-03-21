#include "microservice.h"

/*
 * This file is the bridge boundary for the Pinto21 microservice module.
 * The current module runtime uses module_native_microservice_runtime.c
 * as the direct invoke path, but this translation unit remains the
 * natural place for future VM/runtime registration helpers, adapter
 * shims, and optional ABI-stable bridge exports.
 */

int p21_microservice_bindings_anchor(void) {
    return 1;
}
