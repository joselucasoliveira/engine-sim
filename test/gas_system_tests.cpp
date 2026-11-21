#include <gtest/gtest.h>

#include "../include/gas_system.h"
#include "../include/units.h"
#include <cmath>

namespace {
void expectFiniteGas(const GasSystem &gas) {
    EXPECT_GT(gas.volume(), 0.0);
    EXPECT_GT(gas.pressure(), 0.0);
    EXPECT_GT(gas.temperature(), 0.0);
    EXPECT_GT(gas.n(), 0.0);
    EXPECT_TRUE(std::isfinite(gas.volume()));
    EXPECT_TRUE(std::isfinite(gas.pressure()));
    EXPECT_TRUE(std::isfinite(gas.temperature()));
    EXPECT_TRUE(std::isfinite(gas.n()));
    EXPECT_TRUE(std::isfinite(gas.totalEnergy()));
    EXPECT_TRUE(std::isfinite(gas.velocity_x()));
    EXPECT_TRUE(std::isfinite(gas.velocity_y()));
}

void transferAtEquilibrium(GasSystem &donor, GasSystem &recipient, double moles) {
    const double initialMoles = donor.n() + recipient.n();
    const double initialEnergy = donor.totalEnergy() + recipient.totalEnergy();
    const double energyPerMole = donor.kineticEnergyPerMol();
    const auto mix = donor.mix();

    donor.loseN(moles, energyPerMole);
    recipient.gainN(moles, energyPerMole, mix);

    EXPECT_NEAR(donor.pressure(), recipient.pressure(), 1E-6);
    EXPECT_NEAR(donor.n() + recipient.n(), initialMoles, 1E-6);
    EXPECT_NEAR(donor.totalEnergy() + recipient.totalEnergy(), initialEnergy, 1E-4);
}
}

TEST(GasSystemTests, GasSystemSanity) {
    GasSystem system;
    system.initialize(0.0, 0.0, 0.0);
}

TEST(GasSystemTests, AdiabaticEnergyConservation) {
    constexpr double pistonArea = units::area(1.0, units::cm2);
    constexpr double vesselHeight = units::distance(1.0, units::cm);
    const double compression = vesselHeight * 0.5;
    const int steps = 10000;

    GasSystem system;
    system.initialize(
        units::pressure(1.0, units::atm),
        units::volume(1.0, units::cc),
        units::celcius(25.0)
    );

    const double initialSystemEnergy = system.kineticEnergy();
    const double initialMolecules = system.n();

    double W = 0.0;
    double currentPistonHeight = vesselHeight;
    for (int i = 1; i <= steps; ++i) {
        const double newPistonHeight = vesselHeight - (compression / steps) * i;
        const double dH = (currentPistonHeight - newPistonHeight);
        const double F = system.pressure() * pistonArea;
        W += F * dH;

        system.changeVolume((newPistonHeight - currentPistonHeight) * pistonArea);

        currentPistonHeight = newPistonHeight;
    }

    const double finalSystemEnergy = system.kineticEnergy();
    const double finalMolecules = system.n();

    EXPECT_NEAR(finalMolecules, initialMolecules, 1E-6);
    EXPECT_NEAR(finalSystemEnergy - initialSystemEnergy, W, 1E-4);
}

TEST(GasSystemTests, PressureEqualizationEnergyConservation) {
    GasSystem system1, system2;
    system1.initialize(
        units::pressure(1.0, units::atm),
        units::volume(1000.0, units::cc),
        units::celcius(25.0)
    );

    system2.initialize(
        units::pressure(2.0, units::atm),
        units::volume(1000.0, units::cc),
        units::celcius(25.0)
    );

    const double initialSystemEnergy = system1.totalEnergy() + system2.totalEnergy();
    const double initialMolecules = system1.n() + system2.n();

    GasSystem::FlowParameters params;
    params.k_flow = 0.000001;
    params.crossSectionArea_0 = 1.0;
    params.crossSectionArea_1 = 1.0;
    params.direction_x = 1.0;
    params.direction_y = 0.0;
    params.dt = 1.0;
    params.system_0 = &system1;
    params.system_1 = &system2;

    const double dt = 1 / 100.0;
    const int steps = 1000;
    for (int i = 1; i <= steps; ++i) {
        GasSystem::flow(params);
    }

    const double finalSystemEnergy = system1.totalEnergy() + system2.totalEnergy();
    const double finalMolecules = system1.n() + system2.n();

    const double p0 = system1.pressure();
    const double p1 = system2.pressure();

    EXPECT_NEAR(finalMolecules, initialMolecules, 1E-6);
    EXPECT_NEAR(finalSystemEnergy, initialSystemEnergy, 1E-4);
}

TEST(GasSystemTests, PressureEquilibriumMaxFlow) {
    GasSystem system1, system2;
    system1.initialize(
        units::pressure(1.0, units::atm),
        units::volume(1.0, units::cc),
        units::celcius(2500.0)
    );

    system2.initialize(
        units::pressure(2.0, units::atm),
        units::volume(1.0, units::cc),
        units::celcius(25.0)
    );

    const double maxFlowIn = system1.pressureEquilibriumMaxFlow(&system2);
    ASSERT_LT(maxFlowIn, 0.0);
    transferAtEquilibrium(system2, system1, -maxFlowIn);

    system1.changePressure(units::pressure(100.0, units::atm));
    const double maxFlowOut = system1.pressureEquilibriumMaxFlow(&system2);
    ASSERT_GT(maxFlowOut, 0.0);
    transferAtEquilibrium(system1, system2, maxFlowOut);
}

TEST(GasSystemTests, PressureEquilibriumMaxFlowInfinite) {
    GasSystem system1;
    system1.initialize(
        units::pressure(1.0, units::atm),
        units::volume(1.0, units::cc),
        units::celcius(25.0)
    );

    constexpr double P_env = units::pressure(2.0, units::atm);
    constexpr double T_env = units::celcius(25.0);

    const double maxFlow = system1.pressureEquilibriumMaxFlow(P_env, T_env);
    const double E_k_per_mol = GasSystem::kineticEnergyPerMol(T_env, system1.degreesOfFreedom());

    ASSERT_LT(maxFlow, 0.0);
    system1.gainN(-maxFlow, E_k_per_mol);

    EXPECT_NEAR(system1.pressure(), P_env, 1E-6);
}

TEST(GasSystemTests, PressureEquilibriumMaxFlowInfiniteOverpressure) {
    GasSystem system1;
    system1.initialize(
        units::pressure(100.0, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(2500.0)
    );

    constexpr double P_env = units::pressure(2.0, units::atm);
    constexpr double T_env = units::celcius(25.0);

    const double maxFlow = system1.pressureEquilibriumMaxFlow(P_env, T_env);

    ASSERT_GT(maxFlow, 0.0);
    system1.loseN(maxFlow, system1.kineticEnergyPerMol());

    EXPECT_NEAR(system1.pressure(), P_env, 1E-6);
}

TEST(GasSystemTests, FlowVariableVolume) {
    GasSystem system1;
    system1.initialize(
        units::pressure(100.0, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(25.0)
    );

    constexpr double dV = units::volume(1.0, units::m3) / 100;
    constexpr double dt = 1.0 / 60.0;
    // Leave one increment of positive volume at the end of the stroke.
    for (int i = 0; i < 99; ++i) {
        SCOPED_TRACE(i);
        for (double pressure : {units::pressure(0.1, units::atm), units::pressure(0.2, units::atm)}) {
            const double pressureDifference = system1.pressure() - pressure;
            const double molesBefore = system1.n();
            const double flow = system1.flow(0.01, dt, pressure, units::celcius(25.0));
            EXPECT_GE(flow * pressureDifference, 0.0);
            EXPECT_NEAR(system1.n(), molesBefore - flow, 1E-6);
        }
        system1.changeVolume(-dV);
        system1.changeTemperature(100);
        expectFiniteGas(system1);
    }
    EXPECT_NEAR(system1.volume(), dV, 1E-12);
}

TEST(GasSystemTests, PowerStrokeTest) {
    GasSystem system1;
    system1.initialize(
        units::pressure(100.0, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(2000.0)
    );

    constexpr double ambientPressure = units::pressure(1.0, units::atm);
    const double initialMoles = system1.n();
    double discharged = 0.0;
    for (int i = 0; i < 100; ++i) {
        discharged += system1.flow(1.0, 1.0 / 60.0, ambientPressure, units::celcius(25.0));
        expectFiniteGas(system1);
    }
    EXPECT_GT(discharged, 0.0);
    EXPECT_NEAR(system1.n(), initialMoles - discharged, 1E-6);
    EXPECT_NEAR(system1.pressure(), ambientPressure, 1E-6);
}

TEST(GasSystemTests, IntakeStrokeTest) {
    GasSystem system1, system2;
    system1.initialize(
        units::pressure(1.0, units::atm),
        units::volume(1000.0, units::m3),
        units::celcius(25.0)
    );
    system2.initialize(
        units::pressure(1.0, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(25.0)
    );

    constexpr double dt = 0.01;
    system2.changeVolume(units::volume(4.0, units::m3));
    const double initialPressure = system2.pressure();
    const double initialCharge = system2.n();
    const double initialMoles = system1.n() + system2.n();
    const double initialEnergy = system1.totalEnergy() + system2.totalEnergy();

    const GasSystem::FlowParameters inlet{
        GasSystem::k_carb(100000.0), dt, 1.0, 0.0, 1.0, 1.0, &system1, &system2
    };
    EXPECT_GT(GasSystem::flow(inlet), 0.0);
    for (int i = 1; i < 100; ++i) {
        GasSystem::flow(inlet);
        expectFiniteGas(system2);
    }
    EXPECT_GT(system2.pressure(), initialPressure);
    EXPECT_GT(system2.n(), initialCharge);
    EXPECT_NEAR(system1.n() + system2.n(), initialMoles, 1E-6);
    EXPECT_NEAR(system1.totalEnergy() + system2.totalEnergy(), initialEnergy, 1E-4);
}

TEST(GasSystemTests, FlowLimit) {
    GasSystem system1;
    system1.initialize(
        units::pressure(100.0, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(2000.0)
    );

    constexpr double P_env = units::pressure(1.0, units::atm);
    constexpr double T_env = units::celcius(25.0);
    const double maxFlow = system1.pressureEquilibriumMaxFlow(P_env, T_env);

    system1.flow(15.0, 10.0, P_env, T_env);

    EXPECT_NEAR(system1.pressure(), P_env, 1E-6);
}

TEST(GasSystemTests, IdealGasLaw) {
    GasSystem system1;
    system1.initialize(
        units::pressure(100.0, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(2000.0)
    );

    const double PV = system1.pressure() * system1.volume();
    const double nRT = system1.n() * constants::R * system1.temperature();

    EXPECT_NEAR(PV, nRT, 1E-6);
}

TEST(GasSystemTests, CompositionSanityCheck) {
    GasSystem::Mix a, b;
    a.p_fuel = 1.0;
    a.p_inert = 1.0;
    a.p_o2 = 1.0;

    b.p_fuel = 0.0;
    b.p_inert = 0.0;
    b.p_o2 = 0.0;

    GasSystem system1;
    system1.initialize(
        units::pressure(100.0, units::atm),
        units::volume(100.0, units::m3),
        units::celcius(2000.0),
        a
    );

    GasSystem system2;
    system2.initialize(
        units::pressure(1.0, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(2000.0),
        b
    );

    const double PV = system1.pressure() * system1.volume();
    const double nRT = system1.n() * constants::R * system1.temperature();

    GasSystem::FlowParameters params;
    params.k_flow = 1.0;
    params.crossSectionArea_0 = 1.0;
    params.crossSectionArea_1 = 1.0;
    params.direction_x = 1.0;
    params.direction_y = 0.0;
    params.dt = 1 / 60.0;
    params.system_0 = &system1;
    params.system_1 = &system2;

    for (int i = 0; i < 200; ++i) {
        const double flowRate = GasSystem::flow(params);
        std::cerr << i << ", " << flowRate << ", " << system1.pressure() << "\n";
    }

    EXPECT_NEAR(system2.mix().p_fuel, 1.0, 2E-2);
    EXPECT_NEAR(system2.mix().p_inert, 1.0, 2E-2);
    EXPECT_NEAR(system2.mix().p_o2, 1.0, 2E-2);
}

TEST(GasSystemTests, ChokedFlowTest) {
    GasSystem system1;
    system1.initialize(
        units::pressure(2.5, units::atm),
        units::volume(1.0, units::m3),
        units::celcius(2000.0)
    );

    const double flow_k = GasSystem::flowConstant(
        units::flow(400, units::scfm),
        units::pressure(2.5, units::atm),
        units::pressure(1.5, units::atm),
        units::celcius(2000.0),
        GasSystem::heatCapacityRatio(5)
    );

    const double chokedFlow =
        system1.flowRate(
            flow_k,
            system1.pressure(),
            units::pressure(1.0, units::atm),
            system1.temperature(),
            units::celcius(25),
            GasSystem::heatCapacityRatio(5),
            GasSystem::chokedFlowLimit(5),
            GasSystem::chokedFlowRate(5));
    const double noncriticalFlow =
        system1.flowRate(
            flow_k,
            system1.pressure(),
            units::pressure(2.0, units::atm),
            system1.temperature(),
            units::celcius(25),
            GasSystem::heatCapacityRatio(5),
            GasSystem::chokedFlowLimit(5),
            GasSystem::chokedFlowRate(5));

    const double chokedFlowScfm = units::convert(chokedFlow, units::scfm);
    const double noncriticalFlowScfm = units::convert(noncriticalFlow, units::scfm);
}

TEST(GasSystemTests, CfmConversions) {
    constexpr double standardPressure = units::pressure(1.0, units::atm);
    constexpr double standardTemp = units::celcius(25.0);
    constexpr double airDensity =
        units::AirMolecularMass * (standardPressure * units::volume(1.0, units::m3))
        / (constants::R * standardTemp);

    const double flow_28 = GasSystem::k_28inH2O(300);
    
    const double flowRate = GasSystem::flowRate(
        flow_28,
        units::pressure(1.0, units::atm),
        units::pressure(1.0, units::atm) - units::pressure(41.0, units::inH2O),
        units::celcius(25.0),
        units::celcius(25.0),
        GasSystem::heatCapacityRatio(5),
        GasSystem::chokedFlowLimit(5),
        GasSystem::chokedFlowRate(5));

    const double flowRateCfm = units::convert(flowRate, units::scfm);
}

TEST(GasSystemTests, FlowRateConstant) {
    const double flow_k = GasSystem::flowConstant(
        units::flow(400, units::scfm),
        units::pressure(2.5, units::atm),
        units::pressure(0.5, units::atm),
        units::celcius(2000.0),
        GasSystem::heatCapacityRatio(5)
    );

    const double flowRate =
        GasSystem::flowRate(
            flow_k,
            units::pressure(2.5, units::atm),
            units::pressure(2.5 - 0.5, units::atm),
            units::celcius(2000.0),
            units::celcius(25),
            GasSystem::heatCapacityRatio(5),
            GasSystem::chokedFlowLimit(5),
            GasSystem::chokedFlowRate(5));

    EXPECT_NEAR(flowRate, units::flow(400, units::scfm), 1E-6);
}

TEST(GasSystemTests, GasVelocityReducesStaticPressure) {
    GasSystem system1, system2;
    system1.initialize(
        units::pressure(15, units::psi),
        units::volume(300, units::cc),
        units::celcius(25.0)
    );
    system1.setGeometry(units::distance(10, units::cm), units::distance(10, units::cm), 1.0, 0.0);

    system2.initialize(
        units::pressure(2, units::psi),
        units::volume(1.0, units::L),
        units::celcius(25.0)
    );
    system2.setGeometry(units::distance(10, units::cm), units::distance(2, units::cm), 1.0, 0.0);

    const double initialSystemEnergy = system1.totalEnergy() + system2.totalEnergy();
    const double initialMolecules = system1.n() + system2.n();

    GasSystem::FlowParameters params;
    params.k_flow = GasSystem::k_28inH2O(500.0) * 1.0;
    params.crossSectionArea_0 = 50.0 * units::cm * units::cm;
    params.crossSectionArea_1 = 4.0 * units::cm * units::cm;
    params.direction_x = 1.0;
    params.direction_y = 0.0;
    params.dt = 1 / 10000.0;
    params.system_0 = &system1;
    params.system_1 = &system2;

    double peakVelocity = 0.0;
    double peakDynamicPressure = 0.0;
    const int steps = 1000;
    for (int i = 1; i <= steps; ++i) {
        const double flow = GasSystem::flow(params);
        if (i == 1) EXPECT_GT(flow, 0.0);
        system1.updateVelocity(params.dt);
        system2.updateVelocity(params.dt);
        expectFiniteGas(system1);
        expectFiniteGas(system2);
        peakVelocity = std::fmax(peakVelocity, system2.velocity_x());
        peakDynamicPressure = std::fmax(peakDynamicPressure, system2.dynamicPressure(1.0, 0.0));
    }

    EXPECT_GT(peakVelocity, 0.0);
    EXPECT_GT(peakDynamicPressure, 0.0);
    EXPECT_NEAR(system1.n() + system2.n(), initialMolecules, 1E-6);
    EXPECT_NEAR(system1.totalEnergy() + system2.totalEnergy(), initialSystemEnergy, 1E-4);
}

TEST(GasSystemTests, GasVelocityProducesScavengingEffect) {
    constexpr double cylinderArea =
        constants::pi * units::distance(2.0, units::inch) * units::distance(2.0, units::inch);
    constexpr double tubeArea =
        constants::pi * units::distance(1.75 / 2, units::inch) * units::distance(1.75 / 2, units::inch);

    GasSystem system1, system2, atmosphere;
    system1.initialize(
        units::pressure(1000, units::psi),
        units::volume(1000, units::cc),
        units::celcius(1000.0)
    );
    system1.setGeometry(
        units::distance(10.0, units::cm),
        units::distance(1.0, units::cm),
        1.0,
        0.0);

    system2.initialize(
        units::pressure(15, units::psi),
        tubeArea * units::distance(50.0, units::inch),
        units::celcius(25.0)
    );
    system2.setGeometry(
        units::distance(50.0, units::inch),
        std::sqrt(tubeArea),
        1.0,
        0.0);

    atmosphere.initialize(
        units::pressure(15, units::psi),
        units::volume(10000, units::m3),
        units::celcius(25.0)
    );

    const double initialMoles = system1.n() + system2.n() + atmosphere.n();
    const double initialCylinderMoles = system1.n();
    const double initialAtmosphereMoles = atmosphere.n();

    const double atmosphereArea =
        1000.0;

    GasSystem::FlowParameters params;
    params.k_flow = GasSystem::k_28inH2O(230.0) * 1.0;        
    params.direction_x = 1.0;
    params.direction_y = 0.0;
    params.dt = 1 / (16 * 4000.0);
    params.system_0 = &system1;
    params.system_1 = &system2;

    double peakExhaustVelocity = 0.0;
    const int steps = 10000;
    for (int i = 1; i <= steps; ++i) {
        if (system1.volume() > units::volume(118.0, units::cc)) {
            system1.changeVolume(-units::volume(200000.0, units::cc) * params.dt);
        }
        else {
            system1.setVolume(units::volume(118.0, units::cc));
        }

        params.system_0 = &system1;
        params.system_1 = &system2;
        params.crossSectionArea_0 = cylinderArea;
        params.crossSectionArea_1 = tubeArea;
        params.k_flow = GasSystem::k_28inH2O(230.0);
        GasSystem::flow(params);

        params.system_0 = &system2;
        params.system_1 = &atmosphere;
        params.crossSectionArea_0 = tubeArea;
        params.crossSectionArea_1 = atmosphereArea;
        params.k_flow = GasSystem::k_carb(1000);
        GasSystem::flow(params);

        system1.updateVelocity(params.dt);
        system2.updateVelocity(params.dt);
        system1.dissipateExcessVelocity();
        system2.dissipateExcessVelocity();

        expectFiniteGas(system1);
        expectFiniteGas(system2);
        peakExhaustVelocity = std::fmax(peakExhaustVelocity, system2.velocity_x());
    }
    expectFiniteGas(atmosphere);
    EXPECT_GT(peakExhaustVelocity, 0.0);
    EXPECT_LT(system1.n(), initialCylinderMoles);
    EXPECT_GT(atmosphere.n(), initialAtmosphereMoles);
    EXPECT_NEAR(system1.n() + system2.n() + atmosphere.n(), initialMoles, 1E-6);
}


TEST(GasSystemTests, GasVelocityProducesRamEffect) {
    constexpr double cylinderArea =
        constants::pi * units::distance(2.0, units::inch) * units::distance(2.0, units::inch);
    constexpr double runnerArea =
        constants::pi * units::distance(0.5, units::inch) * units::distance(0.5, units::inch);

    GasSystem cylinder, runner, atmosphere;
    cylinder.initialize(
        units::pressure(1.0, units::atm),
        units::volume(118, units::cc),
        units::celcius(25.0)
    );
    cylinder.setGeometry(
        units::distance(10.0, units::cm),
        units::distance(1.0, units::cm),
        1.0,
        0.0);

    runner.initialize(
        units::pressure(1.0, units::atm),
        units::volume(320, units::cc),
        units::celcius(25.0)
    );
    runner.setGeometry(
        units::distance(5.0, units::inch),
        std::sqrt(runnerArea),
        1.0,
        0.0);

    atmosphere.initialize(
        units::pressure(1.0, units::atm),
        units::volume(10000, units::m3),
        units::celcius(25.0)
    );

    const double initialMoles = cylinder.n() + runner.n() + atmosphere.n();
    const double initialCylinderMoles = cylinder.n();

    const double atmosphereArea =
        1000.0;

    GasSystem::FlowParameters params;
    params.dt = 1 / (16 * 4000.0);
    params.direction_x = 1.0;
    params.direction_y = 0.0;

    constexpr double speed = 3000; // rpm
    constexpr double stroke = units::distance(4.0, units::inch);

    double peakCharge = 0.0;
    double peakRunnerVelocity = 0.0;
    const int steps = 10000;
    for (int i = 1; i <= steps; ++i) {
        const double t = i * params.dt;
        const double pistonHeight =
            units::distance(0.25, units::inch)
            + stroke / 2
            + (stroke / 2) * -std::cos(2 * constants::pi * (t * (speed / 60)));

        cylinder.setVolume(pistonHeight * cylinderArea);
        if (i == 1) {
            cylinder.changePressure(units::pressure(1.0, units::atm) - cylinder.pressure());
        }

        params.system_0 = &runner;
        params.system_1 = &cylinder;
        params.crossSectionArea_0 = runnerArea;
        params.crossSectionArea_1 = cylinderArea;
        params.k_flow = GasSystem::k_28inH2O(230.0);
        GasSystem::flow(params);

        params.system_0 = &atmosphere;
        params.system_1 = &runner;
        params.crossSectionArea_0 = atmosphereArea;
        params.crossSectionArea_1 = cylinderArea;
        params.k_flow = GasSystem::k_carb(500);
        GasSystem::flow(params);

        cylinder.updateVelocity(params.dt, 1.0);
        runner.updateVelocity(params.dt, 0.1);
        cylinder.dissipateExcessVelocity();
        runner.dissipateExcessVelocity();

        expectFiniteGas(cylinder);
        expectFiniteGas(runner);
        peakCharge = std::fmax(peakCharge, cylinder.n());
        peakRunnerVelocity = std::fmax(peakRunnerVelocity, runner.velocity_x());
    }
    expectFiniteGas(atmosphere);
    EXPECT_GT(peakCharge, initialCylinderMoles);
    EXPECT_GT(peakRunnerVelocity, 0.0);
    EXPECT_NEAR(cylinder.n() + runner.n() + atmosphere.n(), initialMoles, 1E-6);
}


TEST(GasSystemTests, GasVelocityStabilizesInClosedSystem) {
    GasSystem system1;
    system1.initialize(
        units::pressure(100, units::psi),
        units::volume(1000, units::cc),
        units::celcius(1000.0)
    );
    system1.setGeometry(
        units::distance(10.0, units::cm),
        units::distance(10.0, units::cm),
        1.0,
        0.0);

    const double initialSystemEnergy = system1.totalEnergy();
    const double initialMolecules = system1.n();

    const int steps = 10000;
    const double dt = 1 / 10000.0;
    for (int i = 1; i <= steps; ++i) {
        system1.updateVelocity(dt);
        EXPECT_DOUBLE_EQ(system1.velocity_x(), 0.0);
        EXPECT_DOUBLE_EQ(system1.velocity_y(), 0.0);
    }
    expectFiniteGas(system1);
    EXPECT_DOUBLE_EQ(system1.totalEnergy(), initialSystemEnergy);
    EXPECT_DOUBLE_EQ(system1.n(), initialMolecules);
}
