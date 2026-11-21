#include <gtest/gtest.h>
#include "compiler.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>

bool placeRod(const ConnectingRod &, const CylinderBank &, const Crankshaft &, double,
              double *, double *, double *, double *);

namespace {
struct Pose {
    double pinX, pinY, travel, ux, uy;
};

Pose independentPose(const ConnectingRod &rod, double angle) {
    const auto &bank = *rod.getPiston()->getCylinderBank();
    double x, y, pinX, pinY;
    if (auto *master = rod.getMasterRod()) {
        const auto parent = independentPose(*master, angle);
        master->getRodJournalPositionLocal(rod.getJournal(), &x, &y);
        y -= master->getBigEndLocal();
        pinX = parent.pinX + x * parent.uy + y * parent.ux;
        pinY = parent.pinY - x * parent.ux + y * parent.uy;
    }
    else {
        auto *shaft = rod.getCrankshaft();
        const double joint = angle + shaft->getRodJournalAngle(rod.getJournal());
        pinX = shaft->getPosX() + shaft->getThrow() * std::cos(joint);
        pinY = shaft->getPosY() + shaft->getThrow() * std::sin(joint);
    }
    const double dx = pinX - bank.getX(), dy = pinY - bank.getY();
    const double along = dx * bank.getDx() + dy * bank.getDy();
    const double normal = dx * bank.getDy() - dy * bank.getDx();
    const double travel = along + std::sqrt(rod.getLength() * rod.getLength() - normal * normal);
    return {pinX, pinY, travel, (travel * bank.getDx() - dx) / rod.getLength(),
            (travel * bank.getDy() - dy) / rod.getLength()};
}

void checkPose(const ConnectingRod &rod, double angle) {
    const auto expected = independentPose(rod, angle);
    double x, y, orientation, travel;
    ASSERT_TRUE(placeRod(rod, *rod.getPiston()->getCylinderBank(), *rod.getCrankshaft(),
                         angle, &x, &y, &orientation, &travel));
    EXPECT_NEAR(x, expected.pinX, 1e-13);
    EXPECT_NEAR(y, expected.pinY, 1e-13);
    EXPECT_NEAR(travel, expected.travel, 1e-13);
    // The rod's local +Y axis joins the crankpin and wristpin.
    EXPECT_NEAR(-std::sin(orientation), expected.ux, 1e-12);
    EXPECT_NEAR(std::cos(orientation), expected.uy, 1e-12);
}

TEST(Displacement, PermutedJournalSliderAndInvalidGraphs) {
    Engine engine;
    Engine::Parameters params{};
    params.cylinderBanks = params.crankshaftCount = 1;
    params.cylinderCount = 2;
    engine.initialize(params);
    Crankshaft::Parameters shaft{};
    shaft.crankThrow = 0.04;
    shaft.rodJournals = 2;
    engine.getCrankshaft(0)->initialize(shaft);
    engine.getCrankshaft(0)->setRodJournalAngle(0, 0);
    engine.getCrankshaft(0)->setRodJournalAngle(1, constants::pi / 2);
    CylinderBank::Parameters bank{};
    bank.cylinderCount = 2;
    bank.bore = 0.08;
    engine.getCylinderBank(0)->initialize(bank);
    for (int i = 0; i < 2; ++i) {
        Piston::Parameters piston{};
        piston.Bank = engine.getCylinderBank(0);
        piston.Rod = engine.getConnectingRod(i);
        piston.CylinderIndex = i;
        engine.getPiston(i)->initialize(piston);
        ConnectingRod::Parameters rod;
        rod.piston = engine.getPiston(i);
        rod.crankshaft = engine.getCrankshaft(0);
        rod.length = 0.15;
        rod.centerOfMass = 0.01;
        rod.journal = 1 - i;
        engine.getConnectingRod(i)->initialize(rod);
        for (int angle = 0; angle < 16; ++angle) checkPose(*engine.getConnectingRod(i), angle * constants::pi / 8);
    }
    engine.calculateDisplacement();
    const double analytic = 2 * bank.cylinderCount * shaft.crankThrow * constants::pi * bank.bore * bank.bore / 4;
    EXPECT_NEAR(engine.getDisplacement(), analytic, 1e-14);

    engine.getConnectingRod(0)->setMaster(engine.getConnectingRod(0));
    EXPECT_THROW(engine.calculateDisplacement(), std::runtime_error);
    engine.getConnectingRod(0)->setMaster(nullptr);
    ConnectingRod::Parameters invalid;
    invalid.piston = engine.getPiston(0);
    invalid.crankshaft = engine.getCrankshaft(0);
    invalid.length = 0.001;
    engine.getConnectingRod(0)->initialize(invalid);
    EXPECT_THROW(engine.calculateDisplacement(), std::runtime_error);
}

TEST(Displacement, SuppliedRadialMatchesIndependentMasterJoints) {
    const char *resourcePath = std::getenv("ENGINE_SIM_TEST_RESOURCES");
    ASSERT_NE(resourcePath, nullptr);
    const std::filesystem::path resources(resourcePath);
    const auto entry = std::filesystem::current_path() / "radial-displacement.mr";
    {
        std::ofstream script(entry);
        script << "import \"engine_sim.mr\"\nimport \"engines/atg-video-2/09_radial_9.mr\"\nmain()\n";
    }
    es_script::Compiler compiler;
    compiler.initialize({(resources / "es").string(), (resources / "assets").string()});
    std::ostringstream diagnostic;
    ASSERT_TRUE(compiler.compile(entry.string(), diagnostic)) << diagnostic.str();
    auto output = compiler.execute();
    compiler.destroy();
    std::unique_ptr<Engine> engine(std::move(output.engine));
    std::unique_ptr<Vehicle> vehicle(std::move(output.vehicle));
    std::unique_ptr<Transmission> transmission(std::move(output.transmission));
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(engine->getCylinderCount(), 9);
    double coarseVolume = 0, fineVolume = 0, independentSamplingBound = 0;
    for (int cylinder = 0; cylinder < 9; ++cylinder) {
        const auto &rod = *engine->getConnectingRod(cylinder);
        double coarseMin = std::numeric_limits<double>::infinity();
        double coarseMax = -coarseMin;
        double fineMin = coarseMin, fineMax = coarseMax;
        double intervalMin = coarseMin, intervalMax = coarseMax, largestIntervalSpan = 0;
        for (int point = 0; point < 20000; ++point) {
            const double angle = 2 * constants::pi * point / 20000;
            const double travel = independentPose(rod, angle).travel;
            ASSERT_TRUE(std::isfinite(travel));
            fineMin = std::min(fineMin, travel);
            fineMax = std::max(fineMax, travel);
            intervalMin = std::min(intervalMin, travel);
            intervalMax = std::max(intervalMax, travel);
            if (point % 20 == 0) {
                coarseMin = std::min(coarseMin, travel);
                coarseMax = std::max(coarseMax, travel);
                if (point % 1000 == 0) checkPose(rod, angle);
            }
            if (point % 20 == 19) {
                largestIntervalSpan = std::max(largestIntervalSpan, intervalMax - intervalMin);
                intervalMin = std::numeric_limits<double>::infinity();
                intervalMax = -intervalMin;
            }
        }
        const double area = rod.getPiston()->getCylinderBank()->boreSurfaceArea();
        coarseVolume += (coarseMax - coarseMin) * area;
        fineVolume += (fineMax - fineMin) * area;
        // Each coarse cell contains its 20 fine observations. Both extrema can move by this span.
        independentSamplingBound += 2 * largestIntervalSpan * area;
    }
    engine->calculateDisplacement();
    EXPECT_NEAR(coarseVolume, 0.0159393382740523, 1e-15);
    EXPECT_NEAR(engine->getDisplacement(), coarseVolume, 1e-14);
    EXPECT_GE(fineVolume, coarseVolume - 1e-14);
    EXPECT_NEAR(engine->getDisplacement(), fineVolume, independentSamplingBound + 1e-14);
    std::ostringstream fineValue, samplingBound;
    fineValue << std::setprecision(17) << fineVolume;
    samplingBound << std::setprecision(17) << independentSamplingBound;
    RecordProperty("independent_fine_volume_m3", fineValue.str());
    RecordProperty("independent_sampling_bound_m3", samplingBound.str());
    std::filesystem::remove(entry);
}
}
