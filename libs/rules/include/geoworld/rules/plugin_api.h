#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEOWORLD_RULE_PLUGIN_ABI_VERSION 1u
#define GEOWORLD_RULE_PLUGIN_ENTRY_SYMBOL "geoworld_rule_plugin_entry"

#if defined(_WIN32) && defined(GEOWORLD_RULE_PLUGIN_BUILD)
#define GEOWORLD_RULE_PLUGIN_EXPORT __declspec(dllexport)
#elif !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define GEOWORLD_RULE_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define GEOWORLD_RULE_PLUGIN_EXPORT
#endif

typedef struct geoworld_rule_event_v1 {
    const char* type;
    uint64_t target_tick;
    uint64_t subject_id;
    const uint8_t* payload;
    uint32_t payload_size;
} geoworld_rule_event_v1;

typedef struct geoworld_rule_host_v1 {
    uint32_t abi_version;
    void* user;
    int (*publish_event)(void* user, const geoworld_rule_event_v1* event);
    int (*set_property)(void* user, uint64_t subject_id,
                        const char* key, const uint8_t* value, uint32_t value_size);
} geoworld_rule_host_v1;

typedef struct geoworld_rule_plugin_v1 {
    uint32_t abi_version;
    const char* plugin_id;
    int (*initialize)(const geoworld_rule_host_v1* host);
    int (*on_event)(const geoworld_rule_event_v1* event);
    void (*shutdown)(void);
} geoworld_rule_plugin_v1;

typedef const geoworld_rule_plugin_v1* (*geoworld_rule_plugin_entry_v1)(void);

GEOWORLD_RULE_PLUGIN_EXPORT const geoworld_rule_plugin_v1*
geoworld_rule_plugin_entry(void);

#ifdef __cplusplus
}
#endif
