#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define WF_GAME_NOEXCEPT noexcept
#define WF_GAME_EXTERN extern "C"
#else
#define WF_GAME_NOEXCEPT
#define WF_GAME_EXTERN extern
#endif

#ifdef _WIN32
#define WF_GAME_CALL __cdecl
#define WF_GAME_EXPORT WF_GAME_EXTERN __declspec(dllexport)
#else
#define WF_GAME_CALL
#define WF_GAME_EXPORT WF_GAME_EXTERN
#endif

#define WF_GAME_ABI_V1 1u

typedef uint32_t WfGameBody;
typedef struct WfGameVec3 { float x, y, z; } WfGameVec3;

enum WfGameInput {
    WF_GAME_FORWARD = 1, WF_GAME_BACK = 2, WF_GAME_LEFT = 4, WF_GAME_RIGHT = 8,
    WF_GAME_JUMP = 16, WF_GAME_INTERACT = 32
};

/* Handles and context are valid only inside callbacks on the game thread. Zero means no body. */
typedef struct WfGameHostV1 {
    uint32_t size;
    WfGameBody (WF_GAME_CALL *player)(void *context) WF_GAME_NOEXCEPT;
    WfGameBody (WF_GAME_CALL *find_body)(void *context, const char *object_id, uint32_t len) WF_GAME_NOEXCEPT;
    int (WF_GAME_CALL *position)(void *context, WfGameBody body, WfGameVec3 *out) WF_GAME_NOEXCEPT;
    int (WF_GAME_CALL *is_dynamic)(void *context, WfGameBody body) WF_GAME_NOEXCEPT;
    int (WF_GAME_CALL *set_velocity)(void *context, WfGameBody body, WfGameVec3 velocity) WF_GAME_NOEXCEPT;
    void (WF_GAME_CALL *log)(void *context, const char *utf8, uint32_t len) WF_GAME_NOEXCEPT;
} WfGameHostV1;

typedef struct WfGameFrameV1 {
    uint32_t size;
    uint32_t input;
    uint64_t step;
    float delta_seconds;
} WfGameFrameV1;

typedef void (WF_GAME_CALL *WfGameCallbackV1)(const WfGameHostV1 *, void *context, const WfGameFrameV1 *) WF_GAME_NOEXCEPT;

typedef struct WfGameModuleV1 {
    uint32_t size;
    uint32_t abi_version;
    WfGameCallbackV1 start;
    WfGameCallbackV1 fixed_update;
    WfGameCallbackV1 stop;
} WfGameModuleV1;

/* Export WfGameGetModuleV1 with this signature. The descriptor must live for the DLL's lifetime. */
typedef const WfGameModuleV1 *(WF_GAME_CALL *WfGameGetModuleV1Fn)(void) WF_GAME_NOEXCEPT;

#ifdef __cplusplus
}
#endif
