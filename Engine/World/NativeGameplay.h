#pragma once

#include "Engine/World/WorldForgeNative.h"
#include "Engine/World/WorldForgeNativeComponents.h"
#include "Engine/World/WorldSimulation.h"

#include <filesystem>
#include <map>
#include <span>
#include <string_view>

namespace worldforge {

struct NativeBodyBinding {
    std::string id;
    std::size_t index;
};

struct NativePropertyValue {
    std::uint32_t kind = 0;
    double number = 0;
    std::string reference;
    bool valid() const;
    bool operator==(const NativePropertyValue &) const = default;
};

struct NativeInstance {
    std::string owner, type;
    std::map<std::string,NativePropertyValue,std::less<>> properties;
};

struct NativeProperty {
    std::string id, label;
    NativePropertyValue initial;
};

struct NativeComponentType {
    std::string id, label;
    std::vector<NativeProperty> properties;
};

class NativeGameplay {
public:
    using Log = void (*)(std::string_view) noexcept;
    NativeGameplay();
    ~NativeGameplay();
    NativeGameplay(NativeGameplay &&) noexcept;
    NativeGameplay &operator=(NativeGameplay &&) noexcept;

    // Stages the candidate. An invalid module leaves the previous module active.
    bool stage(const std::filesystem::path &path,std::string &error,std::span<const NativeInstance> instances = {});
    const std::vector<NativeComponentType> &componentTypes() const;
    bool loaded() const { return bool(active_) || bool(pending_); }
    void tick(WorldSimulation &world,std::span<const NativeBodyBinding> bodies,
              std::optional<std::size_t> player,std::uint32_t input,Log log);
    void stop(WorldSimulation &world,std::span<const NativeBodyBinding> bodies,
              std::optional<std::size_t> player,Log log);
    std::uint64_t steps() const { return steps_; }

private:
    struct Library;
    std::unique_ptr<Library> active_, pending_;
    std::uint64_t steps_ = 0;
    bool started_ = false;
};

}
