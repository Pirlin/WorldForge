#include "Engine/World/NativeGameplay.h"

#include <algorithm>
#include <cmath>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#endif

namespace worldforge {
namespace {
struct Context {
    WorldSimulation &world;
    std::span<const NativeBodyBinding> bodies;
    std::optional<std::size_t> player;
    NativeGameplay::Log writeLog;
    BoxBody *body(WfGameBody handle) noexcept
    {
        return handle > 0 && handle <= world.bodies.size() ? &world.bodies[handle-1] : nullptr;
    }
};

const WfGameHostV1 host{
    sizeof(WfGameHostV1),
    [](void *context) noexcept -> WfGameBody {
        const auto &state = *static_cast<Context *>(context);
        return state.player ? WfGameBody(*state.player+1) : 0;
    },
    [](void *context,const char *id,std::uint32_t size) noexcept -> WfGameBody {
        if (!id || size == 0 || size > 128)
            return 0;
        const auto &bodies = static_cast<Context *>(context)->bodies;
        const auto found = std::ranges::find(bodies,std::string_view(id,size),&NativeBodyBinding::id);
        return found != bodies.end() ? WfGameBody(found->index+1) : 0;
    },
    [](void *context,WfGameBody handle,WfGameVec3 *out) noexcept -> int {
        const auto *body = static_cast<Context *>(context)->body(handle);
        if (!body || !out)
            return 0;
        *out = {body->position[0],body->position[1],body->position[2]};
        return 1;
    },
    [](void *context,WfGameBody handle) noexcept -> int {
        const auto *body = static_cast<Context *>(context)->body(handle);
        return body && body->dynamic;
    },
    [](void *context,WfGameBody handle,WfGameVec3 velocity) noexcept -> int {
        auto *body = static_cast<Context *>(context)->body(handle);
        const Point3 value{velocity.x,velocity.y,velocity.z};
        if (!body || !body->dynamic || !std::ranges::all_of(value,[](float speed) { return std::abs(speed) <= 100; }))
            return 0;
        body->velocity = value;
        return 1;
    },
    [](void *context,const char *utf8,std::uint32_t size) noexcept {
        const auto write = static_cast<Context *>(context)->writeLog;
        if (write && utf8 && size)
            write(std::string_view(utf8,std::min(size,1024u)));
    }
};
}

struct NativeGameplay::Library {
    const WfGameModuleV1 *api = nullptr;
    struct Component {
        NativeInstance instance;
        const WfGameComponentV1 *type = nullptr;
    };
    std::vector<NativeComponentType> types;
    std::vector<Component> components;
    void invoke(Context &context,const WfGameFrameV1 &frame,WfGameComponentCallbackV1 WfGameComponentV1::*callback)
    {
        for (const auto &component : components) {
            const auto call = component.type->*callback;
            if (!call)
                continue;
            const auto owner = WfGameStringV1{component.instance.owner.data(),std::uint32_t(component.instance.owner.size())};
            const WfGameInstanceV1 instance{sizeof(WfGameInstanceV1),owner,host.find_body(&context,owner.data,owner.size),
                &component.instance,[](const void *properties,WfGameStringV1 id,WfGameValueV1 *out) noexcept -> int {
                    if (!out || !id.data || !id.size || id.size > 64)
                        return 0;
                    const auto &values = static_cast<const NativeInstance *>(properties)->properties;
                    const auto match = values.find(std::string_view(id.data,id.size));
                    if (match == values.end())
                        return 0;
                    const auto &value = match->second;
                    *out = {value.kind,value.number,{value.reference.data(),std::uint32_t(value.reference.size())}};
                    return 1;
                }};
            call(&host,&context,&frame,&instance);
        }
    }
#ifdef _WIN32
    HMODULE handle = nullptr;
    std::filesystem::path shadow;
    ~Library()
    {
        if (handle)
            FreeLibrary(handle);
        if (!shadow.empty()) {
            std::error_code ignored;
            std::filesystem::remove(shadow / "Gameplay.dll",ignored);
            std::filesystem::remove(shadow / "Gameplay.pdb",ignored);
            std::filesystem::remove(shadow,ignored);
        }
    }
#endif
};

NativeGameplay::NativeGameplay() = default;
NativeGameplay::~NativeGameplay() = default;
NativeGameplay::NativeGameplay(NativeGameplay &&) noexcept = default;
NativeGameplay &NativeGameplay::operator=(NativeGameplay &&) noexcept = default;

bool NativePropertyValue::valid() const
{
    switch (kind) {
    case WF_GAME_BOOL:
        return reference.empty() && (number == 0 || number == 1);
    case WF_GAME_INT:
        return reference.empty() && number >= INT32_MIN && number <= INT32_MAX && number == std::trunc(number);
    case WF_GAME_REAL:
        return reference.empty() && std::abs(number) <= 1e9;
    case WF_GAME_OBJECT:
    case WF_GAME_MESH:
    case WF_GAME_MATERIAL:
    case WF_GAME_TEXTURE:
        return number == 0 && reference.size() <= 128 && reference.find('\0') == std::string::npos;
    default:
        return false;
    }
}

const std::vector<NativeComponentType> &NativeGameplay::componentTypes() const
{
    static const std::vector<NativeComponentType> empty;
    if (pending_)
        return pending_->types;
    return active_ ? active_->types : empty;
}

bool NativeGameplay::stage(const std::filesystem::path &path,std::string &error,std::span<const NativeInstance> instances)
{
#ifdef _WIN32
    std::error_code ec;
    const auto source = std::filesystem::absolute(path,ec);
    if (ec || !std::filesystem::is_regular_file(source,ec)) {
        error = "The C++ gameplay DLL was not found.";
        return false;
    }
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (ec) {
        error = "Windows did not provide a temporary directory for gameplay.";
        return false;
    }
    static std::atomic_uint64_t generation{0};
    auto candidate = std::make_unique<Library>();
    for (int retry = 0; retry < 10; ++retry) {
        const auto directory = temp / ("worldforge-gameplay-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(++generation));
        if (std::filesystem::create_directory(directory,ec)) {
            candidate->shadow = directory;
            break;
        }
        if (ec)
            break;
    }
    if (candidate->shadow.empty()) {
        error = "Could not create a private copy of the C++ gameplay DLL.";
        return false;
    }
    const auto binary = candidate->shadow / "Gameplay.dll";
    if (!std::filesystem::copy_file(source,binary,ec)) {
        error = "Could not copy the C++ gameplay DLL.";
        return false;
    }
    auto symbols = source;
    symbols.replace_extension(".pdb");
    if (std::filesystem::is_regular_file(symbols,ec))
        std::filesystem::copy_file(symbols,candidate->shadow / "Gameplay.pdb",ec);
    candidate->handle = LoadLibraryExW(binary.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!candidate->handle) {
        error = "Windows could not load the C++ gameplay DLL (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    const auto entry = GetProcAddress(candidate->handle,"WfGameGetModuleV1");
    if (entry)
        candidate->api = reinterpret_cast<WfGameGetModuleV1Fn>(entry)();
    if (!candidate->api || candidate->api->size < sizeof(WfGameModuleV1)
        || candidate->api->abi_version != WF_GAME_ABI_V1 || !candidate->api->fixed_update) {
        error = "Incompatible C++ gameplay DLL. Export WfGameGetModuleV1 and provide ABI v1 and fixed_update.";
        return false;
    }
    const WfGameComponentsV1 *extension = nullptr;
    if (const auto address = GetProcAddress(candidate->handle,"WfGameGetComponentsV1")) {
        extension = reinterpret_cast<WfGameGetComponentsV1Fn>(address)();
        if (!extension || extension->size < sizeof(WfGameComponentsV1)
            || extension->abi_version != WF_GAME_COMPONENTS_ABI_V1 || extension->type_count > 64
            || (extension->type_count && !extension->types)) {
            error = "Incompatible native component descriptor.";
            return false;
        }
    }
    const auto read = [](WfGameStringV1 str,std::size_t limit) {
        return str.data && str.size && str.size <= limit ? std::string(str.data,str.size) : std::string{};
    };
    const auto idValid = [](std::string_view id) {
        return !id.empty() && std::ranges::all_of(id,[](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
    };
    if (extension) {
        std::set<std::string> typeIds;
        for (std::uint32_t index = 0; index < extension->type_count; ++index) {
            const auto &descriptor = extension->types[index];
            NativeComponentType type{read(descriptor.id,64),read(descriptor.label,120),{}};
            if (!idValid(type.id) || type.label.empty() || !typeIds.insert(type.id).second
                || descriptor.property_count > 32 || (descriptor.property_count && !descriptor.properties)
                || !(descriptor.start || descriptor.fixed_update || descriptor.stop)) {
                error = "Invalid or duplicate native component type.";
                return false;
            }
            std::set<std::string> propertyIds;
            for (std::uint32_t field = 0; field < descriptor.property_count; ++field) {
                const auto &value = descriptor.properties[field];
                NativeProperty property{read(value.id,64),read(value.label,120),{value.initial.kind,value.initial.number,
                    read(value.initial.reference,128)}};
                if (!idValid(property.id) || property.label.empty() || !propertyIds.insert(property.id).second
                    || !property.initial.valid() || value.initial.reference.size != 0) {
                    error = "Invalid schema for native component " + type.id + ". Reference defaults must be empty.";
                    return false;
                }
                type.properties.push_back(std::move(property));
            }
            candidate->types.push_back(std::move(type));
        }
    }
    if (instances.size() > 256) {
        error = "A scene supports up to 256 native component instances.";
        return false;
    }
    for (const auto &instance : instances) {
        const auto type = std::ranges::find(candidate->types,instance.type,&NativeComponentType::id);
        if (type == candidate->types.end()) {
            error = "Missing native component type " + instance.type + " on " + instance.owner + ".";
            return false;
        }
        auto resolved = instance;
        for (const auto &property : type->properties) {
            const auto [field,inserted] = resolved.properties.try_emplace(property.id,property.initial);
            if (!inserted && (!field->second.valid() || field->second.kind != property.initial.kind)) {
                error = "Incompatible property " + property.id + " on native component " + instance.type + ".";
                return false;
            }
        }
        if (resolved.properties.size() != type->properties.size()) {
            error = "An authored property is missing from native component " + instance.type + ".";
            return false;
        }
        candidate->components.push_back({std::move(resolved),&extension->types[type-candidate->types.begin()]});
    }
    pending_ = std::move(candidate);
    error.clear();
    return true;
#else
    (void)path;
    (void)instances;
    error = "Native gameplay modules are supported on Windows.";
    return false;
#endif
}

void NativeGameplay::tick(WorldSimulation &world,std::span<const NativeBodyBinding> bodies,
                          std::optional<std::size_t> player,std::uint32_t input,Log log)
{
    if (!loaded())
        return;
    Context context{world,bodies,player,log};
    const WfGameFrameV1 frame{sizeof(WfGameFrameV1),input,steps_,WorldSimulation::stepSeconds};
    if (pending_) {
        if (started_) {
            active_->invoke(context,frame,&WfGameComponentV1::stop);
            if (active_->api->stop)
                active_->api->stop(&host,&context,&frame);
        }
        active_ = std::move(pending_);
        started_ = false;
    }
    if (!started_) {
        if (active_->api->start)
            active_->api->start(&host,&context,&frame);
        active_->invoke(context,frame,&WfGameComponentV1::start);
        started_ = true;
    }
    active_->api->fixed_update(&host,&context,&frame);
    active_->invoke(context,frame,&WfGameComponentV1::fixed_update);
    ++steps_;
}

void NativeGameplay::stop(WorldSimulation &world,std::span<const NativeBodyBinding> bodies,
                          std::optional<std::size_t> player,Log log)
{
    if (started_ && active_) {
        Context context{world,bodies,player,log};
        const WfGameFrameV1 frame{sizeof(WfGameFrameV1),0,steps_,0};
        active_->invoke(context,frame,&WfGameComponentV1::stop);
        if (active_->api->stop)
            active_->api->stop(&host,&context,&frame);
    }
    active_.reset();
    pending_.reset();
    started_ = false;
    steps_ = 0;
}

}
