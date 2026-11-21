#pragma once

#include "WorldForgeNative.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WF_GAME_COMPONENTS_ABI_V1 1u

enum WfGamePropertyKind {
    WF_GAME_BOOL = 1, WF_GAME_INT = 2, WF_GAME_REAL = 3, WF_GAME_OBJECT = 4,
    WF_GAME_MESH = 5, WF_GAME_MATERIAL = 6, WF_GAME_TEXTURE = 7
};

typedef struct WfGameStringV1 { const char *data; uint32_t size; } WfGameStringV1;
typedef struct WfGameValueV1 {
    uint32_t kind;
    double number; /* Boolean, signed 32-bit integer, or real. */
    WfGameStringV1 reference; /* Stable ID for object and asset fields. */
} WfGameValueV1;

typedef struct WfGamePropertyV1 {
    WfGameStringV1 id, label;
    WfGameValueV1 initial;
} WfGamePropertyV1;

/* Instance pointers, values, and context are borrowed for this callback only. */
typedef struct WfGameInstanceV1 {
    uint32_t size;
    WfGameStringV1 owner_id;
    WfGameBody owner_body;
    const void *properties;
    int (WF_GAME_CALL *get)(const void *properties, WfGameStringV1 id, WfGameValueV1 *out) WF_GAME_NOEXCEPT;
} WfGameInstanceV1;

typedef void (WF_GAME_CALL *WfGameComponentCallbackV1)(const WfGameHostV1 *, void *context,
    const WfGameFrameV1 *, const WfGameInstanceV1 *) WF_GAME_NOEXCEPT;

typedef struct WfGameComponentV1 {
    WfGameStringV1 id, label;
    const WfGamePropertyV1 *properties;
    uint32_t property_count;
    WfGameComponentCallbackV1 start, fixed_update, stop;
} WfGameComponentV1;

typedef struct WfGameComponentsV1 {
    uint32_t size, abi_version;
    const WfGameComponentV1 *types;
    uint32_t type_count;
} WfGameComponentsV1;

/* Optional DLL export named WfGameGetComponentsV1. Tables remain valid for the DLL's lifetime. */
typedef const WfGameComponentsV1 *(WF_GAME_CALL *WfGameGetComponentsV1Fn)(void) WF_GAME_NOEXCEPT;

#ifdef __cplusplus
}
#endif
