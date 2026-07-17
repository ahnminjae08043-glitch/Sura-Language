#include "../sura_plugin.h"
#include <time.h>

typedef struct SampleState {
    int call_count;
    int lifecycle_count;
} SampleState;

static SampleState* sample_state(SuraPluginContext* ctx) {
    return ctx ? (SampleState*)ctx->user_data : 0;
}

static void sample_count_call(SuraPluginContext* ctx) {
    SampleState* state = sample_state(ctx);
    if (state) {
        state->call_count += 1;
    }
}

static int sample_spin_ms_cancellable(SuraPluginContext* ctx, int ms) {
    if (ms <= 0) {
        return 0;
    }
    clock_t start = clock();
    clock_t ticks = (clock_t)((double)ms * (double)CLOCKS_PER_SEC / 1000.0);
    while ((clock() - start) < ticks) {
        if (ctx && ctx->host && ctx->host->should_cancel && ctx->host->should_cancel()) {
            return SURA_PLUGIN_CANCELLED;
        }
    }
    return 0;
}

static int native_add(
    SuraPluginContext* ctx,
    const SuraPluginValue* args,
    int arg_count,
    SuraPluginValue* out
) {
    (void)ctx;
    if (arg_count != 2 || args[0].type != SURA_PLUGIN_NUMBER || args[1].type != SURA_PLUGIN_NUMBER) {
        return -1;
    }
    sample_count_call(ctx);
    out->type = SURA_PLUGIN_NUMBER;
    out->as.number_value = args[0].as.number_value + args[1].as.number_value;
    return 0;
}

static int native_mul(
    SuraPluginContext* ctx,
    const SuraPluginValue* args,
    int arg_count,
    SuraPluginValue* out
) {
    (void)ctx;
    if (arg_count != 2 || args[0].type != SURA_PLUGIN_NUMBER || args[1].type != SURA_PLUGIN_NUMBER) {
        return -1;
    }
    sample_count_call(ctx);
    out->type = SURA_PLUGIN_NUMBER;
    out->as.number_value = args[0].as.number_value * args[1].as.number_value;
    return 0;
}

static int native_call_count(
    SuraPluginContext* ctx,
    const SuraPluginValue* args,
    int arg_count,
    SuraPluginValue* out
) {
    (void)args;
    if (arg_count != 0) {
        return -1;
    }
    SampleState* state = sample_state(ctx);
    out->type = SURA_PLUGIN_NUMBER;
    out->as.number_value = state ? state->call_count : -1;
    return 0;
}

static int native_lifecycle_count(
    SuraPluginContext* ctx,
    const SuraPluginValue* args,
    int arg_count,
    SuraPluginValue* out
) {
    (void)args;
    if (arg_count != 0) {
        return -1;
    }
    SampleState* state = sample_state(ctx);
    out->type = SURA_PLUGIN_NUMBER;
    out->as.number_value = state ? state->lifecycle_count : -1;
    return 0;
}

static int native_spin_ms(
    SuraPluginContext* ctx,
    const SuraPluginValue* args,
    int arg_count,
    SuraPluginValue* out
) {
    if (arg_count != 1 || args[0].type != SURA_PLUGIN_NUMBER) {
        return -1;
    }
    int ms = (int)args[0].as.number_value;
    if ((double)ms != args[0].as.number_value || ms < 0 || ms > 5000) {
        return -1;
    }
    sample_count_call(ctx);
    int spin_rc = sample_spin_ms_cancellable(ctx, ms);
    if (spin_rc != 0) {
        return spin_rc;
    }
    out->type = SURA_PLUGIN_NUMBER;
    out->as.number_value = (double)ms;
    return 0;
}

static void destroy_sample_state(const SuraPluginHostApi* host, void* user_data) {
    if (host && host->free && user_data) {
        host->free(user_data);
    }
}

static const SuraPluginExport exports[] = {
    {"native_add", native_add, 2, 2, "native_add(a, b) -> number"},
    {"native_mul", native_mul, 2, 2, "native_mul(a, b) -> number"},
    {"native_call_count", native_call_count, 0, 0, "native_call_count() -> number"},
    {"native_lifecycle_count", native_lifecycle_count, 0, 0, "native_lifecycle_count() -> number"},
    {"native_spin_ms", native_spin_ms, 1, 1, "native_spin_ms(ms) -> number"}
};

SURA_PLUGIN_EXPORT int SURA_PLUGIN_ON_LOAD(SuraPluginContext* ctx) {
    SampleState* state = sample_state(ctx);
    if (!state) {
        return -1;
    }
    state->lifecycle_count += 1;
    return 0;
}

SURA_PLUGIN_EXPORT int SURA_PLUGIN_ON_UNLOAD(SuraPluginContext* ctx) {
    SampleState* state = sample_state(ctx);
    if (state) {
        state->lifecycle_count += 1;
    }
    return 0;
}

SURA_PLUGIN_EXPORT int SURA_PLUGIN_INIT(
    const SuraPluginHostApi* host,
    SuraPluginDescriptor* out_descriptor
) {
    if (!host || !out_descriptor || host->abi_version != SURA_PLUGIN_ABI_VERSION) {
        return -1;
    }
    SampleState* state = (SampleState*)host->alloc(sizeof(SampleState));
    if (!state) {
        return -1;
    }
    state->call_count = 0;
    state->lifecycle_count = 0;
    out_descriptor->abi_version = SURA_PLUGIN_ABI_VERSION;
    out_descriptor->name = "sura_sample_plugin";
    out_descriptor->version = "0.1.0";
    out_descriptor->exports = exports;
    out_descriptor->export_count = sizeof(exports) / sizeof(exports[0]);
    out_descriptor->user_data = state;
    out_descriptor->destroy_user_data = destroy_sample_state;
    return 0;
}
