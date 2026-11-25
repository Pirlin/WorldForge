#include "Engine/Scene/SceneDocument.h"
#include "Engine/Geometry/Mesh.h"
#include "Engine/Geometry/MeshTopology.h"
#include "Engine/Geometry/MeshModifiers.h"
#include "Engine/Geometry/MeshRemesh.h"
#include "Engine/Geometry/SculptBrush.h"
#include "Engine/Geometry/MeshUv.h"
#include "Engine/World/WorldSimulation.h"
#include "Engine/World/PlayableScene.h"
#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/Flip.h"
#include "Engine/Render/SkyLight.h"
#include "Engine/Render/ShadowCascades.h"
#include "Engine/Render/SkyVisibility.h"
#include "Engine/Render/Atmosphere.h"
#include "Engine/Render/LightClusters.h"
#include "Tests/AgxReference.h"
#include <algorithm>
#include <sstream>
#include <fstream>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>

using namespace worldforge;

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void checkFlip()
{
    constexpr int width = 48, height = 32;
    const auto uniform = [](std::uint8_t r,std::uint8_t g,std::uint8_t b) {
        std::vector<std::uint8_t> image;
        for (int i = 0; i < width*height; ++i)
            image.insert(image.end(),{r,g,b});
        return image;
    };
    std::vector<std::uint8_t> pattern;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto value = std::uint8_t((x*37+y*11)%200+20);
            pattern.insert(pattern.end(),{value,std::uint8_t(255-value),std::uint8_t(value/2)});
        }
    require(flip::compare(pattern,pattern,width,height).mean < 1e-6, "FLIP finds no difference between identical images");
    const double extreme = flip::compare(uniform(0,255,0),uniform(0,0,255),width,height).mean;
    require(std::abs(extreme-1) < 1e-3, "FLIP rates green against blue as the largest difference");
    const double blackWhite = flip::compare(uniform(0,0,0),uniform(255,255,255),width,height).mean;
    require(blackWhite > 0.9 && blackWhite < 1, "FLIP rates black against white near the top of its range");
    auto shifted = pattern;
    for (auto &value : shifted)
        value = std::uint8_t(std::min(255,value+3));
    const double slight = flip::compare(pattern,shifted,width,height).mean;
    auto edged = pattern;
    for (int y = 10; y < 20; ++y)
        for (int x = 10; x < 30; ++x)
            for (int c = 0; c < 3; ++c)
                edged[std::size_t(y*width+x)*3+c] = 255;
    const double edge = flip::compare(pattern,edged,width,height).mean;
    require(slight > 0 && slight < 0.12 && edge > slight*2.5, "FLIP rates a slight shift low and a new bright shape high");
    require(flip::compare(pattern,pattern,width,height+1).error.empty(), "FLIP rejects images of the wrong size");
}

void checkColorPipeline()
{
    namespace color = worldforge::color;
    for (const float value : {0.0f,0.002f,0.04f,0.2f,0.5f,0.9f,1.0f})
        require(std::abs(color::linearToSrgb(color::srgbToLinear(value))-value) < 1e-5f, "sRGB decode and encode are inverses");
    require(std::abs(color::linearToSrgb(0.214041f)-0.5f) < 1e-4f, "Linear 0.214 encodes to sRGB 0.5");
    double greyWorst = 0, colorWorst = 0, colorTotal = 0;
    int colors = 0;
    for (const auto &sample : agxReference) {
        const auto display = color::agx({float(sample.input[0]),float(sample.input[1]),float(sample.input[2])});
        const bool grey = sample.input[0] == sample.input[1] && sample.input[1] == sample.input[2];
        for (int channel = 0; channel < 3; ++channel) {
            const double error = std::abs(double(display[channel])-sample.display[channel])*255;
            (grey ? greyWorst : colorWorst) = std::max(grey ? greyWorst : colorWorst,error);
            if (!grey) {
                colorTotal += error;
                ++colors;
            }
        }
    }
    if (greyWorst > 0.6 || colorWorst > 18 || colorTotal/colors > 1.5)
        std::cerr << "AgX error against Studio: grey " << greyWorst << ", color worst " << colorWorst
                  << ", color mean " << colorTotal/colors << " (8-bit levels)\n";
    require(greyWorst <= 0.6, "AgX greys match Studio within 0.6 of an 8-bit level");
    require(colorWorst <= 18 && colorTotal/colors <= 1.5, "AgX colors match Studio within the fitted tolerance");
    const auto agxGrey = color::agx({0.18f,0.18f,0.18f});
    const auto exposed = color::displayColor({0.09f,0.09f,0.09f},1,color::ViewTransform::AgX);
    require(std::abs(agxGrey[1]-exposed[1]) < 1e-5f, "One stop of exposure doubles scene light");
    require(std::abs(agxGrey[1]-0.4613f) < 0.002f, "AgX shows 18% grey like Studio does");
    const auto clipped = color::displayColor({4,4,4},0,color::ViewTransform::Standard);
    const auto rolled = color::displayColor({4,4,4},0,color::ViewTransform::AgX);
    require(clipped[0] > 0.9999f && rolled[0] < 0.95f, "Standard clips highlights and AgX rolls them off");
    require(std::abs(color::unitsFromLux(color::luxPerUnit)-1) < 1e-6f && std::abs(color::luxPerUnit-123532.6) < 1,
            "One scene unit is about 123,500 lux, a bright midday sun");
    require(color::stopsFromEv100(15) == 0 && color::ev100FromStops(-2) == 17, "EV100 15 is the reference exposure");
    // A histogram as the display shader fills it: bin 1+floor((log2 L - low) * 63 / (high - low)).
    const auto binOf = [](float luminance) {
        const float stops = std::log2(luminance);
        return 1+int(std::round((stops-color::meterLowStops)*(color::meterBins-2)/(color::meterHighStops-color::meterLowStops)));
    };
    std::array<std::uint32_t,color::meterBins> bins{};
    bins[std::size_t(binOf(0.18f))] = 1000;
    float average = 0;
    require(color::meteredLuminance(bins,average) && std::abs(color::autoExposureStops(average,0)) < 0.2f,
            "Auto exposure leaves a middle-grey scene at the reference exposure");
    require(std::abs(color::autoExposureStops(average,1)-color::autoExposureStops(average,0)-1) < 1e-4f,
            "Exposure compensation adds stops");
    bins[std::size_t(binOf(100))] = 50;
    bins[0] = 50;
    float withOutliers = 0;
    require(color::meteredLuminance(bins,withOutliers) && std::abs(withOutliers-average) < 0.01f,
            "Auto exposure ignores the brightest and darkest tenth of the view");
    std::array<std::uint32_t,color::meterBins> dim{};
    dim[std::size_t(binOf(0.18f/64))] = 1000;
    float dimAverage = 0;
    require(color::meteredLuminance(dim,dimAverage) && std::abs(color::autoExposureStops(dimAverage,0)-6) < 0.2f,
            "Auto exposure opens up six stops for a scene 64 times darker");
    require(!color::meteredLuminance(std::array<std::uint32_t,color::meterBins>{},average), "An empty histogram meters nothing");
    const float step = color::adaptExposure(0,4,0.1f), settled = color::adaptExposure(0,4,10);
    require(step > 0 && step < 1 && std::abs(settled-4) < 1e-3f, "Exposure adapts gradually and settles on the target");
    require(color::adaptExposure(0,-4,0.1f) < -step, "Eyes adapt to brightness faster than to darkness");
    // Sky light: under a uniform sky of radiance L with no sun, a white floor reflects L, the ground below the
    // horizon reflects groundAlbedo * L, and a wall sees half of each.
    const float grey = 0.5f, radiance = color::srgbToLinear(grey);
    const auto uniformSky = color::skyLight({grey,grey,grey},{grey,grey,grey},{0,0,1},0);
    const auto floorLight = color::skyIrradiance(uniformSky,0,0,1), wallLight = color::skyIrradiance(uniformSky,1,0,0);
    require(std::abs(uniformSky.ground[0]-color::groundAlbedo*radiance) < 0.01f*radiance, "The ground reflects a quarter of the sky");
    require(std::abs(floorLight[1]-radiance) < 0.06f*radiance, "A floor under a uniform sky reflects the sky's radiance");
    require(std::abs(wallLight[1]-0.5f*(1+color::groundAlbedo)*radiance) < 0.08f*radiance, "A wall sees half sky and half ground");
    // Where probes trace the real ground, the sky term uses the sky above the horizon alone: a floor still sees all of
    // it, a wall half the sky and no ground, a ceiling almost nothing.
    const auto above = [&](float x,float y,float z) {
        const auto basis = color::shBasis(x,y,z);
        float value = 0;
        for (std::size_t i = 0; i < 9; ++i)
            value += uniformSky.irradianceAbove[i][1]*basis[i];
        return value;
    };
    require(std::abs(above(0,0,1)-radiance) < 0.06f*radiance && std::abs(above(1,0,0)-0.5f*radiance) < 0.08f*radiance
            && above(0,0,-1) < 0.08f*radiance,
            "Sky irradiance above the horizon leaves out the ground for surfaces the probes light");
    const auto sunny = color::skyLight({grey,grey,grey},{grey,grey,grey},{0,0,1},1);
    require(sunny.ground[0] > uniformSky.ground[0]+0.2f && color::skyIrradiance(sunny,0,0,1)[0] > floorLight[0],
            "Sunlight brightens the ground and the light it bounces");
    // Prefiltered reflections of the uniform sky: straight up a smooth-ish surface sees the sky, straight down the
    // ground, and a rough surface facing the horizon sees a blend of the two.
    const auto reflection = [&](float z,float roughness) {
        const float d[3]{std::sqrt(1-z*z),0,z};
        return color::skyReflection(uniformSky,d,roughness)[1];
    };
    require(std::abs(reflection(0.99f,0.3f)-radiance) < 0.01f*radiance && std::abs(reflection(-0.99f,0.3f)-uniformSky.ground[1]) < 0.01f*radiance,
            "Prefiltered reflections show the sky above and the ground below");
    const float belowHorizon = reflection(-0.05f,1);
    require(belowHorizon > uniformSky.ground[1]*1.1f && belowHorizon < radiance*0.95f,
            "A rough reflection just below the horizon blends sky and ground");
    // Upward reflections leave out the part of the lobe under the surface, so they never see the ground.
    require(std::abs(reflection(0.05f,1)-radiance) < 0.02f*radiance, "A rough reflection just above the horizon sees only the sky");
    // The physical sky lights the scene with its own sunlight and blue sky.
    const auto physical = color::physicalSkyLight({0.3f,0.2f,0.93f},1);
    const auto physicalFloor = color::skyIrradiance(physical,0,0,1);
    require(physical.environment.size() == color::mapTexels && physicalFloor[2] > physicalFloor[0] && physical.sunLight[0] > physical.sunLight[2],
            "The physical sky gives blue sky light and warm sunlight");
    // The spherical harmonics match the irradiance integrated from the radiance map, also at twilight, when a bright
    // band along the horizon makes bands 0-2 alone overstate the light on the ground by 11%.
    {
        const float pi = 3.14159265f, below = -6*pi/180;
        const auto twilight = color::physicalSkyLight({std::cos(below),0,std::sin(below)},1);
        const auto exact = [&](const float n[3]) {
            double sum = 0;
            constexpr int width = 256, height = 128;
            for (int y = 0; y < height; ++y) {
                const double polar = (y+0.5)/height*pi, solidAngle = std::sin(polar)*(pi/height)*(2*pi/width);
                for (int x = 0; x < width; ++x) {
                    const double around = (x+0.5)/width*2*pi;
                    const float d[3]{float(std::sin(polar)*std::cos(around)),float(std::sin(polar)*std::sin(around)),
                                     float(std::cos(polar))};
                    const double cosine = n[0]*d[0]+n[1]*d[1]+n[2]*d[2];
                    if (cosine > 0)
                        sum += color::sky::sample(twilight.environment,0,color::mapWidth,color::mapHeight,d)[1]*cosine*solidAngle;
                }
            }
            return sum/pi;
        };
        double worst = 0;
        for (const auto &n : {std::array<float,3>{0,0,1},{1,0,0},{-1,0,0},{0,1,0},{0.7071f,0,0.7071f}}) {
            const double reference = exact(n.data());
            worst = std::max(worst,std::abs(color::skyIrradiance(twilight,n[0],n[1],n[2])[1]/reference-1));
        }
        require(worst < 0.06,"Sky irradiance stays within 6% of the integrated twilight sky in every direction");
    }
    require(color::skyAmbientScale({166/255.0f,197/255.0f,219/255.0f},{65/255.0f,119/255.0f,177/255.0f}) == 1,
            "The default sky keeps the ambient light unchanged");
    require(color::skyAmbientScale({0.03f,0.03f,0.06f},{0.01f,0.01f,0.03f}) < 0.01f, "A night sky gives almost no ambient light");
}
}

// A perspective camera's clip-to-world matrix (Vulkan depth 0 near to 1 far), column-major.
shadow::Matrix inverseCamera(shadow::Vector eye,shadow::Vector target,float verticalFov,float aspect,float nearPlane,
                             float farPlane)
{
    using namespace shadow::detail;
    const auto forward = normalized(subtract(target,eye));
    const auto right = normalized(cross(forward,{0,0,1}));
    const auto up = cross(right,forward);
    const float tanHalf = std::tan(verticalFov/2);
    const float k = (farPlane-nearPlane)/(farPlane*nearPlane);
    // world * w = eye * w + forward + right * x * tanHalf * aspect + up * y * tanHalf, with w = 1/near - z k.
    const auto column0 = scale(right,tanHalf*aspect), column1 = scale(up,tanHalf);
    const auto column3 = add(scale(eye,1/nearPlane),forward);
    return {column0[0],column0[1],column0[2],0, column1[0],column1[1],column1[2],0,
            -eye[0]*k,-eye[1]*k,-eye[2]*k,-k, column3[0],column3[1],column3[2],1/nearPlane};
}

void checkShadowCascades()
{
    const shadow::Vector eye{0,-30,1.7f}, target{0,0,1.5f}, towardSun{-0.66f,0.71f,0.24f};
    const auto camera = inverseCamera(eye,target,0.785f,16.0f/9,0.05f,1800);
    const shadow::Bounds street{{-60,-60,0},{60,60,12}};
    const auto cascades = shadow::fit(camera,towardSun,street);
    for (int i = 1; i < shadow::cascadeCount; ++i)
        require(cascades.scales[i][0] > cascades.scales[i-1][0] && cascades.scales[i][2] > cascades.scales[i-1][2],
                "Shadow cascades grow coarser and reach farther in order");
    require(cascades.scales[0][0] < 0.02f, "The nearest shadow cascade is finer than 2 cm per texel");
    require(std::abs(cascades.scales[3][2]-90.0f) < 1.5f, "Shadow cascades end at the far side of the scene");
    // Every scene point the camera sees falls inside some cascade, depth included.
    for (float depth : {5.0f,15.0f,30.0f,50.0f,80.0f})
        for (float side : {-0.9f,0.0f,0.9f}) {
            const shadow::Vector point{side*depth*0.4f,eye[1]+depth,0.2f};
            bool held = false;
            for (int i = 0; i < shadow::cascadeCount && !held; ++i) {
                const auto p = shadow::project(cascades,i,point);
                held = p[0] > 0 && p[0] < 1 && p[1] > 0 && p[1] < 1 && p[2] > 0 && p[2] < 1;
            }
            require(held,"Every visible scene point falls inside a shadow cascade");
        }
    // Casters toward the sun stay in front of the receivers they shade.
    const shadow::Vector receiver{0,-25,0};
    const auto caster = shadow::detail::add(receiver,shadow::detail::scale(shadow::detail::normalized(towardSun),40));
    const auto lit = shadow::project(cascades,0,receiver), blocking = shadow::project(cascades,0,caster);
    require(blocking[2] > 0 && blocking[2] < lit[2] && std::abs(blocking[0]-lit[0]) < 1e-4f && std::abs(blocking[1]-lit[1]) < 1e-4f,
            "A shadow caster toward the sun lands on the receiver's texel and nearer the sun");
    // Moving the camera a fraction of a texel keeps each tile on the same texel grid.
    const auto moved = shadow::fit(inverseCamera({0.003f,-30,1.7f},{0.003f,0,1.5f},0.785f,16.0f/9,0.05f,1800),towardSun,street);
    for (int i = 0; i < shadow::cascadeCount; ++i) {
        const auto a = shadow::project(cascades,i,receiver), b = shadow::project(moved,i,receiver);
        const float texels = (a[0]-b[0])*float(shadow::tileSize);
        require(std::abs(texels-std::round(texels)) < 1e-2f, "Shadow cascades move in whole texels");
    }
}

void checkSkyVisibility()
{
    using namespace shadow::detail;
    const shadow::Bounds street{{-60,-60,0},{60,60,12}};
    const auto maps = sky::fit(street);
    require(maps.metersPerTexel > 0.2f && maps.metersPerTexel < 0.5f, "Sky visibility maps cover the street at about a third of a meter per texel");
    for (int i = 0; i < sky::directionCount; ++i) {
        const shadow::Vector toward{maps.directions[i][0],maps.directions[i][1],maps.directions[i][2]};
        require(std::abs(dot(toward,toward)-1) < 1e-4f && toward[2] >= sky::lowestElevation-1e-4f,
                "Sky visibility directions are unit vectors above the lowest elevation");
        const auto project = [&](shadow::Vector point) {
            const auto &m = maps.matrices[i];
            shadow::Vector result;
            for (int row = 0; row < 3; ++row)
                result[row] = m[row]*point[0]+m[4+row]*point[1]+m[8+row]*point[2]+m[12+row];
            return result;
        };
        const auto center = project({0,0,6});
        require(std::abs(center[0]) < 1e-4f && std::abs(center[1]) < 1e-4f && std::abs(center[2]-0.5f) < 1e-4f,
                "The scene center lands in the middle of each sky visibility map");
        // A roof above a floor point, toward the sky, is nearer the sky in that direction's map.
        const shadow::Vector floor{5,5,0};
        const auto roof = project(add(floor,scale(toward,4))), ground = project(floor);
        require(std::abs(roof[0]-ground[0]) < 1e-4f && std::abs(roof[1]-ground[1]) < 1e-4f && roof[2] < ground[2] && roof[2] > 0,
                "An occluder toward the sky covers the point beneath it in the sky visibility map");
    }
}

void checkAtmosphere()
{
    const auto lum = [](const color::Rgb &c) { return 0.2126f*c[0]+0.7152f*c[1]+0.0722f*c[2]; };
    const atmosphere::Sky noon({0.3f,0.2f,0.93f},1), evening({-0.66f,0.71f,0.12f},1);
    const float up[3]{0,0,1}, horizon[3]{1,0,0.02f}, away[3]{0.3f,0.95f,0.05f};
    const auto zenith = noon.radiance(up), side = noon.radiance(horizon);
    // Prints the sky values when WORLDFORGE_PRINT_ATMOSPHERE is set, for tuning the atmosphere by hand.
    const bool printAtmosphere = [] {
#ifdef _MSC_VER
        char *value = nullptr;
        std::size_t length = 0;
        const bool set = _dupenv_s(&value,&length,"WORLDFORGE_PRINT_ATMOSPHERE") == 0 && value;
        std::free(value);
        return set;
#else
        return std::getenv("WORLDFORGE_PRINT_ATMOSPHERE") != nullptr;
#endif
    }();
    if (printAtmosphere) {
        const auto p = [](const char *name,const color::Rgb &c) { std::cout << name << ' ' << c[0] << ' ' << c[1] << ' ' << c[2] << '\n'; };
        p("noon zenith",zenith); p("noon horizon",side); p("noon sun",noon.sunLight());
        p("evening zenith",evening.radiance(up)); p("evening away",evening.radiance(away)); p("evening sun",evening.sunLight());
        const float down[3]{0,0.3f,-0.95f};
        p("noon ground",noon.radiance(down));
    }
    require(zenith[2] > zenith[1] && zenith[1] > zenith[0], "The physical sky is blue overhead");
    require(lum(side) > lum(zenith) && side[2]/side[0] < zenith[2]/zenith[0], "The physical sky is brighter and whiter at the horizon");
    require(std::abs(lum(atmosphere::Sky({0,0,1},1).sunLight())-1) < 0.01f, "An overhead sun gives the authored sunlight");
    const auto low = evening.sunLight(), high = noon.sunLight();
    require(lum(low) < lum(high) && low[0]/low[2] > 1.5f*high[0]/high[2], "A low sun is dimmer and redder");
    require(lum(zenith)/lum(high) > 0.05f && lum(zenith)/lum(high) < 0.5f, "The blue sky's radiance is a realistic fraction of sunlight");

    const float radius = atmosphere::bottomRadius+atmosphere::viewerHeight;
    const auto belowHorizon = atmosphere::Sky::transmittance(radius,-0.001f);
    const auto grazing = atmosphere::Sky::transmittance(radius,0);
    const auto aboveHorizon = atmosphere::Sky::transmittance(radius,0.005f);
    require(belowHorizon == color::Rgb{}, "The Earth blocks sunlight below the geometric horizon");
    require(grazing[0] > grazing[2] && grazing[2] > 0 && lum(grazing) < lum(aboveHorizon),
            "Grazing sunlight is reddened and attenuated without mixing with Earth-blocked rays");

    const float horizonAngle = std::asin(atmosphere::Sky::horizonCosine());
    constexpr float sunRadius = shadow::sunAngularDiameter*0.5f;
    float previousSun = 0;
    for (int i = -32; i <= 32; ++i) {
        const float elevation = horizonAngle+sunRadius*float(i)/24;
        const atmosphere::Sky sunrise({std::cos(elevation),0,std::sin(elevation)},1);
        const float direct = lum(sunrise.sunLight());
        require(direct >= previousSun-1e-6f && direct-previousSun < 0.02f,
                "Direct sunlight grows continuously as the solar disc rises across the horizon");
        if (i == 0)
            require(std::abs(direct-0.5f*sunrise.sunDiscStrength()) < 1e-6f,
                    "The Earth covers half the sun when the disc is centered on the horizon");
        previousSun = direct;
    }
    require(previousSun > 0.005f,"Sunlight reaches the ground once the solar disc clears the horizon");
}

void checkLightClusters()
{
    // Lamp ranges follow the exposure: an 1800 cd street lamp reaches tens of meters at night but only a few in the
    // sun's exposure, where its farther light cannot show; without an exposure the absolute limit applies.
    const float lamp = color::unitsFromCandela(1800);
    const float night = lights::range(lamp,color::exposureScale(10)), day = lights::range(lamp,color::exposureScale(-0.5f));
    require(lights::range(lamp) > 80 && std::abs(night-lights::range(lamp)) < 0.01f && day < 8 && day > lights::minimumRange,
            "Lamp ranges keep their night reach and shrink to where their light can show by day");
    // A camera at (0, -30, 1.7) looking along +y; Vulkan clip space (y down), 16:9, 45 degrees.
    const float f = 1/std::tan(0.3927f), aspect = 16.0f/9;
    const std::array<float,16> camera{f/aspect,0,0,0, 0,0,0.999f,1, 0,-f,0,0, 0,f*1.7f,29.9f,30};
    const std::vector<lights::Light> scene{
        {{2,0,3},0.02f},                                   // a street lamp 30 m ahead
        {{0,-45,2},1e-5f},                                 // a dim one behind the camera (range about 2 m)
        {{0.3f,0.2f,0.9f},0.5f,{1,1,1},true}};             // a directional light
    const auto clusters = lights::build(scene,camera,{0,-30,1.7f},{0,1,0});
    require(clusters.header[0] == 3 && clusters.header[1] == 1 && clusters.lights[1][3] == 1.0f,
            "Clustered lights list directional lights first");
    const auto listed = [&](uint32_t light) {
        int clustersHolding = 0;
        for (const auto &range : clusters.ranges)
            for (uint32_t k = 0; k < range[1]; ++k)
                clustersHolding += clusters.indices[range[0]+k] == light;
        return clustersHolding;
    };
    // The lamp's own pixel: project it and find its cluster.
    const float ndcX = (f/aspect*2)/30, ndcY = (-f*(3-1.7f))/30;
    const int tileX = int((ndcX*0.5f+0.5f)*lights::tilesX), tileY = int((ndcY*0.5f+0.5f)*lights::tilesY);
    const auto &own = clusters.ranges[std::size_t((lights::slice(30)*lights::tilesY+tileY)*lights::tilesX+tileX)];
    bool found = false;
    for (uint32_t k = 0; k < own[1]; ++k)
        found |= clusters.indices[own[0]+k] == 1;
    require(found, "A lamp is listed in the cluster of the pixel it lights");
    require(listed(1) > 0 && listed(2) == 0, "A lamp whose light ends behind the camera is listed in no cluster");
    require(std::abs(lights::range(0.02f)-100) < 1 && lights::range(1e-9f) == lights::minimumRange,
            "A lamp's range follows its intensity");
    // Point light shadows: the nearest on-screen lamp gets slot 0, and its cube faces see points in their own direction
    // at the center, with depth growing from the near plane to the light's range.
    require(clusters.header[2] == 1 && clusters.lights[5][3] == 2.0f && clusters.shadowed.size() == 1,
            "The nearest lamp on screen gets a point light shadow");
    const auto faces = lights::cubeFaces({2,0,3},20);
    const auto project = [](const std::array<float,16> &m,float x,float y,float z) {
        const float w = m[3]*x+m[7]*y+m[11]*z+m[15];
        return std::array<float,3>{(m[0]*x+m[4]*y+m[8]*z+m[12])/w,(m[1]*x+m[5]*y+m[9]*z+m[13])/w,
                                   (m[2]*x+m[6]*y+m[10]*z+m[14])/w};
    };
    const auto near = project(faces[0],3,0,3), far = project(faces[0],21.9f,0,3), below = project(faces[5],2,0,-2);
    require(std::abs(near[0]) < 1e-4f && std::abs(near[1]) < 1e-4f && near[2] > 0 && near[2] < far[2] && far[2] < 1
            && std::abs(below[0]) < 1e-4f && std::abs(below[1]) < 1e-4f,
            "Point light shadow faces look along their axes with depth from near to the light's range");

    const std::array<lights::Light,1> edgeLamp{{{{7,-16,4},0.00045f}}};
    const auto tight = lights::build(edgeLamp,camera,{0,-30,1.7f},{0,1,0});
    const float radius = lights::range(edgeLamp.front().intensity);
    int checked = 0;
    for (int ix = -4; ix <= 4; ++ix)
        for (int iy = -4; iy <= 4; ++iy)
            for (int iz = -4; iz <= 4; ++iz) {
                if (ix*ix+iy*iy+iz*iz >= 16)
                    continue;
                const float x = 7+float(ix)*radius/4, y = -16+float(iy)*radius/4, z = 4+float(iz)*radius/4;
                const float depth = y+30;
                if (depth <= 0.1f)
                    continue;
                const auto ndc = project(camera,x,y,z);
                if (std::abs(ndc[0]) >= 1 || std::abs(ndc[1]) >= 1)
                    continue;
                const int sampleTileX = int((ndc[0]*0.5f+0.5f)*lights::tilesX);
                const int sampleTileY = int((ndc[1]*0.5f+0.5f)*lights::tilesY);
                const auto &tile = tight.ranges[std::size_t((lights::slice(depth)*lights::tilesY+sampleTileY)*lights::tilesX+sampleTileX)];
                require(tile[1] == 1 && tight.indices[tile[0]] == 0,"A visible point within a lamp's reach keeps its light");
                ++checked;
            }
    // The sphere crosses the near plane, so the coarse projection touches every tile in the occupied depth slices.
    const auto coarseEntries = std::size_t(lights::tilesX*lights::tilesY*(lights::slice(14+radius)-lights::slice(14-radius)+1));
    require(checked > 30 && tight.indices.size() < coarseEntries,
            "Tight cluster footprints conservatively reject unused lamp entries");

    std::vector<lights::Light> lamps;
    for (int index = -4; index <= 4; ++index)
        lamps.push_back({{2.0f*float(index),0,4},0.02f});
    const auto firstSlots = lights::build(lamps,camera,{-3,-30,1.7f},{0,1,0});
    const auto secondSlots = lights::build(lamps,camera,{3,-30,1.7f},{0,1,0},firstSlots.shadowed);
    const auto dirtySlots = lights::shadowUpdates(secondSlots.shadowed,firstSlots.shadowed,false);
    require(firstSlots.shadowed.size() == lights::shadowSlots && secondSlots.shadowed.size() == lights::shadowSlots
        && std::ranges::count(dirtySlots,true) == 1,"Replacing one selected lamp preserves the seven other shadow slots");
}

int main(int argc,char *argv[])
{
    if (argc == 2) {
        PlayableScene sample;
        sample.horizon = {0.68f,0.77f,0.88f};
        sample.zenith = {0.23f,0.44f,0.73f};
        sample.fog = {0.71f,0.8f,0.9f};
        sample.fogDensity = 0.025f;
        sample.fogFalloff = 0.045f;
        sample.localFogCenter = {3,0,1};
        sample.localFogRadius = 6;
        sample.localFogDensity = 0.06f;
        sample.simulation.terrains.push_back({{},{}});
        sample.simulation.terrains.front().field.paint(2,0,4,2);
        sample.foliage.push_back({4,42});
        sample.simulation.bodies.push_back({{0,0,1},{0.3f,0.3f,0.85f},{},0,true});
        auto &avatar = sample.simulation.bodies.front();
        avatar.shape = BoxBody::Shape::Capsule;
        avatar.lockRotation = true;
        avatar.friction = 0.05f;
        std::ofstream output(argv[1]);
        return writePlayable(output,sample) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    checkColorPipeline();
    checkFlip();
    checkShadowCascades();
    checkSkyVisibility();
    checkAtmosphere();
    checkLightClusters();
    const auto example = Guid::parse("A1723291-8A22-47DC-B098-1F6718CFFAAC");
    require(example && example->toString() == "a1723291-8a22-47dc-b098-1f6718cffaac", "GUID canonicalizes");
    require(!Guid::parse("A1723291-8A22-47DC-B098-1F6718CFFAAX"), "Invalid GUID rejected");
    require(!Guid::parse("A17232918A22-47DC-B098-1F6718CFFAAC"), "Missing separator rejected");
    const auto generated = Guid::generate();
    const auto generatedText = generated.toString();
    require(!generated.isNull() && generatedText[14] == '4'
            && std::string_view("89ab").find(generatedText[19]) != std::string_view::npos,
            "Generated ID has version-4 layout");
    require(Guid::parse(generatedText) == generated, "Generated ID round trips");

    const auto project = Guid::generate();
    const auto scene = Guid::generate();
    const auto root = Guid::generate();
    const auto child = Guid::generate();
    auto document = std::get<SceneDocument>(SceneDocument::open({project, scene, 0, {}}));
    const auto command = [&](SceneEdit edit) {
        return SceneCommand{Guid::generate(), project, scene, document.revision(), std::move(edit)};
    };
    const auto checkRejected = [&](SceneEdit edit, SceneError expected, const char *message) {
        const auto old = document.snapshot();
        const auto result = document.apply(command(std::move(edit)));
        require(result.error == expected && !result.inverse && result.revision == old.revision
                && document.snapshot() == old, message);
    };

    const auto createRoot = command(AddEntity{{root, {}, "Root", {}}});
    const auto preview = document.preview(createRoot);
    require(preview.ok() && preview.revision == 1 && !document.find(root), "Preview does not change the scene");
    require(document.apply(createRoot).ok(), "Add root");
    require(document.apply(createRoot).error == SceneError::RevisionConflict, "Replay at stale revision rejected");
    require(document.apply(command(AddEntity{{child, root, "Cube", {}}})).ok(), "Add child");
    checkRejected(AddEntity{{child, {}, "Duplicate", {}}}, SceneError::DuplicateEntity, "Duplicate identity rejected");
    checkRejected(AddEntity{{Guid{}, {}, "Empty ID", {}}}, SceneError::InvalidIdentity, "Null identity rejected");
    checkRejected(ReparentEntity{root, child}, SceneError::HierarchyCycle, "Parent cycle rejected");
    checkRejected(ReparentEntity{child, Guid::generate()}, SceneError::MissingParent, "Dangling parent rejected");
    checkRejected(RemoveEntity{root}, SceneError::HasChildren, "Parent deletion preserves children");
    checkRejected(RemoveEntity{Guid::generate()}, SceneError::MissingEntity, "Missing target rejected");
    checkRejected(RenameEntity{child, " \t"}, SceneError::InvalidName, "Blank entity name rejected");

    Transform transform;
    transform.positionMeters = {5.0, 0.0, 1.0};
    transform.rotationDegrees = {0.0, 25.0, 0.0};
    transform.scale = {1.0, 2.0, 1.0};
    auto invalid = transform;
    invalid.positionMeters[0] = std::numeric_limits<double>::quiet_NaN();
    checkRejected(SetTransform{child, invalid}, SceneError::InvalidTransform, "NaN transform rejected");
    invalid = transform;
    invalid.scale[1] = 0;
    checkRejected(SetTransform{child, invalid}, SceneError::InvalidTransform, "Singular scale rejected");
    auto wrongScene = command(SetTransform{child, transform});
    wrongScene.scene = Guid::generate();
    require(document.apply(wrongScene).error == SceneError::WrongDocument, "Wrong scene rejected");
    auto futureEdit = command(SetTransform{child, transform});
    ++futureEdit.schemaVersion;
    require(document.apply(futureEdit).error == SceneError::UnsupportedSchema, "Unknown command schema rejected");

    auto change = document.apply(command(SetTransform{child, transform}));
    require(change.ok() && change.inverse && document.find(child)->transform == transform, "Set transform");
    auto undo = document.apply(command(*change.inverse));
    require(undo.ok() && undo.inverse && document.find(child)->transform == Transform{}, "Undo with inverse");
    require(document.apply(command(*undo.inverse)).ok() && document.find(child)->transform == transform, "Redo with inverse");
    require(document.apply(command(RenameEntity{child, "First Light Cube"})).ok(), "Rename preserves ID");

    const auto snapshot = document.snapshot();
    auto reopened = SceneDocument::open(snapshot);
    require(std::holds_alternative<SceneDocument>(reopened)
            && std::get<SceneDocument>(reopened).snapshot() == snapshot, "Snapshot reopen preserves the full scene");
    require(std::get<SceneDocument>(reopened).find(child)->parent == root, "Parent ID survives reopen");
    auto futureSnapshot = snapshot;
    ++futureSnapshot.schemaVersion;
    require(std::get<SceneError>(SceneDocument::open(futureSnapshot)) == SceneError::UnsupportedSchema,
            "Unknown scene schema rejected");
    const auto removed = document.apply(command(RemoveEntity{child}));
    require(removed.ok() && removed.inverse && !document.find(child), "Delete leaf");
    require(document.apply(command(*removed.inverse)).ok() && document.find(child)->transform == transform, "Undo restores deleted entity");

    auto cyclic = snapshot;
    for (auto &entity : cyclic.entities) {
        if (entity.id == root)
            entity.parent = child;
    }
    require(std::get<SceneError>(SceneDocument::open(cyclic)) == SceneError::HierarchyCycle, "Cyclic snapshot rejected");
    auto missingParent = snapshot;
    for (auto &entity : missingParent.entities) {
        if (entity.id == child)
            entity.parent = Guid::generate();
    }
    require(std::get<SceneError>(SceneDocument::open(missingParent)) == SceneError::MissingParent, "Dangling snapshot rejected");
    auto exhausted = snapshot;
    exhausted.revision = std::numeric_limits<Revision>::max();
    auto endOfRevisions = std::get<SceneDocument>(SceneDocument::open(exhausted));
    require(endOfRevisions.apply({Guid::generate(), project, scene, exhausted.revision, RemoveEntity{child}}).error
        == SceneError::RevisionExhausted, "Revision overflow rejected");
    const auto cube = makeCube();
    require(validateMesh(cube).empty() && cube.vertices.size() == 8 && cube.faces.size() == 6, "Valid editable cube");
    for (const auto kind : {Primitive::Cube,Primitive::Plane,Primitive::Sphere,Primitive::Cylinder,
                            Primitive::Cone,Primitive::Torus,Primitive::Circle}) {
        const auto primitive = makePrimitive(kind,{2,3,4},17,11);
        require(primitive && validateMesh(*primitive).empty(), "Parametric primitive validates");
        const bool flat = kind == Primitive::Plane || kind == Primitive::Circle;
        for (int axis = 0; axis < 3; ++axis) {
            float low = primitive->vertices.front()[axis], high = low;
            for (const auto &vertex : primitive->vertices) {
                low = std::min(low,vertex[axis]);
                high = std::max(high,vertex[axis]);
            }
            require(std::abs(high-low-(flat && axis == 2 ? 0.0f : float(axis+2))) < 0.0001f,
                    "Primitive has exact requested dimensions");
        }
        const MeshTopology topology(*primitive);
        require(!topology.edgeFaces.empty(), "Primitive has real polygon edges");
        for (const auto &[edge,faces] : topology.edgeFaces)
            require(faces.size() == (flat ? 1 : 2), "Primitive has the expected open or manifold topology");
    }
    require(!makePrimitive(Primitive::Sphere,{0,1,1}) && !makePrimitive(Primitive::Torus,{1,1,1},129),
            "Invalid primitive dimensions and excessive segments are rejected");
    auto disconnected = cube;
    disconnected.vertices.push_back({-0.49f,-0.5f,-0.5f});
    const auto falloff = connectedFalloff(disconnected,{0},1.5f);
    require(falloff[0] == 1 && falloff[1] > 0 && falloff[1] < 1 && falloff[2] == 0 && falloff.back() == 0,
            "Soft selection follows connected distance and ignores nearby disconnected vertices");
    const auto symmetry = mirroredVerticesX(cube,0.0001f);
    require(symmetry[0] == 1 && symmetry[1] == 0 && symmetry[7] == 6 && symmetry[6] == 7,
            "X symmetry matches corresponding vertices in both directions");
    auto extruded = cube;
    require(!extrudeFace(extruded, 99, 0.25f).empty() && extruded == cube, "Invalid face leaves geometry intact");
    require(extrudeFace(extruded, 1, 0.25f).empty() && extruded.vertices.size() == 12
        && extruded.faces.size() == 10 && extruded.vertices.back()[2] == 0.75f, "Face extrusion creates cap and sides");
    const std::array<std::size_t,2> adjacentFaces{1,2};
    auto region = cube, individual = cube;
    require(extrudeFaces(region,adjacentFaces,0.25f,false).empty()
        && region.vertices.size() == 14 && region.faces.size() == 12, "Region extrusion shares cap edges and emits only border walls");
    require(extrudeFaces(individual,adjacentFaces,0.25f,true).empty()
        && individual.vertices.size() == 16 && individual.faces.size() == 14, "Individual extrusion creates separate cap rims");
    const std::array<std::size_t,1> top{1};
    auto inset = cube;
    require(!insetFaces(inset,top,0.6f).empty() && inset == cube, "Oversized inset is rejected atomically");
    require(insetFaces(inset,top,0.1f).empty() && inset.faces.size() == 10
        && std::abs(inset.vertices[8][0]+0.4f) < 0.0001f && inset.vertices[8][2] == 0.5f,
        "Inset preserves the face plane and shrinks the cap");
    auto beveled = cube;
    require(!bevelFace(beveled, 1, 0.6f).empty() && beveled == cube, "Excess bevel rejected atomically");
    require(bevelFace(beveled, 1, 0.1f).empty() && beveled.faces.size() == 10
        && beveled.vertices[4][2] < 0.5f && beveled.vertices[8][2] == 0.5f, "Perimeter bevel creates a chamfer");
    auto cut = cube;
    require(cutQuadLoop(cut, 1, 0).empty() && cut.faces.size() == 10 && cut.vertices.size() == 12,
        "Quad loop cut splits a ring and reuses shared midpoints");
    auto multiple = cube;
    require(cutQuadLoop(multiple,1,0,2,0.25f).empty() && multiple.vertices.size() == 16 && multiple.faces.size() == 14,
        "Multiple loop cuts maintain a continuous shared ring");
    require(std::ranges::any_of(multiple.vertices,[](const auto &vertex) {
        return std::abs(vertex[0]-0.25f) < 0.0001f;
    }),"Loop slide positions cuts away from the midpoint");
    Mesh concave;
    concave.vertices = {{0,0,0},{2,0,0},{2,2,0},{1,1,0},{0,2,0}};
    concave.faces.push_back({{{0,{0,0}},{1,{1,0}},{2,{1,1}},{3,{0.5f,0.5f}},{4,{0,1}}}});
    const auto triangles = faceTriangles(concave,concave.faces.front());
    require(triangles.size() == 3 && makePlayVertices(concave).size() == 9, "Concave polygons triangulate for the Player");
    const std::array<std::size_t,1> firstFace{0};
    require(triangulateFaces(concave,firstFace).empty() && concave.faces.size() == 3
        && std::ranges::all_of(concave.faces,[](const auto &face) { return face.corners.size() == 3; }),
        "Authored triangulation preserves the three triangles");
    auto subdivided = cube;
    require(subdivideFaces(subdivided,top).empty() && subdivided.vertices.size() == 13
        && subdivided.faces.size() == 9 && subdivided.faces[2].corners.size() == 5,
        "Subdivision inserts shared midpoints in adjacent unselected faces");
    const MeshTopology subdividedTopology(subdivided);
    require(std::ranges::all_of(subdividedTopology.edgeFaces,[](const auto &entry) { return entry.second.size() == 2; }),
        "Subdivision creates no boundary or T-junction on a closed cube");
    auto open = cube;
    const std::array<MeshEdge,4> opening{{{4,5},{5,6},{6,7},{4,7}}};
    require(deleteFaces(open,top).empty() && open.faces.size() == 5
        && fillBoundary(open,opening).empty() && open.faces.size() == 6, "Delete and fill reopen and close a cube");
    Mesh caps = cube;
    caps.faces = {cube.faces[0],cube.faces[1]};
    const std::array<MeshEdge,8> capBorders{{{0,1},{1,2},{2,3},{0,3},{4,5},{5,6},{6,7},{4,7}}};
    require(bridgeBoundaries(caps,capBorders).empty() && caps.faces.size() == 6, "Bridge connects two opposing borders");
    auto weld = cube;
    const std::array<VertexIndex,2> pair{0,1};
    require(!weldVertices(weld,pair,0.1f).empty() && weld == cube, "Weld leaves geometry intact when no vertices qualify");
    require(weldVertices(weld,pair,0,1).empty() && weld.vertices.size() == 7,
        "Weld to Active collapses an edge while retaining faces with at least three corners");
    auto plane = *makePrimitive(Primitive::Plane,{1,1,1});
    require(triangulateFaces(plane,firstFace).empty(), "Plane triangulates");
    const std::array<MeshEdge,1> diagonal{{{0,2}}};
    require(dissolveEdges(plane,diagonal).empty() && plane.faces.size() == 1 && plane.faces.front().corners.size() == 4,
        "Dissolve restores a triangulated quad");
    auto shaded = cube;
    require(setFaceShading(shaded,adjacentFaces,true).empty() && shaded.faces[1].corners[0].normal != Point3{}
        && setFaceShading(shaded,adjacentFaces,false).empty() && shaded == cube, "Smooth and flat shading change stored corner normals");
    const std::array<MeshEdge,1> bevelSelection{{{4,5}}};
    auto edgeBevel = cube;
    require(bevelEdges(edgeBevel,bevelSelection,0.1f,3).empty()
        && edgeBevel.faces.size() == 9 && edgeBevel.vertices.size() == 14, "Edge bevel adds three profile strips");
    const MeshTopology bevelTopology(edgeBevel);
    require(std::ranges::all_of(bevelTopology.edgeFaces,[](const auto &entry) { return entry.second.size() == 2; }),
        "Edge bevel leaves a watertight cube");
    auto vertexBevel = cube;
    const std::array<VertexIndex,1> corner{0};
    require(bevelVertices(vertexBevel,corner,0.1f).empty() && vertexBevel.faces.size() == 7 && vertexBevel.vertices.size() == 10,
        "Vertex chamfer adds a cap and replaces its corner");
    auto slid = cube;
    require(slideVertex(slid,0,1,0.25f).empty() && slid.vertices[0][0] == -0.25f
        && slideEdge(slid,{4,5},-0.25f).empty() && slid.vertices[4] != cube.vertices[4], "Vertex and edge slides follow neighboring edges");
    auto split = cube;
    require(splitFaces(split,top).empty() && split.vertices.size() == 12 && split.faces.size() == 6,
        "Split duplicates a region boundary without changing its appearance");
    auto grid = cube;
    require(deleteFaces(grid,top).empty() && gridFillBoundary(grid,opening,1).empty()
        && grid.faces.size() == 6, "Grid fill makes a quad across a four-edge opening");
    auto knife = cube;
    require(knifeFace(knife,1,{{4,5},0.5f},{{6,7},0.5f}).empty()
        && knife.faces.size() == 7 && knife.vertices.size() == 10 && knife.faces[2].corners.size() == 5,
        "Knife splits a face and inserts matching vertices in neighbors");
    auto bisected = cube;
    auto pathMesh = cube;
    std::set<MeshEdge> knifeSeams{{4,5}};
    const std::array<MeshKnifeSegment,2> cutPath{{{1,{{6,7},0.5f},{{4,5},0.5f}},
                                                 {2,{{4,5},0.5f},{{0,1},0.5f}}}};
    require(knifePath(pathMesh,cutPath,&knifeSeams).empty() && pathMesh.vertices.size() == 11 && pathMesh.faces.size() == 8,
        "Two-face Knife path reuses one vertex on the shared edge");
    const MeshTopology pathTopology(pathMesh);
    require(knifeSeams.size() == 2 && !knifeSeams.contains({4,5})
        && std::ranges::all_of(knifeSeams,[&](auto edge) { return pathTopology.edgeFaces.contains(edge); }),
        "Knife preserves a marked UV seam as two connected segments");
    require(std::ranges::all_of(pathTopology.edgeFaces,[](const auto &entry) { return entry.second.size() == 2; }),
        "Multi-face Knife keeps the cut connected on a closed mesh");
    auto wrongPath = cutPath;
    wrongPath.back().start.fraction = 0.25f;
    auto rejectedPath = cube;
    require(!knifePath(rejectedPath,wrongPath).empty() && rejectedPath == cube,
        "A discontinuous Knife path is rejected without editing the mesh");
    auto angledCut = *makePrimitive(Primitive::Plane,{2,2,1});
    const std::array<MeshKnifeSegment,2> sameFaceCut{{
        {0,{{0,1},0.5f},{{1,2},0.5f}},
        {0,{{1,2},0.5f},{{2,3},0.5f}}
    }};
    require(knifePath(angledCut,sameFaceCut).empty() && angledCut.faces.size() == 3
        && angledCut.vertices.size() == 7 && MeshTopology(angledCut).edgeFaces.at({4,5}).size() == 2
        && MeshTopology(angledCut).edgeFaces.at({5,6}).size() == 2,
        "Multi-Cut can turn along multiple segments on one original face");
    auto beveledLoop = cube;
    require(bevelEdges(beveledLoop,opening,0.1f,3).empty() && beveledLoop.faces.size() == 18
        && beveledLoop.vertices.size() == 20, "Connected face perimeter bevel creates a three-band profile");
    const MeshTopology loopTopology(beveledLoop);
    require(std::ranges::all_of(loopTopology.edgeFaces,[](const auto &entry) { return entry.second.size() == 2; }),
        "Segmented perimeter bevel keeps the mesh watertight");
    require(bisectMesh(bisected,0,0).empty() && bisected.vertices.size() == 12 && bisected.faces.size() == 10,
        "Bisect cuts across all intersected faces");
    require(triangulateFaces(plane,firstFace).empty() && rotateEdge(plane,{0,2}).empty()
        && MeshTopology(plane).edgeFaces.contains({1,3}), "Rotate Edge flips a quad's diagonal");
    const auto spinProfile = makeRetopologyQuad({{{1,0,-1},{2,0,-1},{2,0,1},{1,0,1}}},{0,-1,0});
    require(bool(spinProfile),"Spin boundary fixture is valid");
    const std::array<MeshEdge,1> spinSide{{{1,2}}};
    auto partialSpin = *spinProfile;
    require(spinEdges(partialSpin,spinSide,2,{0,0,0},90,4).empty() && partialSpin.faces.size() == 5
        && partialSpin.vertices.size() == 12 && MeshTopology(partialSpin).edgeFaces.at({1,2}).size() == 2,
        "Partial Spin attaches four new quads to the source boundary");
    auto fullSpin = *spinProfile;
    require(spinEdges(fullSpin,spinSide,2,{0,0,0},-360,8).empty() && fullSpin.faces.size() == 9
        && fullSpin.vertices.size() == 20 && MeshTopology(fullSpin).edgeFaces.at({4,5}).size() == 2,
        "Full Spin closes the sweep seam with shared vertices");
    auto invalidSpin = cube;
    require(!spinEdges(invalidSpin,spinSide,2,{0,0,0},90,4).empty() && invalidSpin == cube,
        "Spin rejects interior edges without mutating the mesh");
    const std::array<Point2,4> latheProfile{{{0,-1},{1,-1},{1,1},{0,1}}};
    const std::array<Point2,3> arc{{{1,0},{1,1},{0,1}}};
    const std::array<float,3> weights{1,std::sqrt(0.5f),1};
    const auto rationalArc = sampleNurbsProfile(arc,weights,2,8);
    require(rationalArc && rationalArc->front() == arc.front() && rationalArc->back() == arc.back()
        && std::abs((*rationalArc)[4][0]-std::sqrt(0.5f)) < 0.0001f
        && std::abs((*rationalArc)[4][1]-std::sqrt(0.5f)) < 0.0001f,
        "A rational quadratic NURBS evaluates the midpoint of a circular arc");
    Mesh lathe;
    require(revolveProfile(lathe,latheProfile,2,360,16).empty()
        && lathe.vertices.size() == 34 && lathe.faces.size() == 48
        && std::ranges::all_of(MeshTopology(lathe).edgeFaces,[](const auto &edge) { return edge.second.size() == 2; })
        && faceNormal(lathe,lathe.faces.front())[2] < -0.9f,
        "A full revolve shares pole and seam vertices and keeps outward winding");
    const auto savedLathe = lathe;
    auto invalidProfile = latheProfile;
    invalidProfile[1][0] = -1;
    require(!revolveProfile(lathe,invalidProfile,2,360,16).empty() && lathe == savedLathe,
        "Invalid revolve input leaves the destination unchanged");
    const auto unitPlane = *makePrimitive(Primitive::Plane,{2,2,1});
    auto connectedVertices = unitPlane;
    require(connectVertices(connectedVertices,0,2).empty() && connectedVertices.vertices == unitPlane.vertices
        && MeshTopology(connectedVertices).edgeFaces.at({0,2}).size() == 2,
        "Connect vertices preserves the polygon corners and inserts a shared diagonal");
    auto connectedEdges = unitPlane;
    std::set<MeshEdge> connectSeams{{0,1}};
    require(connectEdges(connectedEdges,{0,1},{2,3},&connectSeams).empty()
        && connectedEdges.vertices.size() == 6 && connectedEdges.faces.size() == 2
        && connectSeams == std::set<MeshEdge>{{0,4},{1,4}},
        "Connect edge midpoints interpolates new corners and carries marked seams forward");
    auto weldedEdges = unitPlane;
    for (auto position : unitPlane.vertices) {
        position[0] += 3;
        weldedEdges.vertices.push_back(position);
    }
    auto second = unitPlane.faces.front();
    for (auto &secondCorner : second.corners)
        secondCorner.vertex += 4;
    weldedEdges.faces.push_back(second);
    require(weldBoundaryEdges(weldedEdges,{4,7},{1,2}).empty()
        && weldedEdges.vertices.size() == 6 && MeshTopology(weldedEdges).edgeFaces.at({1,2}).size() == 2,
        "Target Weld joins two boundary edges with opposite winding");
    auto patch = makeRetopologyQuad({{{0,0,0},{1,0,0},{1,1,0},{0,1,0}}},{0,0,1});
    require(patch && extendQuadBoundary(*patch,{1,2},{{{2,0,0},{2,1,0}}}).empty()
        && patch->faces.size() == 2 && MeshTopology(*patch).edgeFaces.at({1,2}).size() == 2,
        "Retopology extends a quad patch across a shared edge");
    auto subdivision = cube;
    require(applyMeshModifier(subdivision,{ModifierKind::Subdivision,true,0,1}).empty()
        && subdivision.vertices.size() == 26 && subdivision.faces.size() == 24
        && std::abs(subdivision.vertices[0][0]+0.2777778f) < 0.0001f,
        "Catmull-Clark subdivision smooths corners and shares new quad edges");
    require(applyMeshModifier(subdivision,{ModifierKind::Array,true,0,3,2}).empty()
        && subdivision.faces.size() == 72 && subdivision.vertices.size() == 78,
        "Array repeats an evaluated mesh without changing material slots");
    auto solid = *makePrimitive(Primitive::Plane,{1,1,1});
    require(applyMeshModifier(solid,{ModifierKind::Solidify,true,0,2,0.1f}).empty()
        && solid.faces.size() == 6 && solid.vertices.size() == 8,
        "Solidify gives a plane a back surface and four rim faces");
    const MeshTopology solidTopology(solid);
    require(std::ranges::all_of(solidTopology.edgeFaces,[](const auto &entry) { return entry.second.size() == 2; }),
        "Solidify closes the boundary");
    auto normalWeighted = cube;
    const auto wrapTarget = *makePrimitive(Primitive::Plane,{4,4,1});
    auto wrapped = *makePrimitive(Primitive::Plane,{1,1,1});
    for (auto &vertex : wrapped.vertices)
        vertex[2] = 1;
    require(applyMeshModifier(wrapped,{ModifierKind::Shrinkwrap,true,0,2,0.1f},&wrapTarget).empty()
        && std::ranges::all_of(wrapped.vertices,[](auto vertex) { return std::abs(vertex[2]-0.1f) < 0.0001f; }),
        "Shrinkwrap projects to nearest target triangles with a normal offset");
    require(applyMeshModifier(normalWeighted,{ModifierKind::WeightedNormal}).empty()
        && normalWeighted.faces.front().corners.front().normal != Point3{},
        "Weighted normal evaluation stores a new corner normal");
    auto charts = cube;
    require(!unwrapUvIslands(charts,{},0.02f).empty() && charts == cube,
        "A closed surface asks for seams and preserves prior UVs");
    std::set<MeshEdge> allSeams;
    const MeshTopology cubeTopology(cube);
    for (const auto &entry : cubeTopology.edgeFaces)
        allSeams.insert(entry.first);
    require(unwrapUvIslands(charts,allSeams,0.02f).empty() && charts.vertices == cube.vertices,
        "Unwrap opens cut islands without changing geometry");
    const auto chartReport = inspectUvs(charts,allSeams);
    require(chartReport.islands == 6 && !chartReport.outside && !chartReport.degenerate && !chartReport.overlaps,
        "Packed unwrap has six distinct islands inside the tile");
    auto anchored = charts;
    const UvPins pins{{0,0}};
    require(unwrapUvIslands(anchored,allSeams,0.02f,pins).empty() && anchored.faces[0] == charts.faces[0]
        && !inspectUvs(anchored,allSeams).overlaps,
        "Unwrap holds a pinned chart in place while packing the remaining charts");
    require(packUvIslands(anchored,allSeams,0.02f,pins).empty() && anchored.faces[0] == charts.faces[0],
        "Packing preserves pinned coordinates");
    std::set<MeshEdge> netSeams = allSeams;
    for (const auto &edge : {MeshEdge{4,5},MeshEdge{5,6},MeshEdge{6,7},MeshEdge{4,7},MeshEdge{0,1}})
        netSeams.erase(edge);
    require(unwrapUvIslands(charts,netSeams,0.01f).empty()
        && inspectUvs(charts,netSeams).islands == 1 && !inspectUvs(charts,netSeams).overlaps,
        "A connected cube net unwraps as one chart without overlap");
    auto conflictingPins = cube;
    require(!unwrapUvIslands(conflictingPins,netSeams,0.01f,{{0,3},{2,0}}).empty() && conflictingPins == cube,
        "A joined vertex with conflicting UV pins fails without moving either anchor");
    auto ring = *makePrimitive(Primitive::Plane,{4,3,1});
    const std::array<std::size_t,1> centerFace{0};
    require(insetFaces(ring,centerFace,0.75f).empty() && deleteFaces(ring,centerFace).empty(),
        "Build a planar UV chart around an opening");
    const auto ringPositions = ring.vertices;
    require(unwrapUvIslands(ring,{},0.02f).empty() && ring.vertices == ringPositions && ring.faces.size() == 4,
        "Unwrap a chart with two boundary loops while keeping the mesh opening");
    const auto ringReport = inspectUvs(ring,{});
    require(ringReport.islands == 1 && !ringReport.outside && !ringReport.degenerate && !ringReport.overlaps,
        "Packed UVs preserve a single nonoverlapping ring chart");
    auto overlapping = charts;
    for (auto &face : overlapping.faces)
        for (std::size_t i = 0; i < face.corners.size(); ++i)
            face.corners[i].uv = cube.faces.front().corners[i].uv;
    require(inspectUvs(overlapping,allSeams).overlaps > 0, "UV inspection detects coincident triangles");
    require(packUvIslands(overlapping,allSeams,0.01f).empty() && !inspectUvs(overlapping,allSeams).overlaps,
        "Packing separates overlapping chart rectangles");
    auto half = cube;
    for (auto &vertex : half.vertices)
        vertex[0] += 0.5f;
    require(mirrorMeshX(half).empty() && half.vertices.size() == 12 && half.faces.size() == 10,
        "Mirror welds the plane and removes the interior cap");
    auto asymmetric = cube;
    auto sculpt = cube;
    SculptStroke stroke(sculpt);
    require(stroke.dab(sculpt,{0.5f,0.5f,0.5f},{0,0,1},0.2f,0.5f,SculptBrush::Draw,true)
        && sculpt.vertices[6][2] > cube.vertices[6][2] && sculpt.vertices[7][2] == sculpt.vertices[6][2]
        && sculpt.vertices[2] == cube.vertices[2] && sculpt.faces.size() == cube.faces.size(),
        "A sculpt dab moves only nearby vertices and mirrors across local X");
    const auto raised = sculpt.vertices[6][2];
    require(stroke.dab(sculpt,{0.5f,0.5f,0.5f},{0,0,1},0.2f,0.5f,SculptBrush::Smooth,false)
        && sculpt.vertices[6][2] < raised && validateMesh(sculpt).empty(),"Sculpt smooth uses neighboring vertices");
    for (const auto &edges : {std::vector<MeshEdge>{{0,1},{1,2}},std::vector<MeshEdge>{{0,1},{1,2},{1,5}}}) {
        auto junction = cube;
        const auto bevelError = bevelEdges(junction,edges,0.12f,3);
        require(bevelError.empty(),bevelError.c_str());
        const MeshTopology closed(junction);
        require(junction.faces.size() > cube.faces.size()+edges.size()
            && std::ranges::all_of(closed.edgeFaces,[](const auto &edge) { return edge.second.size() == 2; })
            && std::ranges::all_of(junction.faces,[&](const auto &face) { return !faceTriangles(junction,face).empty(); }),
            "Adjoining bevels close two-edge runs and three-edge junctions with renderable faces");
    }
    auto allBeveled = cube;
    std::vector<MeshEdge> boxEdges(allSeams.begin(),allSeams.end());
    const auto boxBevelError = bevelEdges(allBeveled,boxEdges,0.12f,3);
    require(boxBevelError.empty(),boxBevelError.c_str());
    require(std::ranges::all_of(MeshTopology(allBeveled).edgeFaces,[](const auto &edge) { return edge.second.size() == 2; }),
        "Beveling all box edges preserves watertight junctions");
    auto oversizedBevel = cube;
    require(!bevelEdges(oversizedBevel,boxEdges,0.8f,3).empty() && oversizedBevel == cube,
        "An oversized adjoining bevel preserves the original mesh");
    for (auto &vertex : asymmetric.vertices)
        if (vertex[0] < 0)
            vertex[0] = -2;
    asymmetric.materials.push_back({"Negative side"});
    asymmetric.faces[5].material = 1;
    auto positiveMirror = asymmetric, negativeMirror = asymmetric;
    require(mirrorMeshX(positiveMirror).empty() && mirrorMeshX(negativeMirror,MirrorSide::Negative).empty(),
        "Crossing Mirror bisects a full mesh from either source side");
    const MeshTopology mirroredTopology(negativeMirror);
    require(positiveMirror.faces.size() == 10 && negativeMirror.faces.size() == 10
        && std::ranges::all_of(mirroredTopology.edgeFaces,[](const auto &edge) { return edge.second.size() == 2; })
        && std::ranges::count(positiveMirror.faces,1u,&MeshFace::material) == 0
        && std::ranges::count(negativeMirror.faces,1u,&MeshFace::material) == 2
        && std::ranges::all_of(positiveMirror.vertices,[](const auto &p) { return std::abs(p[0]) <= 0.5f; })
        && std::ranges::count_if(negativeMirror.vertices,[](const auto &p) { return std::abs(p[0]) == 2; }) == 8,
        "Crossing Mirror preserves selected-side materials and welds the new seam");
    Mesh concaveMirror;
    concaveMirror.vertices = {{-2,-2,0},{2,-2,0},{2,-1,0},{-1,-1,0},{-1,1,0},{2,1,0},{2,2,0},{-2,2,0}};
    concaveMirror.faces.push_back({{{0},{1},{2},{3},{4},{5},{6},{7}}});
    require(mirrorMeshX(concaveMirror,MirrorSide::Positive).empty()
        && std::ranges::all_of(concaveMirror.vertices,[](const auto &v) { return std::abs(v[1]) >= 1; }),
        "Mirror triangulates a concave face with multiple cut intervals and preserves its gap");
    auto cutter = cube;
    cutter.materials.front().name = "Cutter finish";
    const auto solidVolume = [](const Mesh &mesh) {
        double volume = 0;
        for (const auto &face : mesh.faces) {
            const auto p = mesh.vertices[face.corners[0].vertex];
            const auto q = mesh.vertices[face.corners[1].vertex];
            const auto r = mesh.vertices[face.corners[2].vertex];
            volume += p[0]*(q[1]*r[2]-q[2]*r[1])+p[1]*(q[2]*r[0]-q[0]*r[2])+p[2]*(q[0]*r[1]-q[1]*r[0]);
        }
        return volume/6;
    };
    for (const auto &[mode,volume] : {std::pair{BooleanOperation::Difference,0.5},
             std::pair{BooleanOperation::Union,1.5},std::pair{BooleanOperation::Intersection,0.5}}) {
        auto result = cube;
        const auto booleanError = booleanMesh(result,cutter,mode,{0.5f,0,0});
        require(booleanError.empty(),booleanError.c_str());
        const MeshTopology topology(result);
        require(std::abs(solidVolume(result)-volume) < 0.0001
            && std::ranges::all_of(topology.edgeFaces,[](const auto &edge) { return edge.second.size() == 2; })
            && std::ranges::any_of(result.faces,[](const auto &face) { return face.material == 1; }),
            "Boolean modes produce a closed solid of the expected volume and retain cutter materials");
    }
    auto failedBoolean = cube;
    require(!booleanMesh(failedBoolean,cutter,BooleanOperation::Intersection,{2,0,0}).empty() && failedBoolean == cube
        && !booleanMesh(failedBoolean,*makePrimitive(Primitive::Plane,{1,1,1}),BooleanOperation::Union).empty()
        && failedBoolean == cube,"Invalid and empty Booleans preserve the source mesh");
    auto remeshed = cube;
    const auto remeshError = remeshVoxels(remeshed,0.17f);
    require(remeshError.empty(),remeshError.c_str());
    const MeshTopology remeshTopology(remeshed);
    require(remeshed.faces.size() > cube.faces.size() && std::abs(solidVolume(remeshed)-1) < 0.12
        && std::ranges::all_of(remeshTopology.edgeFaces,[](const auto &edge) { return edge.second.size() == 2; }),
        "Voxel Remesh creates a closed sampled surface near the original size");
    auto cancelled = cube;
    std::stop_source stop;
    stop.request_stop();
    require(!remeshVoxels(cancelled,0.17f,stop.get_token()).empty() && cancelled == cube,
        "Cancelling voxel reconstruction preserves the source mesh");
    auto remeshLimit = cube;
    auto openRemesh = *makePrimitive(Primitive::Plane,{1,1,1});
    const auto openSource = openRemesh;
    require(!remeshVoxels(remeshLimit,0.0001f).empty() && remeshLimit == cube
        && !remeshVoxels(openRemesh,0.17f).empty() && openRemesh == openSource,
        "Voxel Remesh preserves the source when the preview exceeds the grid limit or the source is open");
    auto uv = cube;
    require(unwrapFace(uv, 1).empty() && uv.faces[1].corners[2].uv == Point2{1, 1},
        "Planar unwrap produces unit-square UVs");
    require(translateFaceUV(uv, 1, {0.25f, -0.125f}).empty()
        && uv.faces[1].corners[2].uv == Point2{1.25f, 0.875f}
        && uv.faces[0] == cube.faces[0] && uv.vertices == cube.vertices, "UV translation only changes the target face");
    Heightfield terrain;
    require(terrain.sculpt(0,0,3,2,false) && terrain.height(0,0) == 2 && !terrain.height(99,99),
            "Heightfield sculpt and height sampling");
    require(terrain.sculpt(0,0,3,1,true) && terrain.height(0,0).value() < 2, "Terrain smoothing");
    const auto automatic = terrain.surfaceColor({0,0,0},{0,0,1});
    require(terrain.paint(0,0,3,2) && terrain.layers[16*Heightfield::side+16] == 2
        && terrain.surfaceColor({0,0,0},{0,0,1}) != automatic, "Painted terrain layer changes its surface color");
    require(validateMesh(terrain.mesh()).empty(), "Terrain produces a valid render mesh");
    const auto firstChunk = terrain.chunk(0,0), nextChunk = terrain.chunk(1,0);
    constexpr int stride = (Heightfield::side-1)/Heightfield::chunks+1;
    for (int row = 0; row < stride; ++row)
        require(firstChunk.vertices[row*stride+stride-1] == nextChunk.vertices[row*stride],
                "Adjacent streamed chunks share identical edge vertices");
    require(terrain.chunkInRange(0,0,-14,-14,1) && !terrain.chunkInRange(3,3,-14,-14,1),
            "Terrain chunks load according to the streaming focus");
    const auto trees = terrain.foliage(0,0,8,42);
    require(!trees.faces.empty() && trees == terrain.foliage(0,0,8,42) && trees != terrain.foliage(0,0,8,43),
            "Foliage placement is deterministic and varies with the seed");
    WorldSimulation world;
    world.terrains.push_back({{},{}});
    world.bodies.push_back({{0,0,5},{0.5f,0.5f,0.5f},{},0,true});
    for (int i = 0; i < 360; ++i)
        world.step();
    require(world.bodies[0].grounded && std::abs(world.bodies[0].position[2]-0.5f) < 0.002f, "Box lands on terrain");
    for (const auto shape : {BoxBody::Shape::Sphere,BoxBody::Shape::Capsule}) {
        WorldSimulation curved;
        curved.terrains.push_back({{},{}});
        curved.bodies.push_back({{0,0,3},{0.30f,0.30f,0.85f},{},0,true});
        curved.bodies.front().shape = shape;
        curved.bodies.front().lockRotation = true;
        for (int i = 0; i < 220; ++i)
            curved.step();
        const float height = shape == BoxBody::Shape::Sphere ? 0.30f : 0.85f;
        require(curved.bodies.front().grounded && std::abs(curved.bodies.front().position[2]-height) < 0.004f,
                "Sphere and Z-axis capsule settle at their actual collider heights");
    }
    WorldSimulation platform;
    platform.bodies.push_back({{0,0,0},{4,4,0.01f},{},0,false});
    platform.bodies.push_back({{0,0,4},{0.1f,0.1f,0.1f},{0,0,-50},0,true});
    for (int i = 0; i < 60; ++i)
        platform.step();
    require(platform.bodies[1].grounded && std::abs(platform.bodies[1].position[2]-0.11f) < 0.002f,
            "Swept contact stops a fast box at a thin static platform");
    WorldSimulation streamed;
    streamed.terrains = {{{},{}},{{},{500,0,0}}};
    streamed.bodies.push_back({{0,0,2},{0.5f,0.5f,0.5f},{},0,true});
    streamed.step();
    streamed.bodies[0].position = {500,0,2};
    for (int i = 0; i < 180; ++i)
        streamed.step();
    require(streamed.bodies[0].grounded && std::abs(streamed.bodies[0].position[2]-0.5f) < 0.002f,
            "A distant terrain tile supplies collision after entering its streaming range");
    WorldSimulation spin;
    spin.gravity = 0;
    spin.bodies.push_back({{0,0,0},{0.5f,0.25f,0.5f},{},0,true});
    spin.bodies[0].angularVelocity = {0,0,3};
    for (int i = 0; i < 60; ++i)
        spin.step();
    require(spin.bodies[0].rotation[2] > 0.5f, "A rigid body integrates angular velocity");
    using Type = WorldSimulation::Joint::Type;
    for (const auto type : {Type::Fixed,Type::Hinge,Type::Distance}) {
        WorldSimulation linked;
        linked.bodies.push_back({{0,0,3},{0.1f,0.1f,0.1f}});
        linked.bodies.push_back({{0,2,3},{0.2f,0.2f,0.2f},{},0,true});
        linked.joints.push_back({type,0,1,{0,0,3},{1,0,0}});
        for (int i = 0; i < 75; ++i)
            linked.step();
        const auto &body = linked.bodies[1];
        require(std::abs(std::hypot(body.position[1],body.position[2]-3)-2) < 0.025f,
                "Joint holds its bodies together under gravity");
        if (type == Type::Fixed)
            require(std::abs(body.position[2]-3) < 0.02f, "Fixed joint holds the initial pose");
        if (type == Type::Hinge)
            require(body.position[2] < 2.5f && std::abs(body.rotation[0]) > 0.15f,
                    "Hinge swings around its configured axis");
    }
    PlayableScene playable;
    playable.simulation.bodies.push_back({{0,0,2},{0.3f,0.3f,0.85f},{},0,true});
    playable.simulation.bodies[0].lockRotation = true;
    playable.simulation.bodies[0].shape = BoxBody::Shape::Capsule;
    playable.simulation.terrains.push_back({terrain,{}});
    playable.foliage.push_back({8,42});
    playable.localFogRadius = 13;
    playable.meshes.push_back({-1,makePlayVertices(cube)});
    std::stringstream package;
    require(writePlayable(package,playable), "Serialize a standalone scene package");
    auto loaded = readPlayable(package);
    require(loaded && loaded->simulation.terrains[0].field.samples == terrain.samples
            && loaded->simulation.terrains[0].field.layers == terrain.layers && loaded->localFogRadius == 13
            && loaded->simulation.bodies.front().shape == BoxBody::Shape::Capsule
            && loaded->foliage[0].seed == 42 && loaded->meshes[0].vertices.size() == 36
            && loaded->meshes[0].clearCoat == 0 && loaded->meshes[0].clearCoatRoughness == 0,
            "Standalone package restores terrain, foliage, rigid bodies and render geometry");
    {
        // Format 30: static meshes grouped into 64 m cells, which a reader can load one at a time.
        PlayableScene city;
        city.simulation.bodies.push_back({{0,0,2},{0.3f,0.3f,0.85f},{},0,true});
        city.simulation.bodies[0].lockRotation = true; // a player body
        const auto block = makePlayVertices(cube);
        for (int i = 0; i < 6; ++i) {
            auto moved = block;
            for (auto &vertex : moved) {
                vertex.position[0] += float(i)*40;
                if (i == 0)
                    vertex.color = {1,1,1}; // a white mesh: its mean color must not round past 1
            }
            city.meshes.push_back({-1,moved});
        }
        city.meshes.push_back({0,block}); // follows a body, so it stays global
        city.gameplay.routes.push_back({{{0,0,0},{10,0,0}},5});
        city.partitions.cellSize = 64;
        std::stringstream file;
        require(writePlayable(file,city) && file.str().starts_with("WORLDFORGE_PLAY 30\n"),"Cells select playable format 30");
        std::stringstream fullCopy(file.str()), indexCopy(file.str());
        const auto full = readPlayable(fullCopy);
        const auto &cells = full ? full->partitions.cells : std::vector<PlayCell>{};

        require(full && full->meshes.size() == 7 && full->partitions.globalMeshes == 1 && full->meshes[0].body == 0
                && cells.size() == 4 && cells[0].meshCount == 2 && cells[2].meshCount == 1 && full->gameplay.routes.size() == 1,
                "A full read of format 30 restores every mesh, global ones first, and four cells along x");
        require(std::ranges::all_of(cells,[](const PlayCell &cell) { return !cell.proxy.empty() && cell.proxy.size() <= 36*cell.meshCount; })
                && cells[1].minimum[0] > 60 && cells[1].maximum[0] < 130,
                "Each cell carries its bounds and a proxy no larger than a box per mesh");
        const auto index = readPlayableIndex(indexCopy);
        require(index && index->meshes.size() == 1 && index->partitions.cells.size() == 4 && index->gameplay.routes.size() == 1,
                "The index skips the cells' meshes and still reads what follows them");
        bool matching = index.has_value();
        for (std::size_t cell = 0; matching && cell < cells.size(); ++cell) {
            const auto meshes = readPlayableCell(indexCopy,*index,cell);
            matching = meshes && meshes->size() == cells[cell].meshCount;
            for (std::size_t i = 0; matching && i < meshes->size(); ++i) {
                const auto &expected = full->meshes[cells[cell].firstMesh+i].vertices;
                const auto &actual = (*meshes)[i].vertices;
                matching = actual.size() == expected.size() && std::ranges::equal(actual,expected,{},&PlayVertex::position,
                    &PlayVertex::position);
            }
        }
        require(matching,"Each cell reads on its own exactly as in the full read");
    }
    playable.meshes[0].clearCoat = 0.75f;
    playable.meshes[0].clearCoatRoughness = 0.19f;
    std::stringstream coatedPackage;
    require(writePlayable(coatedPackage,playable) && coatedPackage.str().starts_with("WORLDFORGE_PLAY 21\n"),
            "A coated mesh selects version 21 without requiring other material inputs");
    const auto coated = readPlayable(coatedPackage);
    require(coated && coated->meshes.front().clearCoat == 0.75f && coated->meshes.front().clearCoatRoughness == 0.19f,
            "Clear coat weight and roughness round-trip through the standalone package");
    playable.meshes[0].clearCoatRoughness = 1.1f;
    std::ostringstream invalidCoat;
    require(!writePlayable(invalidCoat,playable), "Reject clear coat factors outside the supported range");
    playable.meshes[0].clearCoatRoughness = 0.19f;
    playable.meshes[0].layers = {{0.2f,0.4f,0.7f},0.55f};
    std::stringstream layeredPackage;
    require(writePlayable(layeredPackage,playable) && layeredPackage.str().starts_with("WORLDFORGE_PLAY 22\n"),
            "A mesh with sheen selects the layered material package format");
    const auto layered = readPlayable(layeredPackage);
    require(layered && layered->meshes.front().layers == playable.meshes.front().layers
            && layered->meshes.front().clearCoatRoughness == 0.19f && coated->meshes.front().layers == MaterialLayers{},
            "Sheen and clear coat round-trip together and version 21 defaults to zero sheen");
    playable.meshes[0].layers.transmission = 0.9f;
    playable.meshes[0].layers.ior = 1.65f;
    std::stringstream glassPackage;
    require(writePlayable(glassPackage,playable) && glassPackage.str().starts_with("WORLDFORGE_PLAY 23\n"),
            "Thin glass selects the transmissive material package format");
    const auto glass = readPlayable(glassPackage);
    require(glass && glass->meshes.front().layers == playable.meshes.front().layers
            && layered->meshes.front().layers.transmission == 0 && layered->meshes.front().layers.ior == 1.5f,
            "Transmission and IOR round-trip with earlier packages defaulting to opaque material");
    auto &advanced = playable.meshes[0].layers;
    advanced.subsurface = 0.55f;
    advanced.thickness = 0.017f;
    advanced.scatteringDistance = 0.029f;
    advanced.anisotropy = 0.8f;
    advanced.anisotropyRotation = -1.57f;
    std::stringstream advancedPackage;
    require(writePlayable(advancedPackage,playable) && advancedPackage.str().starts_with("WORLDFORGE_PLAY 24\n"),
            "Measured scattering and directional roughness use version 24");
    const auto advancedSurface = readPlayable(advancedPackage);
    require(advancedSurface && advancedSurface->meshes.front().layers == advanced
            && !glass->meshes.front().layers.hasScatteringOrAnisotropy(),
            "Subsurface and anisotropy values round-trip with zero weights in earlier packages");
    advanced.coatNormalScale = 0.6f;
    auto &maps = playable.meshes.front().surfaceMaps;
    for (std::size_t i = baseMaterialTextureCount-1; i < maps.size(); ++i)
        maps[i] = {0x89,'P','N','G',uint8_t(i)};
    std::stringstream mappedPackage;
    require(writePlayable(mappedPackage,playable) && mappedPackage.str().starts_with("WORLDFORGE_PLAY 25\n"),
            "Layer maps use version 25");
    const auto mapped = readPlayable(mappedPackage);
    require(mapped && mapped->meshes.front().surfaceMaps == maps && mapped->meshes.front().layers == advanced
            && advancedSurface->meshes.front().layers.coatNormalScale == 1
            && advancedSurface->meshes.front().surfaceMaps.back().empty(),
            "Layer maps and normal scale round-trip while old packages retain their defaults");
    advanced.wetness = 0.68f;
    advanced.puddles = 0.39f;
    advanced.heightScale = 0.08f;
    advanced.decal = true;
    advanced.decalOpacity = 0.77f;
    playable.meshes.front().surfaceAlpha = 0.81f;
    std::stringstream streetLayersPackage;
    require(writePlayable(streetLayersPackage,playable) && streetLayersPackage.str().starts_with("WORLDFORGE_PLAY 26\n"),
            "Street materials use version 26");
    const auto streetLayers = readPlayable(streetLayersPackage);
    require(streetLayers && streetLayers->meshes.front().layers == advanced && streetLayers->meshes.front().surfaceMaps == maps
            && streetLayers->meshes.front().surfaceAlpha == 0.81f && !mapped->meshes.front().layers.hasStreetLayers(),
            "Street layers and decal alpha round-trip with unchanged layer maps and old defaults");
    playable.probeGi = playable.screenSpaceGi = playable.reflectionProbes = true;
    std::stringstream giPackage;
    require(writePlayable(giPackage,playable) && giPackage.str().starts_with("WORLDFORGE_PLAY 27\n"),
            "R3 lighting controls use version 27");
    const auto gi = readPlayable(giPackage);
    require(gi && gi->probeGi && gi->screenSpaceGi && gi->reflectionProbes && gi->meshes.front().layers == advanced
        && !streetLayers->probeGi && !streetLayers->screenSpaceGi && !streetLayers->reflectionProbes,
        "R3 lighting controls round-trip while older scenes retain their lighting");

    playable.weather = {0.8f,0.25f,0.75f,20};
    std::stringstream weatherPackage;
    require(writePlayable(weatherPackage,playable) && weatherPackage.str().starts_with("WORLDFORGE_PLAY 28\n"),
            "Cloud cover and distance haze use version 28");
    const auto weather = readPlayable(weatherPackage);
    require(weather && weather->weather.haze == 0.8f && weather->weather.cloudsAt(-1) == 0.25f
        && weather->weather.cloudsAt(10) == 0.5f && weather->weather.cloudsAt(25) == 0.75f
        && !gi->weather.authored(),"Weather round-trips and smoothly interpolates to its target during play");

    PlayLight warm{{3,4,2},0.025f,false};
    warm.color = {1,0.43f,0.12f};
    playable.lights.push_back(warm);
    std::stringstream lampPackage;
    require(writePlayable(lampPackage,playable) && lampPackage.str().starts_with("WORLDFORGE_PLAY 29\n"),
            "Colored lamps use version 29");
    const auto lamp = readPlayable(lampPackage);
    require(lamp && lamp->lights.back().color == warm.color && lamp->weather.haze == 0.8f
        && std::ranges::all_of(weather->lights,[](const PlayLight &light) { return light.color == Point3{1,1,1}; }),
        "A lamp's linear tint round-trips and old scenes use white lamps");
    playable.lights.back().color[0] = 1.01f;
    std::ostringstream invalidLamp;
    require(!writePlayable(invalidLamp,playable),"Reject lamp tints the reader cannot load");
    std::istringstream oversized("WORLDFORGE_PLAY 1\n999999 0 0 0 0 0");
    require(!readPlayable(oversized), "Reject oversized standalone scene package tables");

    // Version 5 carries UVs, base-color maps, traffic, and missions to the standalone Player.
    PlayableScene street;
    street.simulation.bodies.push_back({{0,0,1},{0.3f,0.3f,0.85f},{},0,true});
    street.simulation.bodies[0].lockRotation = true;
    street.simulation.bodies.push_back({{0,-0.25f,0},{30,30,0.25f}});
    street.simulation.bodies.push_back({{-6,-4,0.8f},{2,0.9f,0.7f},{},0,true});
    street.simulation.bodies.push_back({{0,4,1.5f},{4,0.15f,1.5f}});
    auto textured = makePlayVertices(cube);
    textured[1].uv = {0.25f,0.75f};
    street.meshes.push_back({-1,textured,{},{},0,{0x89,'P','N','G',0,1,2,3,4,5}});
    street.gameplay.routes.push_back({{{-6,-4,0},{6,-4,0},{6,-8,0},{-6,-8,0}},6});
    street.gameplay.vehicles.push_back({2,0});
    Mission mission;
    mission.name = "Repair \"Run\"";
    mission.goal = "take the parts to the shop";
    mission.start = {0,-1,0};
    mission.target = {0,6,0};
    street.gameplay.missions.push_back(mission);
    std::stringstream streetPackage;
    require(writePlayable(streetPackage,street), "Serialize a street package");
    auto streetLoaded = readPlayable(streetPackage);
    require(streetLoaded && streetLoaded->meshes[0].baseColorTexture == street.meshes[0].baseColorTexture
            && streetLoaded->meshes[0].vertices[1].uv == textured[1].uv
            && streetLoaded->gameplay.routes.size() == 1 && streetLoaded->gameplay.routes[0].points.size() == 4
            && streetLoaded->gameplay.vehicles.size() == 1 && streetLoaded->gameplay.missions.size() == 1
            && streetLoaded->gameplay.missions[0].name == mission.name,
            "Street package restores UVs, base-color maps, traffic, and missions");
    {
        // Mission text is limited in UTF-8 bytes. Three-byte characters reach the limit at 53 characters.
        std::string name;
        for (int i = 0; i < 40; ++i)
            name += "\xe4\xbf\xae";
        auto &missionText = street.gameplay.missions[0].name;
        const auto original = std::exchange(missionText,name);
        std::stringstream fits;
        require(writePlayable(fits,street), "Serialize 120 bytes of multibyte mission text");
        const auto multibyte = readPlayable(fits);
        require(multibyte && multibyte->gameplay.missions[0].name == name, "Multibyte mission text survives the Player format");
        for (int i = 0; i < 14; ++i)
            missionText += "\xe4\xbf\xae";
        std::stringstream overlong;
        require(!writePlayable(overlong,street), "Refuse to write mission text the Player cannot read");
        missionText = original;
    }
    {
        // Exposure and view transform need Player format 14; default display settings keep the older format.
        std::stringstream plain;
        require(writePlayable(plain,street) && !plain.str().starts_with("WORLDFORGE_PLAY 14"),
                "Default display settings keep the older Player format");
        street.exposure = -1.5f;
        street.viewTransform = color::ViewTransform::Standard;
        std::stringstream display;
        require(writePlayable(display,street) && display.str().starts_with("WORLDFORGE_PLAY 14"),
                "Display settings use Player format 14");
        const auto shown = readPlayable(display);
        require(shown && shown->exposure == -1.5f && shown->viewTransform == color::ViewTransform::Standard
                && shown->gameplay.missions.size() == 1,
                "Exposure and view transform survive the Player format");
        street.exposure = 18;
        std::stringstream outOfRange;
        require(!writePlayable(outOfRange,street), "Exposure outside the supported range is refused");
        street.exposure = 0;
        street.viewTransform = color::ViewTransform::AgX;
    }
    const auto around = findPath(street.simulation,{0,0,1},{0,8,1},0.4f,0.15f,1.85f);
    require(around.size() >= 2 && std::ranges::all_of(around,[](const Point3 &p) { return std::abs(p[0]) > 4 || p[1] > 4.5f; }),
            "Path planning walks around a wall instead of through it");
    auto &streetWorld = streetLoaded->simulation;
    streetLoaded->gameplay.start(streetWorld);
    for (int step = 0; step < 120*20 && !streetLoaded->gameplay.trafficLooped(); ++step) {
        streetLoaded->gameplay.update(streetWorld,0,false);
        streetWorld.step();
    }
    require(streetLoaded->gameplay.trafficLooped(), "A traffic vehicle completes its route");
    Autopilot pilot;
    for (int step = 0; step < 120*30 && !streetLoaded->gameplay.missionsComplete(); ++step) {
        float pilotYaw = 0;
        bool interact = false;
        const bool forward = pilot.drive(streetWorld,0,streetLoaded->gameplay,pilotYaw,interact);
        int jumpBuffer = 0;
        streetLoaded->gameplay.update(streetWorld,0,interact);
        drivePlayer(streetWorld.bodies[0],{0,0,1},pilotYaw,int(forward),0,false,jumpBuffer);
        streetWorld.step();
    }
    if (!streetLoaded->gameplay.missionsComplete()) {
        const auto &p = streetWorld.bodies[0].position;
        std::cerr << "Autopilot ended at " << p[0] << ' ' << p[1] << ' ' << p[2] << " state "
                  << int(streetLoaded->gameplay.missions[0].state) << " path " << pilot.path.size()
                  << " stuck " << pilot.stuck << '\n';
    }
    require(streetLoaded->gameplay.missionsComplete(), "The autopilot completes a mission around a wall");
    std::cout << "Scene, mesh and world simulation contracts passed\n";
}
