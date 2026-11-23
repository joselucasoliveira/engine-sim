#include "definition.hpp"
#include "engine_session.h"

#include "piston_engine_simulator.h"

#include <engine_sim/config-json.hpp>

#include <cmath>
#include <fstream>
#include <iostream>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace assembly = engine_sim::assembly;

struct IgnitionAccess : IgnitionModule {
    static constexpr auto plug = &IgnitionAccess::getPlug;
    static constexpr auto phase = &IgnitionAccess::m_lastCrankshaftAngle;
};
struct ChamberAccess : CombustionChamber {
    static constexpr auto random = &ChamberAccess::m_random;
};
struct TransmissionAccess : Transmission {
    static constexpr auto ratios = &TransmissionAccess::m_gearRatios;
    static constexpr auto clutch_torque = &TransmissionAccess::m_maxClutchTorque;
};

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, const char *message) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-9, message);
}

cfg::NormalizedDocument normalized(cfg::ModelResult result) {
    if (const auto *problem = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(problem->code + " " + problem->pointer + " " + problem->message);
    return std::get<cfg::NormalizedDocument>(std::move(result));
}

cfg::Document read(const char *path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto extent = file.tellg();
    require(extent > 0 && extent < 16 * 1024 * 1024, "Cannot open simulation assembly fixture");
    std::string contents(static_cast<std::size_t>(extent), '\0');
    file.seekg(0);
    require(static_cast<bool>(file.read(contents.data(), static_cast<std::streamsize>(contents.size()))),
            "Cannot read complete simulation assembly fixture");
    return normalized(cfg::read_json(contents)).values();
}

EngineDefinition build(const cfg::NormalizedDocument &model, std::uint64_t seed = 403) {
    auto result = assembly::build_definition(model, seed, "simulation-assembly-fixture");
    if (const auto *problem = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(problem->code + " " + problem->pointer + " " + problem->message);
    return std::get<EngineDefinition>(std::move(result));
}

template<class Record>
Record different(Record source, const char *id) {
    source.id.value = id;
    return source;
}

cfg::Document with_second_bank(cfg::Document model, bool share_exhaust, bool share_impulse) {
    auto &p = model.components;
    p.pistons.push_back(different(p.pistons.front(), "piston-b"));
    p.rods.push_back(different(p.rods.front(), "rod-b"));
    auto journal = different(p.journals.front(), "journal-b");
    journal.angle = {0.3125};
    p.journals.push_back(journal);
    p.crankshafts.front().journals.push_back(journal.id);
    auto second = different(p.banks.front(), "bank-b");
    second.angle = {0.375};
    second.head = different(p.heads.front(), "head-b").id;
    p.heads.push_back(different(p.heads.front(), "head-b"));
    auto &cylinder = second.cylinders.front();
    cylinder.rod_journal = journal.id;
    cylinder.piston = p.pistons.back().id;
    cylinder.connecting_rod = p.rods.back().id;
    cylinder.sound_attenuation = {0.375};
    cylinder.primary_length = {0.25};
    if (!share_exhaust) {
        auto exhaust = different(p.exhausts.front(), "exhaust-b");
        exhaust.audio_volume = {0.875};
        if (!share_impulse) {
            auto impulse = different(p.impulses.front(), "response-b");
            impulse.path = "sub/alternate.wav";
            impulse.volume = {0.375};
            p.impulses.push_back(impulse);
            exhaust.impulse_response = impulse.id;
        }
        p.exhausts.push_back(exhaust);
        cylinder.exhaust_system = exhaust.id;
    }
    p.banks.push_back(second);
    model.engine.banks.insert(model.engine.banks.begin(), second.id);
    p.ignitions.front().posts.push_back({cylinder.ignition_wire, {0.875}});
    return model;
}

void routing(const cfg::Document &base) {
    for (const bool share_exhaust : {true, false}) {
        for (const bool share_impulse : {true, false}) {
            const auto model = normalized(cfg::normalize(with_second_bank(base, share_exhaust, share_impulse)));
            auto definition = build(model);
            auto &engine = *definition.engine;
            require(engine.getCylinderBankCount() == 2 && engine.getIntakeCount() == 1 &&
                    engine.getExhaustSystemCount() == (share_exhaust ? 1 : 2), "Gas slot counts differ");
            require(engine.getHead(0)->getIntake(0) == engine.getIntake(0) &&
                    engine.getHead(1)->getIntake(0) == engine.getIntake(0), "Intake sharing differs");
            require(engine.getHead(0)->getExhaustSystem(0) == engine.getExhaustSystem(0) &&
                    engine.getHead(1)->getExhaustSystem(0) == engine.getExhaustSystem(share_exhaust ? 0 : 1),
                    "First bank encounter did not determine exhaust order");
            const auto *response = engine.getExhaustSystem(0)->getImpulseResponse();
            const bool alternate = !share_exhaust && !share_impulse;
            require(response->getFilename() == (alternate ? "sub/alternate.wav" : "impulse.wav") &&
                    response->getVolume() == (alternate ? 0.375 : 0.01), "First response locator or gain differs");
            if (!share_exhaust) {
                const auto *other = engine.getExhaustSystem(1)->getImpulseResponse();
                require(other != response && other->getVolume() == 0.01 &&
                        engine.getExhaustSystem(0)->getAudioVolume() == 0.875,
                        "Distinct exhaust descriptors or gains were collapsed");
            }
            auto *spark = engine.getIgnitionModule();
            for (int i = 0; i < engine.getCylinderCount(); ++i) {
                const auto *plug = (spark->*IgnitionAccess::plug)(i);
                require(plug->enabled && plug->angle == 0.875, "Last ignition post did not win");
            }
            require(engine.getHead(0)->getHeaderPrimaryLength(0) == 0.25 &&
                    engine.getHead(0)->getSoundAttenuation(0) == 0.375, "Per-cylinder exhaust metadata differs");
            require(engine.getChamber(0)->m_meanPistonSpeedToTurbulence ==
                    engine.getChamber(1)->m_meanPistonSpeedToTurbulence, "Mean-speed curve was duplicated");
        }
    }
}

void controls(cfg::Document model) {
    auto &p = model.components;
    auto &low_cam = p.camshafts.front();
    low_cam.advance = {0.125};
    low_cam.lobes = {{0.75}};
    auto curve = p.functions.front();
    curve.filter_radius = {0.375};
    curve.samples = {{{1}, {3}}, {{0}, {0}}, {{1}, {5}}, {{1}, {7}}};
    p.functions.front() = curve;
    auto high = different(low_cam, "high");
    high.base_radius = {0.03125};
    high.lobe_profile = different(curve, "independent-lift").id;
    p.functions.push_back(different(curve, "independent-lift"));
    p.camshafts.push_back(high);
    const auto common = std::get<cfg::Standard>(p.valvetrains.front());
    cfg::Vtec train{};
    train.id = common.id;
    train.intake_camshaft = common.intake_camshaft;
    train.exhaust_camshaft = common.exhaust_camshaft;
    train.vtec_intake_camshaft = high.id;
    train.vtec_exhaust_camshaft = common.exhaust_camshaft;
    train.min_rpm = {150};
    train.min_speed = {7500};
    train.manifold_vacuum = {99000};
    train.min_throttle_position = {0.4375};
    p.valvetrains.front() = train;
    p.throttles.front() = cfg::Governor{model.engine.throttle, {40}, {400}, {-1.25}, {2.75}, {1.5}, {81.25}, {0.625}};
    p.fuels.front().energy_density = {51200000.0};
    p.ignitions.front().posts.clear();

    auto definition = build(normalized(cfg::normalize(std::move(model))));
    auto &engine = *definition.engine;
    auto *head = engine.getHead(0);
    auto *low_in = head->getIntakeCamshaft();
    auto *low_out = head->getExhaustCamshaft();
    require(low_in != low_out && low_in->getLobeProfile() == low_out->getLobeProfile(),
            "Repeated cam definitions did not create independent cams with a shared profile");
    require(low_in->getAdvance() == 0.125 && low_in->getLobeCenterline(0) == 0.375,
            "Cam advance or crank-to-cam lobe conversion differs");
    require(low_in->getLobeProfile()->sampleTriangle(1) == 7 &&
            low_in->getLobeProfile()->triangle(0.1875) == 0.5,
            "Curve insertion order, unit scale or radius differs");
    engine.setThrottle(0.8);
    engine.getOutputCrankshaft()->m_body.v_theta = -220;
    require(head->getIntakeCamshaft() == low_in, "VTEC bypassed its throttle threshold");
    engine.setThrottle(0.25);
    auto *high_in = head->getIntakeCamshaft();
    require(high_in != low_in && high_in->getBaseRadius() == 0.03125 &&
            high_in->getLobeProfile() != low_in->getLobeProfile() &&
            high_in->getLobeProfile()->sampleTriangle(1) == 7,
            "VTEC high branch or independent equal-valued profile differs");
    require(head->getExhaustCamshaft() != low_out, "Repeated high exhaust cam was shared");
    engine.getOutputCrankshaft()->m_body.v_theta = -140;
    require(head->getIntakeCamshaft() == low_in, "VTEC bypassed the crank speed threshold");
    engine.setSpeedControl(1.0);
    engine.update(0.125);
    near(engine.getThrottle(), 1 - std::pow(0.15625, 0.625), "Governor velocity clamp or gamma differs");
    require(engine.getFuel()->getEnergyDensity() == 51200000.0 &&
            !(engine.getIgnitionModule()->*IgnitionAccess::plug)(0)->enabled,
            "Fuel or default unconnected ignition differs");
}

std::unique_ptr<EngineSession> prepare(const cfg::NormalizedDocument &model, std::uint64_t seed) {
    return EngineSession::prepare(build(model, seed), [](const ImpulseResponse &) {
        return std::vector<std::int16_t>{1024, -512, 31};
    }, {12000, 0.0});
}

void initialization(cfg::Document fixture) {
    auto omitted = fixture;
    omitted.vehicle.reset();
    omitted.transmission.reset();
    auto defaults = build(normalized(cfg::normalize(std::move(omitted))));
    defaults.engine->setSpeedControl(0.375);
    defaults.engine->update(0.001);
    require(defaults.engine->getThrottle() == 0.625, "Direct throttle mapping differs");
    constexpr double default_ratios[]{2.97, 2.07, 1.43, 1, 0.84, 0.56};
    auto &default_drive = *defaults.transmission;
    require(defaults.vehicle->getMass() == 1597 && defaults.vehicle->getDiffRatio() == 3.42 &&
            default_drive.getGearCount() == 6 && default_drive.getGear() == -1 &&
            default_drive.getClutchPressure() == 0 && default_drive.*TransmissionAccess::clutch_torque == 1355.817456,
            "Default drivetrain differs");
    for (int i = 0; i < 6; ++i)
        require((default_drive.*TransmissionAccess::ratios)[i] == default_ratios[i], "Default gear order differs");

    auto dual = with_second_bank(fixture, false, false);
    dual.vehicle->mass = {1234.5};
    dual.vehicle->diff_ratio = {3.875};
    dual.transmission = cfg::Transmission{{560.25}, {{-2.5}, {0.9375}, {1.75}}};
    const auto model = normalized(cfg::normalize(std::move(dual)));
    auto active = prepare(model, 403);
    auto paired = prepare(model, 403);
    auto different_seed = prepare(model, 404);
    auto &engine = *active->definition.engine;
    auto &drive = *active->definition.transmission;
    require(active->definition.vehicle->getMass() == 1234.5 &&
            active->definition.vehicle->getDiffRatio() == 3.875 &&
            drive.getGearCount() == 3 && drive.getGear() == -1 && drive.getClutchPressure() == 0 &&
            drive.*TransmissionAccess::clutch_torque == 560.25, "Explicit drivetrain differs");
    const auto *ratios = drive.*TransmissionAccess::ratios;
    require(ratios[0] == -2.5 && ratios[1] == 0.9375 && ratios[2] == 1.75, "Explicit gear order or sign differs");

    const auto *placed = dynamic_cast<PistonEngineSimulator *>(active->simulator.get());
    require(placed && placed->getFluidSimulationSteps() == 8 && placed->getSimulationFrequency() == 12000 &&
            engine.getOutputCrankshaft()->m_body.v_theta == 0 &&
            engine.getIgnitionModule()->*IgnitionAccess::phase == engine.getOutputCrankshaft()->getCycleAngle(),
            "Initial solver frequency, phase or motion differs");
    for (int i = 0; i < engine.getCylinderCount(); ++i) {
        auto &chamber = *engine.getChamber(i);
        auto &other = *paired->definition.engine->getChamber(i);
        const auto &body = chamber.getPiston()->m_body;
        require(&chamber != &other && std::isfinite(body.p_x) && std::isfinite(body.p_y) &&
                body.p_x == other.getPiston()->m_body.p_x && body.p_y == other.getPiston()->m_body.p_y &&
                chamber.m_system.volume() == chamber.getVolume() && chamber.getVolume() > 0 &&
                !chamber.isLit() && !engine.getIgnitionModule()->getIgnitionEvent(i),
                "Initial solver did not place independent chambers at rest");
        near(chamber.m_system.pressure(), 101325.0, "Initial chamber pressure differs");
        near(chamber.m_system.temperature(), 298.15, "Initial chamber temperature differs");
        auto actual_stream = chamber.*ChamberAccess::random;
        auto paired_stream = other.*ChamberAccess::random;
        auto changed_stream = different_seed->definition.engine->getChamber(i)->*ChamberAccess::random;
        engine_sim::RandomStream expected(403, 1, static_cast<std::uint32_t>(i));
        const auto first = actual_stream.next();
        require(first == expected.next() && first == paired_stream.next() && first != changed_stream.next(),
                "Cylinder seed or stream index differs");
    }

    int requests = 0;
    try {
        (void)EngineSession::prepare(build(model), [&](const ImpulseResponse &) {
            if (++requests == 2) throw std::bad_alloc();
            return std::vector<std::int16_t>{901, -13};
        });
        throw std::logic_error("Partially initialized candidate was returned");
    } catch (const std::bad_alloc &) {}
    require(requests == 2 && engine.getChamber(0)->m_system.volume() ==
            paired->definition.engine->getChamber(0)->m_system.volume() &&
            prepare(model, 403)->definition.engine->getRandomSeed() == 403, "Resource failure corrupted another session");

    fixture.components.pistons.front().compression_height = {1.0};
    auto failed = assembly::build_definition(normalized(cfg::normalize(fixture)), 403);
    const auto &problem = std::get<cfg::Diagnostic>(failed);
    require(problem.code == "CFG_INITIALIZE" && problem.message == "cylinder 0 has nonpositive chamber volume",
            "Nonpositive geometry reached solver activation");
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto model = read(argv[2]);
        const std::string_view mode = argv[1];
        if (mode == "induction") {
            routing(model);
            controls(model);
        } else if (mode == "initialization") {
            initialization(model);
        } else {
            return 2;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
