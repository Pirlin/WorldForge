#include "Engine/World/WorldSimulation.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace worldforge {

std::optional<float> Heightfield::height(float x, float y) const
{
    const float u = (x/size + 0.5f)*(side-1), v = (y/size + 0.5f)*(side-1);
    if (!(u >= 0 && u <= side-1 && v >= 0 && v <= side-1))
        return {};
    const int i = std::min(int(u), side-2), j = std::min(int(v), side-2);
    const float a = u-i, b = v-j;
    const auto index = j*side+i;
    const float h00 = samples[index], h10 = samples[index+1];
    const float h01 = samples[index+side], h11 = samples[index+side+1];
    return b <= a ? h00 + a*(h10-h00) + b*(h11-h10)
                  : h00 + b*(h01-h00) + a*(h11-h01);
}

bool Heightfield::sculpt(float x, float y, float radius, float amount, bool smooth)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius) || radius <= 0 || !std::isfinite(amount))
        return false;
    const auto previous = samples;
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            const float dx = size*(float(i)/(side-1)-0.5f)-x;
            const float dy = size*(float(j)/(side-1)-0.5f)-y;
            const float distance = std::hypot(dx, dy);
            if (distance >= radius)
                continue;
            const float influence = 1-distance/radius;
            const auto index = j*side+i;
            float value = previous[index] + amount*influence*influence;
            if (smooth) {
                float sum = 0;
                int count = 0;
                for (int row = std::max(0,j-1); row <= std::min(j+1,side-1); ++row)
                    for (int col = std::max(0,i-1); col <= std::min(i+1,side-1); ++col) {
                        sum += previous[row*side+col];
                        ++count;
                    }
                value = std::lerp(previous[index], sum/count, std::clamp(std::abs(amount)*influence, 0.0f, 1.0f));
            }
            samples[index] = std::clamp(value, -256.0f, 256.0f);
        }
    }
    return samples != previous;
}

Mesh Heightfield::mesh() const { return region(0,0,side-1); }

bool Heightfield::paint(float x,float y,float radius,std::uint8_t layer)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius) || radius <= 0 || layer > 3)
        return false;
    bool changed = false;
    for (int row = 0; row < side; ++row)
        for (int col = 0; col < side; ++col) {
            const float dx = size*(float(col)/(side-1)-0.5f)-x;
            const float dy = size*(float(row)/(side-1)-0.5f)-y;
            auto &value = layers[row*side+col];
            if (value != layer && dx*dx+dy*dy < radius*radius) {
                value = layer;
                changed = true;
            }
        }
    return changed;
}

Point3 Heightfield::surfaceColor(Point3 position,Point3 normal) const
{
    const int x = std::clamp(int(std::lround((position[0]/size+0.5f)*(side-1))),0,side-1);
    const int y = std::clamp(int(std::lround((position[1]/size+0.5f)*(side-1))),0,side-1);
    const auto layer = layers[y*side+x];
    constexpr Point3 grass{0.25f,0.42f,0.16f}, rock{0.43f,0.40f,0.36f}, soil{0.38f,0.28f,0.16f};
    if (layer)
        return layer == 1 ? grass : layer == 2 ? rock : soil;
    const float slope = std::clamp((0.96f-normal[2])*5,0.0f,1.0f);
    const float variation = 0.5f+0.5f*std::sin(position[0]*0.17f)*std::cos(position[1]*0.21f);
    const float earth = std::clamp((variation-0.67f)*1.4f,0.0f,0.45f);
    Point3 color;
    for (int i = 0; i < 3; ++i)
        color[i] = std::lerp(std::lerp(grass[i],soil[i],earth),rock[i],slope);
    return color;
}

Mesh Heightfield::chunk(int x, int y) const
{
    constexpr int cells = (side-1)/chunks;
    return region(x*cells,y*cells,cells);
}

bool Heightfield::chunkInRange(int x, int y, float localX, float localY, float radius) const
{
    const float width = size/chunks;
    const float left = x*width-size/2, bottom = y*width-size/2;
    return std::hypot(localX-std::clamp(localX,left,left+width),
                      localY-std::clamp(localY,bottom,bottom+width)) <= radius;
}

Mesh Heightfield::foliage(int x, int y, int density, unsigned seed) const
{
    Mesh mesh;
    mesh.materials[0] = {"Bark",{0.33f,0.21f,0.12f,1},0,1};
    mesh.materials.push_back({"Needles",{0.20f,0.43f,0.24f,1},0,1});
    seed ^= (1u+x+chunks*y)*0x9e3779b9u;
    const auto random = [&seed] {
        seed = seed*1664525u+1013904223u;
        return float(seed >> 8)/16777216.0f;
    };
    const auto ring = [&](float px,float py,float z,float radius) {
        for (int i = 0; i < 5; ++i) {
            const float angle = float(i)*1.2566370614f;
            mesh.vertices.push_back({px+radius*std::cos(angle),py+radius*std::sin(angle),z});
        }
    };
    const float width = size/chunks;
    for (int n = 0; n < std::clamp(density,0,48); ++n) {
        const float px = (x+0.08f+0.84f*random())*width-size/2;
        const float py = (y+0.08f+0.84f*random())*width-size/2;
        const float h = (0.8f+random()*1.6f)*std::min(1.4f,width*0.22f);
        const float z = height(px,py).value_or(0)-0.03f;
        const auto base = VertexIndex(mesh.vertices.size());
        ring(px,py,z,0.065f*h);
        ring(px,py,z+0.57f*h,0.045f*h);
        ring(px,py,z+0.34f*h,0.29f*h);
        mesh.vertices.push_back({px,py,z+h});
        for (VertexIndex i = 0; i < 5; ++i) {
            const auto a = base+i, b = base+(i+1)%5;
            mesh.faces.push_back({{{a},{b},{b+5},{a+5}}});
            mesh.faces.push_back({{{a+10},{b+10},{base+15}},1});
        }
    }
    return mesh;
}

float Heightfield::foliageSway(const Mesh &mesh,VertexIndex vertex)
{
    constexpr VertexIndex verticesPerTree = 16;
    return mesh.vertices[vertex][2]-mesh.vertices[vertex-vertex%verticesPerTree][2];
}

Mesh Heightfield::region(int x, int y, int cells) const
{
    Mesh result;
    result.materials[0] = {"Terrain", {0.29f,0.48f,0.23f,1}, 0, 0.95f};
    const int stride = cells+1;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i)
            result.vertices.push_back({size*(float(x+i)/(side-1)-0.5f),
                                       size*(float(y+j)/(side-1)-0.5f),samples[(y+j)*side+x+i]});
    const auto corner = [this,x,y,stride](VertexIndex index) {
        const int i = x+int(index%stride), j = y+int(index/stride);
        const int left = std::max(0,i-1), right = std::min(side-1,i+1);
        const int low = std::max(0,j-1), high = std::min(side-1,j+1);
        const float spacing = size/(side-1);
        const float dx = (samples[j*side+right]-samples[j*side+left])/((right-left)*spacing);
        const float dy = (samples[high*side+i]-samples[low*side+i])/((high-low)*spacing);
        const float length = std::sqrt(dx*dx+dy*dy+1);
        return MeshCorner{index,{float(i)/(side-1),float(j)/(side-1)},{-dx/length,-dy/length,1/length}};
    };
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            const auto a = VertexIndex(j*stride+i), b = a+1, c = b+stride, d = a+stride;
            result.faces.push_back({{corner(a),corner(b),corner(c)}});
            result.faces.push_back({{corner(a),corner(c),corner(d)}});
        }
    return result;
}

namespace {
JPH::Vec3 vec(Point3 p) { return {p[0],p[1],p[2]}; }
Point3 point(JPH::Vec3 p) { return {p.GetX(),p.GetY(),p.GetZ()}; }
JPH::Quat quat(const std::array<float,4> &q) { return JPH::Quat(q[0],q[1],q[2],q[3]).Normalized(); }

void registerPhysics()
{
    struct Registration {
        Registration()
        {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory;
            JPH::RegisterTypes();
        }
        ~Registration()
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    };
    static const Registration registration;
};
}

struct WorldSimulation::Backend final : JPH::ContactListener {
    JPH::BroadPhaseLayerInterfaceTable layers{2,2};
    JPH::ObjectLayerPairFilterTable pairs{2};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broadPairs;
    JPH::TempAllocatorImpl scratch{16*1024*1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem system;
    std::vector<JPH::Body *> bodies;
    std::vector<JPH::BodyID> groundBodies;
    std::vector<BoxBody> previous;
    std::vector<bool> grounded;
    std::set<std::pair<std::uint64_t,std::uint64_t>> connected;

    explicit Backend(const WorldSimulation &world) : groundBodies(world.terrains.size()),
        previous(world.bodies), grounded(world.bodies.size())
    {
        layers.MapObjectToBroadPhaseLayer(0,JPH::BroadPhaseLayer(0));
        layers.MapObjectToBroadPhaseLayer(1,JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(0,1);
        pairs.EnableCollision(1,1);
        broadPairs = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(layers,2,pairs,2);
        const auto capacity = JPH::uint(world.bodies.size()+world.terrains.size()+32);
        system.Init(capacity,0,16384,8192,layers,*broadPairs,pairs);
        system.SetContactListener(this);
        auto settings = system.GetPhysicsSettings();
        settings.mPenetrationSlop = 0.0005f;
        system.SetPhysicsSettings(settings);
        auto &api = system.GetBodyInterface();

        for (const auto &box : world.bodies) {
            const auto half = vec(box.halfSize);
            const float radius = std::min(0.02f,0.25f*half.ReduceMin());
            JPH::RefConst<JPH::Shape> shape;
            if (box.shape == BoxBody::Shape::Sphere)
                shape = new JPH::SphereShape(half.ReduceMin());
            else if (box.shape == BoxBody::Shape::Capsule) {
                const float capsuleRadius = std::min({half.GetX(),half.GetY(),half.GetZ()});
                const float segment = half.GetZ()-capsuleRadius;
                if (segment <= 0.001f)
                    shape = new JPH::SphereShape(capsuleRadius);
                else {
                    const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(segment,capsuleRadius);
                    shape = new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(),
                        JPH::Quat::sRotation(JPH::Vec3::sAxisX(),1.570796327f),capsule.GetPtr());
                }
            } else
                shape = new JPH::BoxShape(half,radius);
            JPH::BodyCreationSettings body(shape.GetPtr(),vec(box.position),quat(box.rotation),
                                          box.dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                          box.dynamic ? 1 : 0);
            body.mLinearVelocity = vec(box.velocity);
            body.mAngularVelocity = vec(box.angularVelocity);
            body.mMotionQuality = JPH::EMotionQuality::LinearCast;
            body.mRestitution = box.bounce;
            body.mFriction = box.friction;
            if (box.dynamic && box.mass > 0) {
                body.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                body.mMassPropertiesOverride.mMass = box.mass;
            }
            body.mAllowSleeping = false;
            body.mUserData = bodies.size()+1;
            if (box.lockRotation)
                body.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY
                                  | JPH::EAllowedDOFs::TranslationZ;
            auto *instance = api.CreateBody(body);
            bodies.push_back(instance);
            api.AddBody(instance->GetID(),box.dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        }
        for (const auto &joint : world.joints) {
            if (joint.first >= bodies.size() || joint.second >= bodies.size() || joint.first == joint.second
                || (!world.bodies[joint.first].dynamic && !world.bodies[joint.second].dynamic))
                continue;
            if (!connected.emplace(std::min(joint.first,joint.second)+1,std::max(joint.first,joint.second)+1).second)
                continue;
            auto &a = *bodies[joint.first], &b = *bodies[joint.second];
            if (joint.type == Joint::Type::Hinge) {
                JPH::HingeConstraintSettings hinge;
                hinge.mPoint1 = hinge.mPoint2 = vec(joint.anchor);
                hinge.mHingeAxis1 = hinge.mHingeAxis2 = vec(joint.axis).NormalizedOr(JPH::Vec3::sAxisZ());
                hinge.mNormalAxis1 = hinge.mNormalAxis2 = hinge.mHingeAxis1.GetNormalizedPerpendicular();
                system.AddConstraint(hinge.Create(a,b));
            } else if (joint.type == Joint::Type::Distance) {
                JPH::DistanceConstraintSettings distance;
                distance.mPoint1 = a.GetPosition();
                distance.mPoint2 = b.GetPosition();
                system.AddConstraint(distance.Create(a,b));
            } else {
                JPH::FixedConstraintSettings fixed;
                fixed.mAutoDetectPoint = true;
                system.AddConstraint(fixed.Create(a,b));
            }
        }
        system.OptimizeBroadPhase();
    }

    void streamTerrain(const WorldSimulation &world)
    {
        auto &api = system.GetBodyInterface();
        for (std::size_t i = 0; i < groundBodies.size(); ++i) {
            const auto &ground = world.terrains[i];
            auto &id = groundBodies[i];
            const float half = ground.field.size*0.5f;
            const float radius = id.IsInvalid() ? 120.0f : 144.0f;
            const bool needed = std::ranges::any_of(world.bodies,[&](const BoxBody &body) {
                if (!body.dynamic)
                    return false;
                const float dx = std::max(0.0f,std::abs(body.position[0]-ground.position[0])-half);
                const float dy = std::max(0.0f,std::abs(body.position[1]-ground.position[1])-half);
                return std::hypot(dx,dy) <= radius+vec(body.halfSize).Length();
            });
            if (!needed && !id.IsInvalid()) {
                api.RemoveBody(id);
                api.DestroyBody(id);
                id = {};
            }
            if (!needed || !id.IsInvalid())
                continue;
            // Keep a tile in one collision shape so internal edges remain smooth across render chunk boundaries.
            const auto mesh = ground.field.mesh();
            JPH::VertexList vertices;
            for (const auto &p : mesh.vertices)
                vertices.emplace_back(p[0],p[1],p[2]);
            JPH::IndexedTriangleList triangles;
            for (const auto &face : mesh.faces)
                triangles.emplace_back(face.corners[0].vertex,face.corners[1].vertex,face.corners[2].vertex);
            JPH::MeshShapeSettings shape(std::move(vertices),std::move(triangles));
            const auto result = shape.Create();
            if (!result.HasError())
                id = api.CreateAndAddBody({result.Get().GetPtr(),vec(ground.position),JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Static,0},JPH::EActivation::DontActivate);
        }
    }

    void markGrounded(const JPH::Body &a,const JPH::Body &b,const JPH::ContactManifold &contact)
    {
        const float z = contact.mWorldSpaceNormal.GetZ();
        if (a.IsDynamic() && z < -0.5f)
            grounded[a.GetUserData()-1] = true;
        if (b.IsDynamic() && z > 0.5f)
            grounded[b.GetUserData()-1] = true;
    }

    JPH::ValidateResult OnContactValidate(const JPH::Body &a,const JPH::Body &b,JPH::RVec3Arg,
                                          const JPH::CollideShapeResult &) override
    {
        const auto first = a.GetUserData(), second = b.GetUserData();
        return connected.contains({std::min(first,second),std::max(first,second)})
            ? JPH::ValidateResult::RejectAllContactsForThisBodyPair : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body &a,const JPH::Body &b,const JPH::ContactManifold &contact,
                        JPH::ContactSettings &settings) override
    {
        settings.mCombinedRestitution = std::max(a.GetRestitution(),b.GetRestitution());
        markGrounded(a,b,contact);
    }

    void OnContactPersisted(const JPH::Body &a,const JPH::Body &b,const JPH::ContactManifold &contact,
                            JPH::ContactSettings &settings) override
    {
        OnContactAdded(a,b,contact,settings);
    }

    void step(WorldSimulation &world)
    {
        auto &api = system.GetBodyInterface();
        system.SetGravity({0,0,-world.gravity});
        streamTerrain(world);
        std::fill(grounded.begin(),grounded.end(),false);
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto &input = world.bodies[i], &last = previous[i];
            const auto id = bodies[i]->GetID();
            if (input.position != last.position || input.rotation != last.rotation) {
                api.SetPositionAndRotation(id,vec(input.position),quat(input.rotation),
                    input.dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
                if (!input.dynamic)
                    previous[i] = input;
            }
            if (!input.dynamic)
                continue;
            if (input.velocity != last.velocity || input.angularVelocity != last.angularVelocity)
                api.SetLinearAndAngularVelocity(id,vec(input.velocity),vec(input.angularVelocity));
        }
        system.Update(WorldSimulation::stepSeconds,1,&scratch,&jobs);
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            auto &output = world.bodies[i];
            if (!output.dynamic)
                continue;
            const auto id = bodies[i]->GetID();
            output.position = point(api.GetPosition(id));
            output.velocity = point(api.GetLinearVelocity(id));
            output.angularVelocity = point(api.GetAngularVelocity(id));
            const auto q = api.GetRotation(id);
            output.rotation = {q.GetX(),q.GetY(),q.GetZ(),q.GetW()};
            output.grounded = grounded[i] && output.velocity[2] < 0.5f;
            previous[i] = output;
        }
    }
};

WorldSimulation::WorldSimulation() = default;
WorldSimulation::~WorldSimulation() = default;
WorldSimulation::WorldSimulation(WorldSimulation &&) noexcept = default;
WorldSimulation &WorldSimulation::operator=(WorldSimulation &&) noexcept = default;

bool WorldSimulation::launchUpward(std::size_t body,float speed)
{
    if (body >= bodies.size() || !bodies[body].dynamic || !std::isfinite(speed) || speed <= 0)
        return false;
    bodies[body].velocity[2] = speed;
    bodies[body].grounded = false;
    return true;
}

void WorldSimulation::step()
{
    if (!backend_) {
        registerPhysics();
        backend_ = std::make_unique<Backend>(*this);
    }
    backend_->step(*this);
}

}
