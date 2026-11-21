#include "Engine/Render/Flip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace worldforge::flip {
namespace {

using Color = std::array<float,3>;

constexpr float qc = 0.7f, qf = 0.5f, pc = 0.4f, pt = 0.95f, featureWidth = 0.082f;
constexpr float pi = std::numbers::pi_v<float>;
constexpr Color illuminant{0.950428545f,1.0f,1.088900371f};

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c/12.92f : std::pow((c+0.055f)/1.055f,2.4f);
}

Color linearToXyz(const Color &c)
{
    return {10135552.0f/24577794.0f*c[0]+8788810.0f/24577794.0f*c[1]+4435075.0f/24577794.0f*c[2],
            2613072.0f/12288897.0f*c[0]+8788810.0f/12288897.0f*c[1]+887015.0f/12288897.0f*c[2],
            1425312.0f/73733382.0f*c[0]+8788810.0f/73733382.0f*c[1]+70074185.0f/73733382.0f*c[2]};
}

Color xyzToLinear(const Color &c)
{
    return {3.241003275f*c[0]-1.537398934f*c[1]-0.498615861f*c[2],
            -0.969224334f*c[0]+1.875930071f*c[1]+0.041554224f*c[2],
            0.055639423f*c[0]-0.204011202f*c[1]+1.057148933f*c[2]};
}

Color xyzToYcxcz(Color c)
{
    for (int i = 0; i < 3; ++i)
        c[i] /= illuminant[i];
    return {116*c[1]-16,500*(c[0]-c[1]),200*(c[1]-c[2])};
}

Color ycxczToXyz(const Color &c)
{
    const float y = (c[0]+16)/116, x = y+c[1]/500, z = y-c[2]/200;
    return {x*illuminant[0],y*illuminant[1],z*illuminant[2]};
}

// CIELab with the Hunt adjustment: chroma scaled by 0.01 * L.
Color huntLab(Color c)
{
    constexpr float delta = 6.0f/29.0f, cube = delta*delta*delta, factor = 1.0f/(3*delta*delta), term = 4.0f/29.0f;
    for (int i = 0; i < 3; ++i) {
        c[i] /= illuminant[i];
        c[i] = c[i] > cube ? std::cbrt(c[i]) : factor*c[i]+term;
    }
    const float l = 116*c[1]-16;
    return {l,0.01f*l*500*(c[0]-c[1]),0.01f*l*200*(c[1]-c[2])};
}

float hyab(const Color &a,const Color &b)
{
    return std::abs(a[0]-b[0])+std::hypot(a[1]-b[1],a[2]-b[2]);
}

// Convolves one channel along x (horizontal) or y with a 1D kernel, clamping at the borders.
std::vector<float> convolve(const std::vector<float> &image,int width,int height,const std::vector<float> &kernel,bool horizontal)
{
    const int radius = int(kernel.size()/2);
    std::vector<float> result(image.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float sum = 0;
            for (int i = -radius; i <= radius; ++i) {
                const int xx = horizontal ? std::clamp(x+i,0,width-1) : x;
                const int yy = horizontal ? y : std::clamp(y+i,0,height-1);
                sum += kernel[std::size_t(i+radius)]*image[std::size_t(yy)*width+xx];
            }
            result[std::size_t(y)*width+x] = sum;
        }
    return result;
}

std::vector<float> separable(const std::vector<float> &image,int width,int height,const std::vector<float> &x,
                             const std::vector<float> &y)
{
    return convolve(convolve(image,width,height,x,true),width,height,y,false);
}

struct Prepared {
    std::array<std::vector<float>,3> ycxcz;
};

Prepared prepare(std::span<const std::uint8_t> rgb,std::size_t pixels)
{
    Prepared result;
    for (auto &channel : result.ycxcz)
        channel.resize(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const Color linear{srgbToLinear(rgb[i*3]/255.0f),srgbToLinear(rgb[i*3+1]/255.0f),srgbToLinear(rgb[i*3+2]/255.0f)};
        const auto value = xyzToYcxcz(linearToXyz(linear));
        for (int c = 0; c < 3; ++c)
            result.ycxcz[c][i] = value[c];
    }
    return result;
}

// Contrast sensitivity filtering in YCxCz, returned as Hunt-adjusted Lab per pixel.
std::vector<Color> filtered(const Prepared &image,int width,int height,float ppd)
{
    const auto gaussian = [](float x2,float a,float b) { return a*std::sqrt(pi/b)*std::exp(-pi*pi*x2/b); };
    const auto gaussianSqrt = [](float x2,float a,float b) { return std::sqrt(a*std::sqrt(pi/b))*std::exp(-pi*pi*x2/b); };
    const int radius = int(std::ceil(3*std::sqrt(0.04f/(2*pi*pi))*ppd));
    std::vector<float> y, cx, cz1, cz2;
    float sumY = 0, sumCx = 0, sumCz1 = 0, sumCz2 = 0;
    for (int i = -radius; i <= radius; ++i) {
        const float x = i/ppd, x2 = x*x;
        y.push_back(gaussian(x2,1,0.0047f));
        cx.push_back(gaussian(x2,1,0.0053f));
        cz1.push_back(gaussianSqrt(x2,34.1f,0.04f));
        cz2.push_back(gaussianSqrt(x2,13.5f,0.025f));
        sumY += y.back(); sumCx += cx.back(); sumCz1 += cz1.back(); sumCz2 += cz2.back();
    }
    const float czNormal = 1/std::sqrt(sumCz1*sumCz1+sumCz2*sumCz2);
    for (std::size_t i = 0; i < y.size(); ++i) {
        y[i] /= sumY;
        cx[i] /= sumCx;
        cz1[i] *= czNormal;
        cz2[i] *= czNormal;
    }
    const auto fy = separable(image.ycxcz[0],width,height,y,y);
    const auto fcx = separable(image.ycxcz[1],width,height,cx,cx);
    const auto fcz1 = separable(image.ycxcz[2],width,height,cz1,cz1);
    const auto fcz2 = separable(image.ycxcz[2],width,height,cz2,cz2);
    std::vector<Color> result(fy.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto linear = xyzToLinear(ycxczToXyz({fy[i],fcx[i],fcz1[i]+fcz2[i]}));
        for (auto &c : linear)
            c = std::clamp(c,0.0f,1.0f);
        result[i] = huntLab(linearToXyz(linear));
    }
    return result;
}

struct Features { std::vector<float> edge, point; };

Features features(const std::vector<float> &luminance,int width,int height,float ppd)
{
    const float sd = 0.5f*featureWidth*ppd;
    const int radius = int(std::ceil(3*sd));
    std::vector<float> g, dg, ddg;
    float gSum = 0, dgPositive = 0, dgNegative = 0, ddgPositive = 0, ddgNegative = 0;
    for (int i = -radius; i <= radius; ++i) {
        const float x = float(i), value = std::exp(-x*x/(2*sd*sd));
        g.push_back(value);
        dg.push_back(-x*value);
        ddg.push_back((x*x/(sd*sd)-1)*value);
        gSum += value;
        (dg.back() > 0 ? dgPositive : dgNegative) += dg.back();
        (ddg.back() > 0 ? ddgPositive : ddgNegative) += ddg.back();
    }
    for (std::size_t i = 0; i < g.size(); ++i) {
        g[i] /= gSum;
        dg[i] /= dg[i] > 0 ? dgPositive : -dgNegative;
        ddg[i] /= ddg[i] > 0 ? ddgPositive : -ddgNegative;
    }
    std::vector<float> normalized(luminance.size());
    for (std::size_t i = 0; i < luminance.size(); ++i)
        normalized[i] = (luminance[i]+16)/116;
    const auto dx = separable(normalized,width,height,dg,g);
    const auto dy = separable(normalized,width,height,g,dg);
    const auto ddx = separable(normalized,width,height,ddg,g);
    const auto ddy = separable(normalized,width,height,g,ddg);
    Features result{std::vector<float>(luminance.size()),std::vector<float>(luminance.size())};
    for (std::size_t i = 0; i < luminance.size(); ++i) {
        result.edge[i] = std::hypot(dx[i],dy[i]);
        result.point[i] = std::hypot(ddx[i],ddy[i]);
    }
    return result;
}

} // namespace

float defaultPixelsPerDegree()
{
    return 0.7f*(3840.0f/0.7f)*(pi/180.0f);
}

Result compare(std::span<const std::uint8_t> reference,std::span<const std::uint8_t> test,int width,int height,
               float pixelsPerDegree)
{
    Result result;
    const auto pixels = std::size_t(std::max(0,width))*std::size_t(std::max(0,height));
    if (!pixels || reference.size() < pixels*3 || test.size() < pixels*3 || pixelsPerDegree <= 0)
        return result;
    const auto preparedReference = prepare(reference,pixels), preparedTest = prepare(test,pixels);
    const auto labReference = filtered(preparedReference,width,height,pixelsPerDegree);
    const auto labTest = filtered(preparedTest,width,height,pixelsPerDegree);
    const auto featuresReference = features(preparedReference.ycxcz[0],width,height,pixelsPerDegree);
    const auto featuresTest = features(preparedTest.ycxcz[0],width,height,pixelsPerDegree);
    const float cmax = std::pow(hyab(huntLab(linearToXyz({0,1,0})),huntLab(linearToXyz({0,0,1}))),qc);
    const float pccmax = pc*cmax;
    result.error.resize(pixels);
    double total = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        float color = std::pow(hyab(labReference[i],labTest[i]),qc);
        color = color < pccmax ? color*pt/pccmax : pt+(color-pccmax)/(cmax-pccmax)*(1-pt);
        const float edge = std::abs(featuresReference.edge[i]-featuresTest.edge[i]);
        const float point = std::abs(featuresReference.point[i]-featuresTest.point[i]);
        const float feature = std::pow(std::max(edge,point)/std::sqrt(2.0f),qf);
        result.error[i] = std::pow(color,1-feature);
        total += result.error[i];
    }
    result.mean = total/double(pixels);
    return result;
}

void heat(float e,std::uint8_t rgb[3])
{
    // Samples of the magma color map at 0, 1/8, ..., 1.
    static constexpr std::uint8_t magma[9][3]{{0,0,4},{28,16,68},{79,18,123},{129,37,129},{181,54,122},
                                              {229,80,100},{251,135,97},{254,194,135},{252,253,191}};
    const float position = std::clamp(e,0.0f,1.0f)*8;
    const int index = std::min(7,int(position));
    const float t = position-float(index);
    for (int c = 0; c < 3; ++c)
        rgb[c] = std::uint8_t(std::lround(magma[index][c]*(1-t)+magma[index+1][c]*t));
}

}
