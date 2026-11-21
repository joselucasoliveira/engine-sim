#include <gtest/gtest.h>

#include "../include/function.h"

#include <cmath>
#include <cstdlib>
#include <limits>

TEST(FunctionTests, FunctionSanityCheck) {
    Function f;
    f.initialize(16, 1.0);
    f.destroy();
}

TEST(FunctionTests, FunctionTriangleFilterTest) {
    Function f;
    f.initialize(0, 1.0);
    for (int i = 0; i < 10; ++i) {
        f.addSample((double)i, (double)i * 2);
    }

    EXPECT_NEAR(f.sampleTriangle(-1.0), 0.0, 1E-6);
    EXPECT_NEAR(f.sampleTriangle(11.0), 18.0, 1E-6);

    for (int i = 0; i < 10; ++i) {
        EXPECT_NEAR(f.sampleTriangle((double)i), (double)i * 2, 1E-6);
    }

    f.destroy();
}

TEST(FunctionTests, FunctionClosestTest) {
    Function f;
    f.initialize(0, 1.0);
    f.addSample(0.0, 1.0);
    f.addSample(2.0, 1.0);
    f.addSample(3.0, 1.1);
    f.addSample(1.0, 1.0);
    f.addSample(5.0, 10.0);
    f.addSample(4.0, 9.0);

    EXPECT_EQ(f.closestSample(2.4), 2);
    EXPECT_EQ(f.closestSample(6.0), 5);

    f.destroy();
}

TEST(FunctionTests, FunctionRandomAddTest) {
    Function f;
    f.initialize(0, 1.0);

    for (int i = 0; i < 1000; ++i) {
        f.addSample(rand() % 1000, i);
    }

    EXPECT_TRUE(f.isOrdered());

    f.destroy();
}

TEST(FunctionTests, FunctionGaussianTest) {
    constexpr double samples[] = {1.0, 1.0, 1.0, 5.0, 9.0, 10.0};
    constexpr double radius = 3.0;
    constexpr double step = radius / (1024 - 32);
    // The compact kernel subtracts its value at the boundary. In its support,
    // |d² exp(-x²)/dx²| <= 2, so linear interpolation perturbs each weight
    // by at most 2 * step² / 8.
    constexpr double weightError = step * step / 4.0;
    const double cutoff = std::exp(-radius * radius);

    Function f, flat;
    f.initialize(0, 1.0);
    flat.initialize(0, 1.0);
    for (int i : {0, 2, 3, 1, 5, 4}) {
        f.addSample(i, samples[i]);
        flat.addSample(i, 7.0);
    }

    for (double x : {2.0, 2.25, 4.0}) {
        double totalWeight = 0.0;
        double total = 0.0;
        int neighbors = 0;
        for (int i = 0; i < 6; ++i) {
            const double distance = i - x;
            if (std::abs(distance) > radius) continue;
            const double weight = std::exp(-distance * distance) - cutoff;
            totalWeight += weight;
            total += weight * samples[i];
            ++neighbors;
        }
        ASSERT_GT(totalWeight, neighbors * weightError);
        const double expected = total / totalWeight;
        double residual = 0.0;
        for (int i = 0; i < 6; ++i) {
            if (std::abs(i - x) <= radius) residual += std::abs(samples[i] - expected);
        }
        const double normalizedError = residual * weightError / (totalWeight - neighbors * weightError);
        const double rounding = 16 * std::numeric_limits<double>::epsilon() * expected;
        EXPECT_NEAR(f.sampleGaussian(x), expected, normalizedError + rounding) << x;
        EXPECT_DOUBLE_EQ(flat.sampleGaussian(x), 7.0) << x;
    }
    EXPECT_DOUBLE_EQ(f.sampleGaussian(100.0), samples[5]);
    EXPECT_DOUBLE_EQ(f.sampleGaussian(-100.0), samples[0]);

    GaussianFilter kernel;
    kernel.initialize(1.0, radius);
    EXPECT_DOUBLE_EQ(kernel.evaluate(0.42), kernel.evaluate(-0.42));
    EXPECT_DOUBLE_EQ(kernel.evaluate(radius), 0.0);
    flat.destroy();
    f.destroy();
}
