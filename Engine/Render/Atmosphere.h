#pragma once

// A physically based sky (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique"), shared by the editor viewport, the Player and the Cycles reference.
//
// The Earth's atmosphere scatters sunlight: air molecules (Rayleigh) scatter blue far more than red, which makes the
// sky blue and a low sun orange; aerosols (Mie) scatter forward, which makes the glow around the sun; ozone absorbs
// some orange, which keeps twilight blue. The sky's radiance in a direction is the light scattered toward the eye
// along that ray, once directly from the sun and many more times between particles. The many-times part uses
// Hillaire's approximation: a table of the light that ends up scattered isotropically at each height and sun angle.
//
// Units are scene units (ColorPipeline.h). The authored sun intensity is the sunlight that reaches the ground with
// the sun overhead; a lower sun passes through more air and arrives dimmer and warmer (sunLight), and the sky's
// brightness follows from the same light above the atmosphere.

#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/ShadowCascades.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace worldforge::atmosphere {

inline constexpr float bottomRadius = 6360e3f, topRadius = 6460e3f; // meters
inline constexpr color::Rgb rayleighScattering{5.802e-6f,13.558e-6f,33.1e-6f}; // per meter at sea level
inline constexpr float rayleighHeight = 8000;
inline constexpr float mieScattering = 3.996e-6f, mieExtinction = 4.40e-6f, mieHeight = 1200, mieAnisotropy = 0.8f;
inline constexpr color::Rgb ozoneAbsorption{0.650e-6f,1.881e-6f,0.085e-6f};
inline constexpr float ozoneCenter = 25e3f, ozoneWidth = 15e3f;
inline constexpr float groundAlbedo = 0.25f;  // as color::groundAlbedo
inline constexpr float viewerHeight = 2;      // meters above the ground

namespace detail {

inline constexpr float pi = 3.14159265358979f;
using color::Rgb;

inline Rgb operator*(const Rgb &a,const Rgb &b) { return {a[0]*b[0],a[1]*b[1],a[2]*b[2]}; }
inline Rgb operator*(const Rgb &a,float s) { return {a[0]*s,a[1]*s,a[2]*s}; }
inline Rgb operator+(const Rgb &a,const Rgb &b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
inline Rgb exp(const Rgb &a) { return {std::exp(a[0]),std::exp(a[1]),std::exp(a[2])}; }
inline float luminance(const Rgb &a) { return 0.2126f*a[0]+0.7152f*a[1]+0.0722f*a[2]; }

struct Medium {
    Rgb scattering, extinction;
    Rgb rayleigh; // Rayleigh scattering
    float mie;    // Mie scattering
};

inline Medium medium(float height)
{
    const float rayleighDensity = std::exp(-std::max(height,0.0f)/rayleighHeight);
    const float mieDensity = std::exp(-std::max(height,0.0f)/mieHeight);
    const float ozoneDensity = std::max(0.0f,1-std::abs(height-ozoneCenter)/ozoneWidth);
    Medium result;
    result.rayleigh = rayleighScattering*rayleighDensity;
    result.mie = mieScattering*mieDensity;
    result.scattering = result.rayleigh+Rgb{result.mie,result.mie,result.mie};
    const float mieTotal = mieExtinction*mieDensity;
    result.extinction = result.rayleigh+Rgb{mieTotal,mieTotal,mieTotal}+ozoneAbsorption*ozoneDensity;
    return result;
}

// Distance along a ray from radius r with cosine mu (to the vertical) to a sphere of the given radius; negative when
// it misses. nearest picks the first hit (for the ground), otherwise the far one (for the top of the atmosphere).
inline float sphereDistance(float r,float mu,float radius,bool nearest)
{
    const float along = -r*mu;
    const float radialDifference = (r-radius)*(r+radius);
    const float discriminant = along*along-radialDifference;
    if (discriminant < 0)
        return -1;
    const float root = std::sqrt(discriminant);
    const float far = along+root;
    if (!nearest)
        return along < 0 ? -radialDifference/(root-along) : far;
    // Use the product of the roots to retain precision for rays starting just above the ground.
    return along > 0 ? radialDifference/far : -1;
}

inline float horizonDistance(float r) { return std::sqrt(std::max(0.0f,(r-bottomRadius)*(r+bottomRadius))); }

inline float rayleighPhase(float cosine) { return 3/(16*pi)*(1+cosine*cosine); }

inline float miePhase(float cosine)
{
    // Cornette-Shanks, as Hillaire uses.
    const float g = mieAnisotropy, g2 = g*g;
    return 3/(8*pi)*(1-g2)*(1+cosine*cosine)/((2+g2)*std::pow(std::max(1+g2-2*g*cosine,1e-6f),1.5f));
}

} // namespace detail

// The sky for one sun direction. The tables span all sun angles and are shared across instances.
class Sky {
public:
    static constexpr int transmittanceHeights = 32, transmittanceAngles = 128;
    static constexpr int multipleHeights = 32, multipleAngles = 32;

    // towardSun need not be normalized; sunIntensity is the authored sunlight at the ground (scene units).
    Sky(std::array<float,3> towardSun,float sunIntensity)
    {
        const float length = std::sqrt(towardSun[0]*towardSun[0]+towardSun[1]*towardSun[1]+towardSun[2]*towardSun[2]);
        sun_ = length > 1e-6f ? std::array<float,3>{towardSun[0]/length,towardSun[1]/length,towardSun[2]/length}
                              : std::array<float,3>{0,0,1};
        const float elevation = std::asin(std::clamp(sun_[2],-1.0f,1.0f));
        constexpr float radius = shadow::sunAngularDiameter*0.5f;
        const float edge = std::clamp((elevation-std::asin(horizonCosine()))/radius,-1.0f,1.0f);
        const float chord = std::sqrt(std::max(0.0f,1-edge*edge));
        const float visible = (std::acos(-edge)+edge*chord)/detail::pi;
        // Use the mean elevation of the visible part of the disc for its atmospheric extinction.
        const float meanOffset = visible > 1e-6f ? radius*(2.0f/3.0f)*chord*chord*chord/(detail::pi*visible) : 0;
        sunTransmittance_ = visible > 1e-6f ? transmittance(bottomRadius+viewerHeight,std::sin(elevation+meanOffset)) : color::Rgb{};
        // Sunlight above the atmosphere (irradiance), chosen so an overhead sun gives the authored intensity.
        const float overhead = std::max(detail::luminance(transmittance(bottomRadius+viewerHeight,1)),1e-4f);
        const float irradiance = detail::pi*std::max(sunIntensity,0.0f)/overhead;
        solar_ = {irradiance,irradiance,irradiance};
        const color::Rgb disc{sunTransmittance_[0]*irradiance/detail::pi,sunTransmittance_[1]*irradiance/detail::pi,
                              sunTransmittance_[2]*irradiance/detail::pi};
        sunDiscStrength_ = detail::luminance(disc);
        sunLight_ = {disc[0]*visible,disc[1]*visible,disc[2]*visible};

        // A distant ground ray reflects the upper sky even before the sun is visible. Sample
        // that hemisphere with cosine weighting once for this sun position.
        using namespace detail;
        constexpr int bands = 12, azimuths = 16;
        for (int band = 0; band < bands; ++band) {
            const float z = std::sqrt((float(band)+0.5f)/bands);
            const float radiusInPlane = std::sqrt(1-z*z);
            for (int azimuth = 0; azimuth < azimuths; ++azimuth) {
                const float phi = 2*pi*(float(azimuth)+0.5f*float(band%2))/azimuths;
                const float direction[]{radiusInPlane*std::cos(phi),radiusInPlane*std::sin(phi),z};
                groundSky_ = groundSky_+radiance(direction)*(groundAlbedo/(bands*azimuths));
            }
        }
    }

    const std::array<float,3> &towardSun() const { return sun_; }
    // Sunlight at the ground in scene units per channel: what a white surface facing the sun reflects.
    color::Rgb sunLight() const { return sunLight_; }
    float sunDiscStrength() const { return sunDiscStrength_; }
    static float horizonCosine()
    {
        const float r = bottomRadius+viewerHeight;
        return -detail::horizonDistance(r)/r;
    }
    // How much sunlight reaches the ground, per channel (0 to 1).
    color::Rgb sunTransmittance() const { return sunTransmittance_; }

    // Radiance of the sky in direction (unit, z up), seen from the ground: light scattered along the ray, and for rays
    // that reach the ground, the ground lit by the sun and sky seen through it. The sun's disc is not included.
    color::Rgb radiance(const float direction[3],int steps = 32) const
    {
        using namespace detail;
        const float r = bottomRadius+viewerHeight;
        const float mu = direction[2];
        float distance = sphereDistance(r,mu,bottomRadius,true);
        const bool ground = distance > 0;
        if (!ground)
            distance = sphereDistance(r,mu,topRadius,false);
        if (distance <= 0)
            return {};
        const float cosine = direction[0]*sun_[0]+direction[1]*sun_[1]+direction[2]*sun_[2];
        const float rayleigh = rayleighPhase(cosine), mie = miePhase(cosine);
        color::Rgb light{}, through{1,1,1};
        float previous = 0;
        for (int i = 0; i < steps; ++i) {
            // Steps grow with distance, so the dense air near the viewer gets the most.
            const float fraction = (float(i)+0.5f)/float(steps);
            const float t = distance*fraction*fraction;
            const float next = distance*(float(i)+1)*(float(i)+1)/(float(steps)*float(steps));
            const float dt = next-previous;
            previous = next;
            // The sample point: radius and the sun's cosine there.
            const float px = direction[0]*t, py = direction[1]*t, pz = r+direction[2]*t;
            const float pr = std::sqrt(px*px+py*py+pz*pz);
            const float sunMu = (px*sun_[0]+py*sun_[1]+pz*sun_[2])/pr;
            const auto m = medium(pr-bottomRadius);
            const auto toSun = solarTransmission(pr,sunMu);
            const auto scattered = (m.rayleigh*rayleigh+color::Rgb{m.mie*mie,m.mie*mie,m.mie*mie})*toSun
                +m.scattering*multipleScattering(pr,sunMu);
            // Integrate the scattering over this step analytically (Hillaire, eq. 6).
            const auto stepThrough = exp(m.extinction*(-dt));
            for (int c = 0; c < 3; ++c) {
                const float extinction = std::max(m.extinction[std::size_t(c)],1e-9f);
                light[std::size_t(c)] += through[std::size_t(c)]*scattered[std::size_t(c)]*(1-stepThrough[std::size_t(c)])/extinction;
            }
            through = through*stepThrough;
        }
        light = light*solar_;
        if (ground) {
            // The virtual ground reflects direct light at the hit and the diffuse upper sky.
            const float px = direction[0]*distance, py = direction[1]*distance, pz = r+direction[2]*distance;
            const float pr = std::sqrt(px*px+py*py+pz*pz);
            const float sunMu = (px*sun_[0]+py*sun_[1]+pz*sun_[2])/pr;
            const auto onGround = transmittance(pr,sunMu)*std::max(sunMu,0.0f)*(1/pi);
            light = light+through*(groundSky_+onGround*solar_*groundAlbedo);
        }
        return light;
    }

    // The fraction of light that passes from radius r along cosine mu to space (0 where the Earth blocks it).
    static color::Rgb transmittance(float r,float mu)
    {
        using namespace detail;
        r = std::clamp(r,bottomRadius,topRadius);
        mu = std::clamp(mu,-1.0f,1.0f);
        const float rho = horizonDistance(r), horizon = horizonDistance(topRadius);
        if (mu < 0 && -r*mu >= rho)
            return {};
        const float minimum = topRadius-r, maximum = rho+horizon;
        const float distance = sphereDistance(r,mu,topRadius,false);
        const float u = std::clamp((distance-minimum)/(maximum-minimum),0.0f,1.0f);
        static const auto table = buildTransmittance();
        return lookup(table,transmittanceAngles,transmittanceHeights,u,rho/horizon);
    }

private:
    static color::Rgb solarTransmission(float radius,float cosine)
    {
        using namespace detail;
        constexpr float discRadius = shadow::sunAngularDiameter*0.5f;
        const float horizon = -horizonDistance(radius)/radius;
        // In the narrow band around the terminator, integrate the visible part of the finite solar disc.
        if (cosine >= horizon+discRadius)
            return transmittance(radius,cosine);
        if (cosine <= horizon-discRadius)
            return {};
        const float edge = std::clamp((std::asin(std::clamp(cosine,-1.0f,1.0f))-std::asin(horizon))/discRadius,-1.0f,1.0f);
        const float chord = std::sqrt(std::max(0.0f,1-edge*edge));
        const float visible = (std::acos(-edge)+edge*chord)/pi;
        if (visible <= 1e-6f)
            return {};
        const float offset = discRadius*(2.0f/3.0f)*chord*chord*chord/(pi*visible);
        return transmittance(radius,std::sin(std::asin(std::clamp(cosine,-1.0f,1.0f))+offset))*visible;
    }

    static color::Rgb lookup(const std::vector<color::Rgb> &table,int columns,int rows,float x,float y)
    {
        const float fx = x*float(columns-1), fy = y*float(rows-1);
        const int x0 = std::min(int(fx),columns-2), y0 = std::min(int(fy),rows-2);
        const float sx = fx-float(x0), sy = fy-float(y0);
        const auto at = [&](int cx,int cy) { return table[std::size_t(cy*columns+cx)]; };
        using namespace detail;
        return (at(x0,y0)*(1-sx)+at(x0+1,y0)*sx)*(1-sy)+(at(x0,y0+1)*(1-sx)+at(x0+1,y0+1)*sx)*sy;
    }

    static color::Rgb multipleScattering(float r,float mu)
    {
        const float h = std::clamp((r-bottomRadius)/(topRadius-bottomRadius),0.0f,1.0f);
        static const auto table = buildMultipleScattering();
        return lookup(table,multipleAngles,multipleHeights,std::clamp(mu*0.5f+0.5f,0.0f,1.0f),std::sqrt(h));
    }

    // Map altitude by distance to the ground horizon and angle by distance to the top of the atmosphere.
    // This spends the table on paths that reach space and resolves long grazing paths near sunset.
    static std::vector<color::Rgb> buildTransmittance()
    {
        using namespace detail;
        std::vector<Rgb> table(std::size_t(transmittanceHeights*transmittanceAngles));
        const float horizon = horizonDistance(topRadius);
        for (int row = 0; row < transmittanceHeights; ++row) {
            const float rho = horizon*float(row)/float(transmittanceHeights-1);
            const float r = std::sqrt(bottomRadius*bottomRadius+rho*rho);
            const float minimum = topRadius-r, maximum = rho+horizon;
            for (int column = 0; column < transmittanceAngles; ++column) {
                const float distance = std::lerp(minimum,maximum,float(column)/float(transmittanceAngles-1));
                auto &result = table[std::size_t(row*transmittanceAngles+column)];
                const float radiusDifference = (topRadius-r)*(topRadius+r);
                const float radial = distance > 0 ? (radiusDifference-distance*distance)/(2*distance) : r;
                constexpr int steps = 40;
                Rgb depth{};
                float start = 0;
                for (int i = 0; i < steps; ++i) {
                    const float fraction = float(i+1)/steps;
                    const float end = distance*fraction*fraction;
                    const float t = (start+end)*0.5f;
                    const float pr = std::sqrt(r*r+2*radial*t+t*t);
                    depth = depth+medium(pr-bottomRadius).extinction*(end-start);
                    start = end;
                }
                result = exp(depth*-1.0f);
            }
        }
        return table;
    }

    // Hillaire's multiple scattering table: for each height and sun cosine, the light scattered isotropically after
    // two or more bounces (psi_ms = L2 / (1 - f_ms)), per unit of sunlight.
    static std::vector<color::Rgb> buildMultipleScattering()
    {
        using namespace detail;
        std::vector<Rgb> table(std::size_t(multipleHeights*multipleAngles));
        constexpr int directions = 8, steps = 20;
        const float isotropic = 1/(4*pi);
        for (int row = 0; row < multipleHeights; ++row) {
            const float h = std::pow(float(row)/float(multipleHeights-1),2.0f)*(topRadius-bottomRadius);
            const float r = bottomRadius+std::max(h,1.0f);
            for (int column = 0; column < multipleAngles; ++column) {
                const float sunMu = float(column)/float(multipleAngles-1)*2-1;
                const float sun[3]{std::sqrt(std::max(0.0f,1-sunMu*sunMu)),0,sunMu};
                Rgb second{}, transfer{};
                for (int a = 0; a < directions; ++a)
                    for (int b = 0; b < directions; ++b) {
                        // Uniform directions over the sphere.
                        const float z = 1-2*(float(a)+0.5f)/directions;
                        const float phi = 2*pi*(float(b)+0.5f)/directions;
                        const float s = std::sqrt(std::max(0.0f,1-z*z));
                        const float d[3]{s*std::cos(phi),s*std::sin(phi),z};
                        float distance = sphereDistance(r,z,bottomRadius,true);
                        const bool ground = distance > 0;
                        if (!ground)
                            distance = sphereDistance(r,z,topRadius,false);
                        Rgb light{}, scattered{}, through{1,1,1};
                        for (int i = 0; i < steps; ++i) {
                            const float t = (float(i)+0.5f)/steps*distance, dt = distance/steps;
                            const float px = d[0]*t, py = d[1]*t, pz = r+d[2]*t;
                            const float pr = std::sqrt(px*px+py*py+pz*pz);
                            const float mu = (px*sun[0]+py*sun[1]+pz*sun[2])/pr;
                            const auto m = medium(pr-bottomRadius);
                            const auto stepThrough = exp(m.extinction*-dt);
                            const auto toSun = transmittance(pr,mu);
                            for (int c = 0; c < 3; ++c) {
                                const float extinction = std::max(m.extinction[std::size_t(c)],1e-9f);
                                const float integral = through[std::size_t(c)]*(1-stepThrough[std::size_t(c)])/extinction;
                                light[std::size_t(c)] += integral*m.scattering[std::size_t(c)]*toSun[std::size_t(c)]*isotropic;
                                scattered[std::size_t(c)] += integral*m.scattering[std::size_t(c)];
                            }
                            through = through*stepThrough;
                        }
                        if (ground) {
                            const float px = d[0]*distance, py = d[1]*distance, pz = r+d[2]*distance;
                            const float pr = std::sqrt(px*px+py*py+pz*pz);
                            const float mu = (px*sun[0]+py*sun[1]+pz*sun[2])/pr;
                            light = light+through*transmittance(pr,mu)*(std::max(mu,0.0f)*groundAlbedo/pi);
                        }
                        const float weight = 1.0f/(directions*directions);
                        second = second+light*weight;
                        transfer = transfer+scattered*(weight*isotropic*4*pi);
                    }
                auto &result = table[std::size_t(row*multipleAngles+column)];
                for (int c = 0; c < 3; ++c)
                    result[std::size_t(c)] = second[std::size_t(c)]/std::max(1-transfer[std::size_t(c)],1e-3f);
            }
        }
        return table;
    }

    std::array<float,3> sun_{};
    color::Rgb sunTransmittance_{1,1,1}, solar_{}, sunLight_{}, groundSky_{};
    float sunDiscStrength_ = 0;
};

} // namespace worldforge::atmosphere
