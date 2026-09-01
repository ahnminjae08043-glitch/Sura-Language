#ifndef SURA_PLUGIN_H
#define SURA_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #define SURA_PLUGIN_EXPORT __declspec(dllexport)
#else
  #define SURA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SURA_PLUGIN_ABI_VERSION_MAJOR 1
#define SURA_PLUGIN_ABI_VERSION_MINOR 1
#define SURA_PLUGIN_ABI_VERSION_PATCH 0
#define SURA_PLUGIN_ABI_VERSION ((SURA_PLUGIN_ABI_VERSION_MAJOR * 10000u) + (SURA_PLUGIN_ABI_VERSION_MINOR * 100u) + SURA_PLUGIN_ABI_VERSION_PATCH)

#define SURA_PLUGIN_CANCELLED 2

typedef enum SuraPluginValueType {
    SURA_PLUGIN_NIL = 0,
    SURA_PLUGIN_NUMBER = 1,
    SURA_PLUGIN_BOOL = 2,
    SURA_PLUGIN_STRING = 3
} SuraPluginValueType;

typedef struct SuraPluginValue {
    SuraPluginValueType type;
    union {
        double number_value;
        int bool_value;
        const char* string_value;
    } as;
} SuraPluginValue;

typedef struct SuraPluginHostApi {
    uint32_t abi_version;
    void (*log)(const char* message);
    void* (*alloc)(size_t bytes);
    void (*free)(void* ptr);
    int (*should_cancel)(void);
} SuraPluginHostApi;

typedef struct SuraPluginContext {
    const SuraPluginHostApi* host;
    void* user_data;
} SuraPluginContext;

typedef int (*SuraPluginFn)(
    SuraPluginContext* ctx,
    const SuraPluginValue* args,
    int arg_count,
    SuraPluginValue* out
);

typedef void (*SuraPluginDestroyUserDataFn)(
    const SuraPluginHostApi* host,
    void* user_data
);

typedef int (*SuraPluginLifecycleFn)(
    SuraPluginContext* ctx
);

typedef struct SuraPluginExport {
    const char* name;
    SuraPluginFn function;
    int min_args;
    int max_args;
    const char* doc;
} SuraPluginExport;

typedef struct SuraPluginDescriptor {
    uint32_t abi_version;
    const char* name;
    const char* version;
    const SuraPluginExport* exports;
    size_t export_count;
    void* user_data;
    SuraPluginDestroyUserDataFn destroy_user_data;
} SuraPluginDescriptor;

typedef int (*SuraPluginInitFn)(
    const SuraPluginHostApi* host,
    SuraPluginDescriptor* out_descriptor
);

#define SURA_PLUGIN_INIT sura_plugin_init
#define SURA_PLUGIN_ON_LOAD sura_plugin_on_load
#define SURA_PLUGIN_ON_UNLOAD sura_plugin_on_unload

SURA_PLUGIN_EXPORT int SURA_PLUGIN_INIT(
    const SuraPluginHostApi* host,
    SuraPluginDescriptor* out_descriptor
);

#ifdef __cplusplus
}
#endif

#endif
