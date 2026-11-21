#include "../include/simulation_object.h"

#include "../include/piston.h"
#include "../include/cylinder_bank.h"

#include "../include/engine_sim_application.h"

SimulationObject::SimulationObject() {
    m_app = nullptr;
}

SimulationObject::~SimulationObject() {
    /* void */
}

void SimulationObject::initialize(EngineSimApplication *app) {
    m_app = app;
}

void SimulationObject::generateGeometry() {
    /* void */
}

void SimulationObject::render(const ViewParameters *settings) {
    /* void */
}

void SimulationObject::process(float dt) {
    /* void */
}

void SimulationObject::destroy() {
    /* void */
}

Piston *SimulationObject::getForemostPiston(CylinderBank *bank, int layer) {
    Engine *engine = m_app->getSimulator()->getEngine();
    Piston *frontmostPiston = nullptr;
    const int cylinderCount = engine->getCylinderCount();
    for (int i = 0; i < cylinderCount; ++i) {
        Piston *piston = engine->getPiston(i);
        if (piston->getCylinderBank() == bank) {
            if (piston->getRod()->getJournal() >= layer) {
                if (frontmostPiston == nullptr
                    || piston->getRod()->getJournal() < frontmostPiston->getRod()->getJournal()) {
                    frontmostPiston = piston;
                }
            }
        }
    }

    return frontmostPiston;
}

void SimulationObject::resetShader() {
    m_app->getShaders()->ResetBaseColor();
    m_app->getShaders()->SetObjectTransform(desktop::math::LoadIdentity());
}

void SimulationObject::setTransform(
    atg_scs::RigidBody *rigidBody,
    float scale,
    float lx,
    float ly,
    float angle,
    float z)
{
    double p_x, p_y;
    rigidBody->localToWorld(lx, ly, &p_x, &p_y);

    const desktop::Matrix rot = desktop::math::RotationTransform(
            desktop::math::Constants::ZAxis,
            (float)rigidBody->theta + angle);
    const desktop::Matrix trans = desktop::math::TranslationTransform(
            desktop::math::LoadVector((float)p_x, (float)p_y, z));
    const desktop::Matrix scaleTransform = desktop::math::ScaleTransform(desktop::math::LoadScalar(scale));

    m_app->getShaders()->SetObjectTransform(
        desktop::math::MatMult(desktop::math::MatMult(trans, rot), scaleTransform));
}

desktop::Vector SimulationObject::tintByLayer(const desktop::Vector &col, int layers) const {
    desktop::Vector result = col;
    for (int i = 0; i < layers; ++i) {
        result = desktop::math::Add(
            desktop::math::Mul(result, desktop::math::LoadScalar(0.3f)),
            desktop::math::Mul(m_app->getBackgroundColor(), desktop::math::LoadScalar(0.7f))
        );
    }

    return result;
}
