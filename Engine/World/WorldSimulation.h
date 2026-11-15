#pragma once

#include "Engine/Geometry/Mesh.h"

#include <optional>
#include <memory>

namespace worldforge {

struct Heightfield {
    static constexpr int side = 33;
    static constexpr int chunks = 4;
    float size = 32;
    std::array<float, side*side> samples{};
    std::array<std::uint8_t, side*side> layers{};
    std::optional<float> height(float x, float y) const;
    bool sculpt(float x, float y, float radius, float amount, bool smooth);
    bool paint(float x, float y, float radius, std::uint8_t layer);
    Point3 surfaceColor(Point3 position, Point3 normal) const;
    Mesh mesh() const;
    Mesh chunk(int x, int y) const;
    Mesh foliage(int x, int y, int density, unsigned seed) const;
    static float foliageSway(const Mesh &mesh,VertexIndex vertex);
    bool chunkInRange(int x, int y, float localX, float localY, float radius) const;
private:
    Mesh region(int x, int y, int cells) const;
};

struct BoxBody {
    enum class Shape { Box, Sphere, Capsule };
    Point3 position{};
    Point3 halfSize{0.5f, 0.5f, 0.5f};
    Point3 velocity{};
    float bounce = 0;
    bool dynamic = false;
    bool grounded = false;
    std::array<float,4> rotation{0,0,0,1};
    Point3 angularVelocity{};
    bool lockRotation = false;
    float mass = 0;
    float friction = 0.55f;
    Shape shape = Shape::Box;
};

struct WorldSimulation {
    WorldSimulation();
    ~WorldSimulation();
    WorldSimulation(WorldSimulation &&) noexcept;
    WorldSimulation &operator=(WorldSimulation &&) noexcept;
    struct Ground { Heightfield field; Point3 position; };
    struct Joint {
        enum class Type { Fixed, Hinge, Distance };
        Type type;
        std::size_t first, second;
        Point3 anchor{}, axis{0,0,1};
    };
    std::vector<Ground> terrains;
    std::vector<BoxBody> bodies;
    std::vector<Joint> joints;
    float gravity = 9.81f;
    static constexpr float stepSeconds = 1.0f/120.0f;
    void step();
private:
    struct Backend;
    std::unique_ptr<Backend> backend_;
};

}
