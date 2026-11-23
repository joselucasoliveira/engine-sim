#include "mechanical.hpp"

#include "connecting_rod.h"
#include "constants.h"
#include "crankshaft.h"
#include "cylinder_bank.h"
#include "piston.h"

#include <engine_sim/config-json.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace assembly = engine_sim::assembly;

using Phase = assembly::MechanicalPhase;

void require(bool condition,const char *message) {
    if (!condition) throw std::runtime_error(message);
}

bool same(double left,double right) {
    std::uint64_t a,b;
    static_assert(sizeof a==sizeof left);
    std::memcpy(&a,&left,sizeof a);
    std::memcpy(&b,&right,sizeof b);
    return a==b;
}

cfg::Document read_fixture(const char *file) {
    std::ifstream stream(file,std::ios::binary|std::ios::ate);
    const auto size=stream.tellg();
    require(size>0 && size<16*1024*1024,"Unable to open the mechanical fixture");
    std::string bytes(static_cast<std::size_t>(size),'\0');
    stream.seekg(0);
    if (!stream.read(bytes.data(),static_cast<std::streamsize>(size)))
        throw std::runtime_error("Unable to read the complete mechanical fixture");
    const auto model=cfg::read_json(bytes);
    return std::get<cfg::NormalizedDocument>(model).values();
}

cfg::NormalizedDocument as_model(cfg::Document document) {
    auto model=cfg::normalize(std::move(document));
    if (const auto *problem=std::get_if<cfg::Diagnostic>(&model))
        throw std::runtime_error(problem->code+" "+problem->pointer+" "+problem->message);
    return std::get<cfg::NormalizedDocument>(std::move(model));
}

assembly::MechanicalCandidate build(cfg::NormalizedDocument model) {
    auto result=assembly::build_mechanics(std::move(model));
    if (const auto *problem=std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(problem->code+" "+problem->message);
    return std::get<assembly::MechanicalCandidate>(std::move(result));
}

template<class Record>
Record &second(std::vector<Record> &parts,std::string_view id) {
    auto copy=parts.at(0);
    copy.id.value=id;
    return parts.emplace_back(std::move(copy));
}

enum class Route { same_crank, different_cranks, later_master };

cfg::Document with_second_bank(cfg::Document document,Route route) {
    auto &parts=document.components;
    auto &bank=second(parts.banks,"second-bank");
    bank.bore={0.076};
    bank.deck_height={0.217};
    bank.angle={0.22};
    bank.position_x={0.002};
    bank.position_y={0.004};
    bank.head=second(parts.heads,"second-head").id;

    auto &cylinder=bank.cylinders.at(0);
    cylinder.piston=second(parts.pistons,"second-piston").id;
    cylinder.connecting_rod=second(parts.rods,"second-rod").id;
    auto &journal=second(parts.journals,"off-phase");
    journal.angle={0.625};
    cylinder.rod_journal=journal.id;
    document.engine.banks.insert(document.engine.banks.begin(),bank.id);
    if (route==Route::different_cranks) {
        auto &crankshaft=second(parts.crankshafts,"second-shaft");
        crankshaft.crank_throw={0.035};
        crankshaft.position_x={0.006};
        crankshaft.position_y={-0.002};
        crankshaft.tdc={0.375};
        crankshaft.mass={5.25};
        crankshaft.flywheel_mass={2.25};
        crankshaft.journals={journal.id};
        document.engine.crankshafts.push_back(crankshaft.id);
    }
    else if (route==Route::later_master) {
        parts.rods.at(0).slave_throw={0.012};
        parts.rods.at(0).journals={journal.id};
    }
    else parts.crankshafts.at(0).journals.push_back(journal.id);
    return document;
}

void identities(const assembly::MechanicalCandidate &candidate) {
    const auto &engine=*candidate.engine;
    const auto &model=candidate.model;
    const auto &routes=model.topology();
    require(engine.getCrankshaftCount()==static_cast<int>(routes.crankshafts.size()) &&
            engine.getCylinderBankCount()==static_cast<int>(routes.banks.size()) &&
            engine.getCylinderCount()==static_cast<int>(routes.cylinders.size()),
            "Engine counts differ from the validated topology");
    require(engine.getOutputCrankshaft()==engine.getCrankshaft(0) &&
            same(engine.getStarterSpeed(),model.values().engine.starter_speed.value) &&
            same(engine.getStarterTorque(),model.values().engine.starter_torque.value),
            "Output shaft or starting values differ");
    require(engine.getIntakeCount()==static_cast<int>(routes.intakes.size()) &&
            engine.getExhaustSystemCount()==static_cast<int>(routes.exhausts.size()) &&
            routes.wire_cylinders.at(0).size()==routes.cylinders.size(),
            "Ordered channels or ignition connections did not survive mechanics assembly");

    for (std::size_t i=0;i<routes.crankshafts.size();++i) {
        const auto &shaft=*engine.getCrankshaft(static_cast<int>(i));
        const auto &typed=model.values().components.crankshafts.at(routes.crankshafts[i]);
        require(same(shaft.getMass(),typed.mass.value) && same(shaft.getFlywheelMass(),typed.flywheel_mass.value) &&
                same(shaft.getThrow(),typed.crank_throw.value) &&
                same(shaft.getMomentOfInertia(),typed.moment_of_inertia.value) &&
                same(shaft.getPosX(),typed.position_x.value) && same(shaft.getPosY(),typed.position_y.value) &&
                same(shaft.getTdc(),typed.tdc.value) &&
                shaft.getRodJournalCount()==static_cast<int>(typed.journals.size()),
                "Crank geometry or inertia changed");
    }
    for (const auto &route:routes.cylinders) {
        const auto i=static_cast<int>(route.index.global);
        const auto *piston=engine.getPiston(i);
        const auto *rod=engine.getConnectingRod(i);
        require(piston->getCylinderBank()==engine.getCylinderBank(static_cast<int>(route.index.bank)) &&
                piston->getCylinderIndex()==static_cast<int>(route.index.local) &&
                piston->getRod()==rod && rod->getPiston()==piston &&
                rod->getCrankshaft()==engine.getCrankshaft(static_cast<int>(route.root_crank)),
                "Cylinder order or mutual linkage differs");
        if (const auto *master=std::get_if<cfg::MasterRodJournalIndex>(&route.index.journal)) {
            require(rod->getMasterRod()==engine.getConnectingRod(static_cast<int>(master->rod)) &&
                    rod->getJournal()==static_cast<int>(master->journal), "Slave journal index differs");
        }
        else {
            const auto direct=std::get<cfg::CrankJournalIndex>(route.index.journal);
            require(!rod->getMasterRod() && rod->getJournal()==static_cast<int>(direct.journal),
                    "Crank journal index differs");
        }
        double x,y,theta,travel;
        require(placeRod(*rod,*piston->getCylinderBank(),*rod->getCrankshaft(),0.2345,&x,&y,&theta,&travel) &&
                std::isfinite(x) && std::isfinite(y) && std::isfinite(theta) && std::isfinite(travel),
                "Bound geometry cannot place the rod");
    }
    require(std::isfinite(engine.getDisplacement()) && engine.getDisplacement()>0,
            "Bound geometry did not calculate a finite displacement");
}

void assembly_cases(const cfg::Document &fixture) {
    identities(build(as_model(fixture)));
    identities(build(as_model(with_second_bank(fixture,Route::same_crank))));
    const auto two=build(as_model(with_second_bank(fixture,Route::different_cranks)));
    identities(two);
    require(two.engine->getConnectingRod(0)->getCrankshaft()==two.engine->getCrankshaft(1) &&
            two.engine->getConnectingRod(1)->getCrankshaft()==two.engine->getCrankshaft(0),
            "Individual rod roots were replaced by the output shaft");
    const auto later=build(as_model(with_second_bank(fixture,Route::later_master)));
    identities(later);
    require(later.engine->getConnectingRod(0)->getMasterRod()==later.engine->getConnectingRod(1) &&
            same(later.engine->getConnectingRod(1)->getRodJournalAngle(0),0.625+constants::pi/2),
            "Later master was missed or its angle shifted twice");
}

void placement_cases(cfg::Document fixture) {
    auto rejected=fixture;
    rejected.components.rods.at(0).length={0.001};
    const auto outcome=assembly::build_mechanics(as_model(std::move(rejected)));
    const auto &problem=std::get<cfg::Diagnostic>(outcome);
    require(problem.code=="CFG_INITIALIZE" && problem.stage=="initialize" &&
            problem.message.find("Cannot calculate displacement for cylinder 0")!=std::string::npos,
            "Unusable placement reached an admitted mechanical handoff");

    const auto model=as_model(with_second_bank(fixture,Route::different_cranks));
    using Stop=std::pair<Phase,std::size_t>;
    const std::vector<Stop> expected{
        {Phase::engine,0},{Phase::crankshaft,0},{Phase::crankshaft,1},{Phase::bank,0},{Phase::cylinder,0},
        {Phase::bank,1},{Phase::cylinder,1},{Phase::links,0},{Phase::geometry,0}
    };
    std::vector<Stop> actual;
    const auto pass=assembly::build_mechanics(model,[&](Phase phase,std::size_t index) {
        actual.emplace_back(phase,index);
    });
    require(std::holds_alternative<assembly::MechanicalCandidate>(pass) && actual==expected,
            "Expected allocation phases did not execute in order");
    for (const auto &stop:expected) {
        bool failed=false;
        try {
            (void)assembly::build_mechanics(model,[&](Phase phase,std::size_t index) {
                if (Stop{phase,index}==stop) throw std::bad_alloc();
            });
        }
        catch (const std::bad_alloc &) { failed=true; }
        require(failed,"Partially built candidate was unexpectedly published");
    }
    identities(build(model));
}
}

int main(int argc,char **argv) {
    if (argc!=3) return 2;
    try {
        auto fixture=read_fixture(argv[2]);
        const std::string_view mode=argv[1];
        if (mode=="assembly") assembly_cases(fixture);
        else if (mode=="placement") placement_cases(std::move(fixture));
        else return 2;
    }
    catch(const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
