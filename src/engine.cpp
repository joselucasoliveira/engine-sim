#include "../include/engine.h"

#include "../include/constants.h"
#include "../include/units.h"
#include "../include/fuel.h"
#include "../include/piston_engine_simulator.h"

#include <cmath>
#include <stdexcept>
#include <assert.h>

Engine::Engine() {
    m_name = "";

    m_crankshafts = nullptr;
    m_cylinderBanks = nullptr;
    m_heads = nullptr;
    m_pistons = nullptr;
    m_connectingRods = nullptr;
    m_exhaustSystems = nullptr;
    m_intakes = nullptr;
    m_combustionChambers = nullptr;

    m_crankshaftCount = 0;
    m_cylinderBankCount = 0;
    m_cylinderCount = 0;
    m_intakeCount = 0;
    m_exhaustSystemCount = 0;
    m_starterSpeed = 0;
    m_starterTorque = 0;
    m_dynoMinSpeed = 0;
    m_dynoMaxSpeed = 0;
    m_dynoHoldStep = 0;
    m_redline = 0;

    m_throttle = nullptr;
    m_throttleValue = 0.0;
    m_displacement = 0.0;

    m_initialSimulationFrequency = 10000.0;
    m_initialHighFrequencyGain = 0.01;
    m_initialJitter = 0.5;
    m_initialNoise = 1.0;
}

Engine::~Engine() {
    destroy();
}

void Engine::initialize(const Parameters &params) {
    destroy();
    m_crankshaftCount = params.crankshaftCount;
    m_cylinderCount = params.cylinderCount;
    m_cylinderBankCount = params.cylinderBanks;
    m_exhaustSystemCount = params.exhaustSystemCount;
    m_intakeCount = params.intakeCount;
    m_starterTorque = params.starterTorque;
    m_starterSpeed = params.starterSpeed;
    m_dynoMinSpeed = params.dynoMinSpeed;
    m_dynoMaxSpeed = params.dynoMaxSpeed;
    m_dynoHoldStep = params.dynoHoldStep;
    m_redline = params.redline;
    m_name = params.name;
    m_throttle = params.throttle;
    m_initialHighFrequencyGain = params.initialHighFrequencyGain;
    m_initialSimulationFrequency = params.initialSimulationFrequency;
    m_initialJitter = params.initialJitter;
    m_initialNoise = params.initialNoise;

    m_crankshafts = new Crankshaft[m_crankshaftCount];
    m_cylinderBanks = new CylinderBank[m_cylinderBankCount];
    m_heads = new CylinderHead[m_cylinderBankCount];
    m_pistons = new Piston[m_cylinderCount];
    m_connectingRods = new ConnectingRod[m_cylinderCount];
    m_exhaustSystems = new ExhaustSystem[m_exhaustSystemCount];
    m_intakes = new Intake[m_intakeCount];
    m_combustionChambers = new CombustionChamber[m_cylinderCount];

    for (int i = 0; i < m_exhaustSystemCount; ++i) {
        m_exhaustSystems[i].m_index = i;
    }

    for (int i = 0; i < m_cylinderCount; ++i) {
        m_combustionChambers[i].setEngine(this);
    }
}

void Engine::destroy() {
    for (int i = 0; i < m_crankshaftCount; ++i) {
        if (m_crankshafts != nullptr) m_crankshafts[i].destroy();
    }

    for (int i = 0; i < m_cylinderCount; ++i) {
        if (m_pistons != nullptr) m_pistons[i].destroy();
        if (m_connectingRods != nullptr) m_connectingRods[i].destroy();
        if (m_combustionChambers != nullptr) m_combustionChambers[i].destroy();
    }

    for (int i = 0; i < m_exhaustSystemCount; ++i) {
        if (m_exhaustSystems != nullptr) m_exhaustSystems[i].destroy();
    }

    for (int i = 0; i < m_intakeCount; ++i) {
        if (m_intakes != nullptr) m_intakes[i].destroy();
    }

    for (int i = 0; i < m_cylinderBankCount; ++i) {
        if (m_heads != nullptr) m_heads[i].destroy();
    }
    m_ignitionModule.destroy();

    if (m_throttle != nullptr) delete m_throttle;
    if (m_crankshafts != nullptr) delete[] m_crankshafts;
    if (m_cylinderBanks != nullptr) delete[] m_cylinderBanks;
    if (m_heads != nullptr) delete[] m_heads;
    if (m_pistons != nullptr) delete[] m_pistons;
    if (m_connectingRods != nullptr) delete[] m_connectingRods;
    if (m_exhaustSystems != nullptr) delete[] m_exhaustSystems;
    if (m_intakes != nullptr) delete[] m_intakes;
    if (m_combustionChambers != nullptr) delete[] m_combustionChambers;

    m_crankshafts = nullptr;
    m_cylinderBanks = nullptr;
    m_pistons = nullptr;
    m_connectingRods = nullptr;
    m_heads = nullptr;
    m_exhaustSystems = nullptr;
    m_intakes = nullptr;
    m_combustionChambers = nullptr;
    m_throttle = nullptr;
    m_crankshaftCount = m_cylinderBankCount = m_cylinderCount = 0;
    m_exhaustSystemCount = m_intakeCount = 0;
    while (!m_resources.empty()) m_resources.pop_back();
}

Crankshaft *Engine::getOutputCrankshaft() const {
    return &m_crankshafts[0];
}

void Engine::setSpeedControl(double s) {
    m_throttle->setSpeedControl(s);
}

double Engine::getSpeedControl() {
    return m_throttle->getSpeedControl();
}

void Engine::setThrottle(double throttle) {
    for (int i = 0; i < m_intakeCount; ++i) {
        m_intakes[i].m_throttle = throttle;
    }

    m_throttleValue = throttle;
}

double Engine::getThrottle() const {
    return m_throttleValue;
}

double Engine::getThrottlePlateAngle() const {
    return (1 - m_intakes[0].getThrottlePlatePosition()) * (constants::pi / 2);
}

bool placeRod(
    const ConnectingRod &rod,
    const CylinderBank &bank,
    const Crankshaft &crankshaft,
    double crankshaftAngle,
    double *p_x,
    double *p_y,
    double *theta,
    double *s)
{
    if (p_x == nullptr || p_y == nullptr || theta == nullptr || s == nullptr
        || !std::isfinite(crankshaftAngle) || !std::isfinite(rod.getLength())
        || rod.getLength() <= 0 || rod.getCrankshaft() != &crankshaft) return false;

    // Reject cycles before following any master-rod chain.
    const ConnectingRod *slow = &rod, *fast = &rod;
    while (fast != nullptr && fast->getMasterRod() != nullptr) {
        slow = slow->getMasterRod();
        fast = fast->getMasterRod()->getMasterRod();
        if (slow == fast) return false;
    }

    double originX, originY, localX, localY, orientation;
    const auto *master = rod.getMasterRod();
    if (master != nullptr) {
        if (rod.getJournal() < 0 || rod.getJournal() >= master->getRodJournalCount()
            || master->getPiston() == nullptr || master->getPiston()->getCylinderBank() == nullptr
            || master->getCrankshaft() == nullptr) return false;
        double masterTravel;
        if (!placeRod(*master, *master->getPiston()->getCylinderBank(), *master->getCrankshaft(),
                crankshaftAngle, &originX, &originY, &orientation, &masterTravel)) return false;
        // The parent result is its crankpin. Journal coordinates use its body origin.
        rod.getMasterRod()->getRodJournalPositionLocal(rod.getJournal(), &localX, &localY);
        localY -= master->getBigEndLocal();
    }
    else {
        if (rod.getJournal() < 0 || rod.getJournal() >= crankshaft.getRodJournalCount()) return false;
        orientation = crankshaftAngle;
        originX = crankshaft.getPosX();
        originY = crankshaft.getPosY();
        // The journal accessor predates const-qualified reads.
        rod.getCrankshaft()->getRodJournalPositionLocal(rod.getJournal(), &localX, &localY);
    }

    const double cosine = std::cos(orientation), sine = std::sin(orientation);
    *p_x = originX + cosine * localX - sine * localY;
    *p_y = originY + sine * localX + cosine * localY;
    const double offsetX = *p_x - bank.getX(), offsetY = *p_y - bank.getY();
    const double axisSquared = bank.getDx() * bank.getDx() + bank.getDy() * bank.getDy();
    const double projection = bank.getDx() * offsetX + bank.getDy() * offsetY;
    const double radiusSquared = offsetX * offsetX + offsetY * offsetY - rod.getLength() * rod.getLength();
    const double discriminant = projection * projection - axisSquared * radiusSquared;
    if (!(axisSquared > 0) || !(discriminant >= 0)) return false;

    *s = (projection + std::sqrt(discriminant)) / axisSquared;
    if (!std::isfinite(*s) || *s < 0) return false;
    const double wristX = bank.getX() + bank.getDx() * *s;
    const double wristY = bank.getY() + bank.getDy() * *s;
    *theta = std::atan2(wristY - *p_y, wristX - *p_x) - constants::pi / 2;
    return std::isfinite(*p_x) && std::isfinite(*p_y) && std::isfinite(*theta);
}

void Engine::calculateDisplacement() {
    // There is a closed-form/correct way to do this which I really
    // don't feel like deriving right now, so I'm just going with this
    // numerical approximation.
    constexpr int Resolution = 1000;

    std::vector<double> min_s(m_cylinderCount);
    std::vector<double> max_s(m_cylinderCount);

    for (int i = 0; i < m_cylinderCount; ++i) {
        min_s[i] = DBL_MAX;
        max_s[i] = -DBL_MAX;
    }

    for (int j = 0; j < Resolution; ++j) {
        const double crankshaftAngle = 2 * (j / static_cast<double>(Resolution)) * constants::pi;

        for (int i = 0; i < m_cylinderCount; ++i) {
            const Piston &piston = m_pistons[i];
            const CylinderBank &bank = *piston.getCylinderBank();
            const ConnectingRod &rod = *piston.getRod();
            const Crankshaft &shaft = *rod.getCrankshaft();

            double p_x, p_y;
            double theta;
            double s;
            if (!placeRod(
                rod,
                bank,
                shaft,
                crankshaftAngle,
                &p_x,
                &p_y,
                &theta,
                &s))
            {
                throw std::runtime_error("Cannot calculate displacement for cylinder "
                    + std::to_string(i) + " at crank angle " + std::to_string(crankshaftAngle));
            }

            min_s[i] = std::min(min_s[i], s);
            max_s[i] = std::max(max_s[i], s);
        }
    }

    double displacement = 0;
    for (int i = 0; i < m_cylinderCount; ++i) {
        const Piston &piston = m_pistons[i];
        const CylinderBank &bank = *piston.getCylinderBank();

        if (min_s[i] < max_s[i]) {
            const double r = bank.getBore() / 2.0;
            displacement += constants::pi * r * r * (max_s[i] - min_s[i]);
        }
    }

    m_displacement = displacement;
}

double Engine::getIntakeFlowRate() const {
    double airIntake = 0;
    for (int i = 0; i < m_intakeCount; ++i) {
        airIntake += m_intakes[i].m_flowRate;
    }

    return airIntake;
}

void Engine::update(double dt) {
    m_throttle->update(dt, this);
}

double Engine::getManifoldPressure() const {
    double pressureSum = 0.0;
    for (int i = 0; i < m_intakeCount; ++i) {
        pressureSum += m_intakes[i].m_system.pressure();
    }

    return pressureSum / m_intakeCount;
}

double Engine::getIntakeAfr() const {
    double totalOxygen = 0.0;
    double totalFuel = 0.0;
    for (int i = 0; i < m_intakeCount; ++i) {
        totalOxygen += m_intakes[i].m_system.n_o2();
        totalFuel += m_intakes[i].m_system.n_fuel();
    }

    constexpr double octaneMolarMass = units::mass(114.23, units::g);
    constexpr double oxygenMolarMass = units::mass(31.9988, units::g);

    if (totalFuel == 0) return 0;
    else {
        return
            (oxygenMolarMass * totalOxygen / 0.21)
            / (totalFuel * octaneMolarMass);
    }
}

double Engine::getExhaustO2() const {
    double totalInert = 0.0;
    double totalOxygen = 0.0;
    double totalFuel = 0.0;
    for (int i = 0; i < m_exhaustSystemCount; ++i) {
        totalInert += m_exhaustSystems[i].m_system.n_inert();
        totalOxygen += m_exhaustSystems[i].m_system.n_o2();
        totalFuel += m_exhaustSystems[i].m_system.n_fuel();
    }

    constexpr double octaneMolarMass = units::mass(114.23, units::g);
    constexpr double oxygenMolarMass = units::mass(31.9988, units::g);
    constexpr double nitrogenMolarMass = units::mass(28.014, units::g);

    if (totalFuel == 0) return 0;
    else {
        return
            (oxygenMolarMass * totalOxygen)
            / (
                totalFuel * octaneMolarMass
                + nitrogenMolarMass * totalInert
                + oxygenMolarMass * totalOxygen);
    }
}

void Engine::resetFuelConsumption() {
    for (int i = 0; i < m_intakeCount; ++i) {
        m_intakes[i].m_totalFuelInjected = 0;
    }
}

double Engine::getTotalFuelMassConsumed() const {
    double n_fuelConsumed = 0;
    for (int i = 0; i < m_intakeCount; ++i) {
        n_fuelConsumed += m_intakes[i].m_totalFuelInjected;
    }

    return n_fuelConsumed * m_fuel.getMolecularMass();
}

double Engine::getTotalVolumeFuelConsumed() const {
    return getTotalFuelMassConsumed() / m_fuel.getDensity();
}

int Engine::getMaxDepth() const {
    int maxDepth = 0;
    for (int i = 0; i < m_crankshaftCount; ++i) {
        maxDepth = std::max(m_crankshafts[i].getRodJournalCount(), maxDepth);
    }

    return maxDepth;
}

Simulator *Engine::createSimulator(Vehicle *vehicle, Transmission *transmission) {
    PistonEngineSimulator *simulator = new PistonEngineSimulator;
    Simulator::Parameters simulatorParams;
    simulatorParams.systemType = Simulator::SystemType::NsvOptimized;
    simulator->initialize(simulatorParams);

    simulator->loadSimulation(this, vehicle, transmission);
    simulator->setFluidSimulationSteps(8);

    return static_cast<Simulator *>(simulator);
}

double Engine::getRpm() const {
    if (m_crankshaftCount == 0) return 0;
    else return std::abs(units::toRpm(getCrankshaft(0)->m_body.v_theta));
}

double Engine::getSpeed() const {
    if (m_crankshaftCount == 0) return 0;
    else return std::abs(getCrankshaft(0)->m_body.v_theta);
}

bool Engine::isSpinningCw() const {
    return getOutputCrankshaft()->m_body.v_theta <= 0;
}
