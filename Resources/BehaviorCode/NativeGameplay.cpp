#include "WorldForgeNative.h"
#include "WorldForgeNativeComponents.h"
#include <string_view>

namespace {
constexpr std::string_view targetId = ""; // Paste the ID of a Dynamic physics mesh here.

void start(const WfGameHostV1 *host, void *context, const WfGameFrameV1 *) noexcept
{
    constexpr std::string_view message = "Native gameplay started.";
    host->log(context,message.data(),uint32_t(message.size()));
    const auto body = host->find_body(context,targetId.data(),uint32_t(targetId.size()));
    if (host->is_dynamic(context,body))
        host->set_velocity(context,body,{0,0,8});
}

void fixedUpdate(const WfGameHostV1 *,void *,const WfGameFrameV1 *) noexcept
{
}

void stop(const WfGameHostV1 *host,void *context,const WfGameFrameV1 *) noexcept
{
    constexpr std::string_view message = "Native gameplay stopped.";
    host->log(context,message.data(),uint32_t(message.size()));
}

constexpr WfGameStringV1 string(std::string_view value)
{
    return {value.data(),uint32_t(value.size())};
}

void startLaunch(const WfGameHostV1 *host,void *context,const WfGameFrameV1 *,
                 const WfGameInstanceV1 *instance) noexcept
{
    const auto value = [instance](std::string_view id) {
        WfGameValueV1 result{};
        instance->get(instance->properties,string(id),&result);
        return result;
    };
    if (!value("launch").number)
        return;
    auto body = instance->owner_body;
    const auto target = value("target").reference;
    if (target.size)
        body = host->find_body(context,target.data,target.size);
    const auto speed = float(value("speed").number*value("percent").number/100);
    if (host->set_velocity(context,body,{0,0,speed})) {
        const auto message = value("mesh").reference.size ? string("Launch component ran with a mesh reference.")
            : string("Launch component ran.");
        host->log(context,message.data,message.size);
    }
}

constexpr WfGamePropertyV1 launchFields[]{
    {string("speed"),string("Upward speed"),{WF_GAME_REAL,8,{}}},
    {string("percent"),string("Speed percent"),{WF_GAME_INT,100,{}}},
    {string("launch"),string("Launch on Play"),{WF_GAME_BOOL,1,{}}},
    {string("target"),string("Target object"),{WF_GAME_OBJECT,0,{}}},
    {string("mesh"),string("Mesh reference"),{WF_GAME_MESH,0,{}}},
    {string("material"),string("Material reference"),{WF_GAME_MATERIAL,0,{}}},
    {string("texture"),string("Texture reference"),{WF_GAME_TEXTURE,0,{}}}
};
constexpr WfGameComponentV1 componentTypes[]{
    {string("worldforge.launch"),string("Launch Body"),launchFields,
        uint32_t(sizeof(launchFields)/sizeof(*launchFields)),startLaunch,nullptr,nullptr}
};
}

WF_GAME_EXPORT const WfGameModuleV1 *WF_GAME_CALL WfGameGetModuleV1() noexcept
{
    static constexpr WfGameModuleV1 module{sizeof(WfGameModuleV1),WF_GAME_ABI_V1,start,fixedUpdate,stop};
    return &module;
}

WF_GAME_EXPORT const WfGameComponentsV1 *WF_GAME_CALL WfGameGetComponentsV1() noexcept
{
    static constexpr WfGameComponentsV1 components{sizeof(WfGameComponentsV1),WF_GAME_COMPONENTS_ABI_V1,componentTypes,1};
    return &components;
}
