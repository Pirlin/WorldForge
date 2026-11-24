#pragma once

#include <algorithm>

namespace worldforge {

struct Weather {
    static constexpr float maximumHaze = 12, maximumTransition = 3600;
    float haze = 0;                 // aerosol extinction per kilometer at ground level
    float cloudCover = 0;           // sky coverage at the start of a play session
    float targetCloudCover = 0;
    float cloudTransition = 0;      // seconds, zero holds the initial coverage

    float cloudsAt(double seconds) const
    {
        if (cloudTransition <= 0)
            return cloudCover;
        const float t = float(std::clamp(seconds/cloudTransition,0.0,1.0));
        return cloudCover+(targetCloudCover-cloudCover)*t*t*(3-2*t);
    }

    bool authored() const { return haze != 0 || cloudCover != 0 || targetCloudCover != 0 || cloudTransition != 0; }
};

inline float cloudSunTransmission(float cover) { return 1-0.92f*cover; }

// Fit the overcast contribution as diffuse sky fill with less variation by direction.
inline float cloudSkyScale(float cover,bool mean) { return (1-0.35f*cover)*(mean ? 1 : 1-0.75f*cover); }

}
