#ifndef ENGINE_SIM_DESKTOP_MATH_H
#define ENGINE_SIM_DESKTOP_MATH_H

// Internal widget math. Conventions adapted from Delta Studio's yds_math/yds_color,
// Copyright (c) 2020 Ange Yaghi, MIT; see THIRD_PARTY_NOTICES.md.
namespace desktop {
struct Vector2 {
    float x = 0, y = 0;
    Vector2() = default;
    Vector2(float x, float y) : x(x), y(y) {}
};
struct Vector3 {
    float x = 0, y = 0, z = 0;
    Vector3() = default;
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
};
struct Vector {
    float x = 0, y = 0, z = 0, w = 0;
    Vector() = default;
    Vector(float x, float y, float z = 0, float w = 0) : x(x), y(y), z(z), w(w) {}
    void Set(float xValue, float yValue, float zValue, float wValue) {
        x=xValue; y=yValue; z=zValue; w=wValue;
    }
    float &operator[](unsigned index);
    float operator[](unsigned index) const;
};
using Vector4 = Vector;
struct Matrix { Vector rows[4]; };
namespace math {
namespace Constants {
inline constexpr float PI = 3.141592654f;
inline constexpr float TWO_PI = 6.2831853071795864769252866f;
inline const Vector XAxis{1, 0, 0, 0}, YAxis{0, 1, 0, 0}, ZAxis{0, 0, 1, 0}, One{1, 1, 1, 1};
}
Vector LoadVector(float x = 0, float y = 0, float z = 0, float w = 0);
Vector LoadVector(const Vector &v);
Vector LoadVector(const Vector3 &v, float w = 0);
Vector LoadVector(const Vector2 &v);
Vector LoadScalar(float value);
Vector GetVector4(const Vector &v);
float GetScalar(const Vector &v);
Vector Add(const Vector &a, const Vector &b);
Vector Sub(const Vector &a, const Vector &b);
Vector Mul(const Vector &a, const Vector &b);
Vector Dot(const Vector &a, const Vector &b);
Vector Cross(const Vector &a, const Vector &b);
Vector Magnitude(const Vector &v);
Vector Normalize(const Vector &v);
Vector ExtendVector(const Vector &v);
Matrix LoadIdentity();
Matrix LoadMatrix(const Vector &a, const Vector &b, const Vector &c, const Vector &d);
Matrix Transpose(const Matrix &m);
Vector MatMult(const Matrix &m, const Vector &v);
Matrix MatMult(const Matrix &a, const Matrix &b);
Matrix TranslationTransform(const Vector &translation);
Matrix ScaleTransform(const Vector &scale);
Matrix RotationTransform(const Vector &axis, float angle);
Matrix OrthographicProjection(float width, float height, float near, float far);
Matrix CameraTarget(const Vector &eye, const Vector &target, const Vector &up);
}
namespace color {
Vector srgbiToLinear(unsigned int rgb, float alpha = 1);
Vector srgbiToLinear(int r, int g, int b, int a = 255);
Vector linearToSrgb(const Vector &value);
}
}
#endif
