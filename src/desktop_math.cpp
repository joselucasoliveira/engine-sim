#include "../include/desktop_math.h"

#include <cmath>
#include <stdexcept>

namespace desktop {
float &Vector::operator[](unsigned index) {
    switch (index) {
    case 0: return x;
    case 1: return y;
    case 2: return z;
    case 3: return w;
    default: throw std::out_of_range("widget vector component");
    }
}
float Vector::operator[](unsigned index) const {
    switch (index) {
    case 0: return x;
    case 1: return y;
    case 2: return z;
    case 3: return w;
    default: throw std::out_of_range("widget vector component");
    }
}
namespace math {
Vector LoadVector(float x, float y, float z, float w) { return {x, y, z, w}; }
Vector LoadVector(const Vector &v) { return v; }
Vector LoadVector(const Vector3 &v, float w) { return {v.x, v.y, v.z, w}; }
Vector LoadVector(const Vector2 &v) { return {v.x, v.y, 0, 0}; }
Vector LoadScalar(float value) { return {value, value, value, value}; }
Vector GetVector4(const Vector &v) { return v; }
float GetScalar(const Vector &v) { return v.x; }
Vector Add(const Vector &a, const Vector &b) { return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
Vector Sub(const Vector &a, const Vector &b) { return {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; }
Vector Mul(const Vector &a, const Vector &b) { return {a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w}; }
Vector Dot(const Vector &a, const Vector &b) {
    // Retain the original SSE pairwise reduction, including the fourth component.
    return LoadScalar((a.y*b.y + a.w*b.w) + (a.x*b.x + a.z*b.z));
}
Vector Cross(const Vector &a, const Vector &b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x, 0};
}
Vector Magnitude(const Vector &v) { return LoadScalar(std::sqrt(Dot(v, v).x)); }
Vector Normalize(const Vector &v) {
    const float length = Magnitude(v).x;
    return {v.x/length, v.y/length, v.z/length, v.w/length};
}
Vector ExtendVector(const Vector &v) { return {v.x, v.y, v.z, 1}; }
Matrix LoadIdentity() { return {{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}}}; }
Matrix LoadMatrix(const Vector &a, const Vector &b, const Vector &c, const Vector &d) { return {{a,b,c,d}}; }
Matrix Transpose(const Matrix &m) {
    Matrix result;
    for (unsigned i=0; i<4; ++i) for (unsigned j=0; j<4; ++j) result.rows[i][j] = m.rows[j][i];
    return result;
}
Vector MatMult(const Matrix &m, const Vector &v) {
    Vector result;
    for (unsigned i=0; i<4; ++i) {
        float value = m.rows[i][0] * v[0];
        for (unsigned j=1; j<4; ++j) value = m.rows[i][j] * v[j] + value;
        result[i] = value;
    }
    return result;
}
Matrix MatMult(const Matrix &a, const Matrix &b) {
    Matrix result;
    for (unsigned i=0; i<4; ++i) {
        result.rows[i] = Mul(LoadScalar(a.rows[i][0]), b.rows[0]);
        for (unsigned j=1; j<4; ++j) result.rows[i] = Add(Mul(LoadScalar(a.rows[i][j]), b.rows[j]), result.rows[i]);
    }
    return result;
}
Matrix TranslationTransform(const Vector &v) {
    auto result = LoadIdentity(); result.rows[0].w=v.x; result.rows[1].w=v.y; result.rows[2].w=v.z; return result;
}
Matrix ScaleTransform(const Vector &v) { return {{{v.x,0,0,0}, {0,v.y,0,0}, {0,0,v.z,0}, {0,0,0,1}}}; }
Matrix RotationTransform(const Vector &axis, float angle) {
    const auto n = Normalize(axis);
    const float ux=n.x, uy=n.y, uz=n.z, cos_a=std::cos(angle), sin_a=std::sin(angle);
    return {{{cos_a+ux*ux*(1-cos_a), ux*uy*(1-cos_a)-uz*sin_a, ux*uz*(1-cos_a)+uy*sin_a, 0},
        {uy*ux*(1-cos_a)+uz*sin_a, cos_a+uy*uy*(1-cos_a), uy*uz*(1-cos_a)-ux*sin_a, 0},
        {uz*ux*(1-cos_a)-uy*sin_a, uz*uy*(1-cos_a)+ux*sin_a, cos_a+uz*uz*(1-cos_a), 0}, {0,0,0,1}}};
}
Matrix OrthographicProjection(float width, float height, float near, float far) {
    const float range = 1.0f/(far-near);
    return {{{2.0f/width,0,0,0}, {0,2.0f/height,0,0}, {0,0,2*range,0}, {0,0,-range*near,1}}};
}
Matrix CameraTarget(const Vector &eye, const Vector &target, const Vector &up) {
    auto r2=Normalize(Sub(target,eye)), r0=Normalize(Cross(r2,up)), r1=Cross(r0,r2);
    const auto negative=Mul(eye, LoadScalar(-1));
    r0.w=Dot(r0,negative).x; r1.w=Dot(r1,negative).x; r2.w=Dot(r2,negative).x;
    return Transpose({{r0,r1,r2,{0,0,0,1}}});
}
}
namespace color {
namespace {
float gamma(float value) {
    if (value<0.0031308f) return 12.92f*value;
    // The original global pow call widens its float inputs and the following arithmetic.
    return static_cast<float>(1.055f*std::pow(static_cast<double>(value),static_cast<double>(1/2.4f))-0.055f);
}
float inverse(float value) {
    if (value<0.04045f) return value/12.92f;
    return static_cast<float>(std::pow(static_cast<double>((value+0.055f)/1.055f),static_cast<double>(2.4f)));
}
}
Vector srgbiToLinear(unsigned int rgb, float alpha) {
    return {inverse(((rgb>>16)&255)/255.0f), inverse(((rgb>>8)&255)/255.0f), inverse((rgb&255)/255.0f), inverse(alpha)};
}
Vector srgbiToLinear(int r, int g, int b, int a) {
    return {inverse(r/255.0f), inverse(g/255.0f), inverse(b/255.0f), inverse(a/255.0f)};
}
Vector linearToSrgb(const Vector &value) { return {gamma(value.x),gamma(value.y),gamma(value.z),gamma(value.w)}; }
}
}
