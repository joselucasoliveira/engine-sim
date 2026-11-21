#include "../include/ui_utilities.h"

desktop::Vector mix(const desktop::Vector &c1, const desktop::Vector &c2, float s) {
    return desktop::math::Add(
            desktop::math::Mul(c1, desktop::math::LoadScalar(1 - s)),
            desktop::math::Mul(c2, desktop::math::LoadScalar(s)));
}
