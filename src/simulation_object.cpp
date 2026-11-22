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

const Piston *SimulationObject::getForemostPiston(const CylinderBank *bank, int layer) {
    auto engine = m_app->engineObservation();
    const Piston *frontmostPiston = nullptr;
    const int cylinderCount = engine->getCylinderCount();
    for (int i = 0; i < cylinderCount; ++i) {
        const Piston *piston = engine->getPiston(i);
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
    const atg_scs::RigidBody *rigidBody,
    float scale,
    float lx,
    float ly,
    float angle,
    float z)
{
    double p_x, p_y;
    auto transform = *rigidBody;
    transform.localToWorld(lx, ly, &p_x, &p_y);

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

void SimulationObject::generatePresentationGeometry() {
    const auto shapes = staticShapes();
    if (!shapes[0]) { generateGeometry(); return; }
    auto *generator = m_app->getGeometryGenerator();
    const float scale = m_app->pixelsToUnits(1);
    const int vertex = generator->getCurrentVertexCount(), index = generator->getCurrentIndexCount();
    if (m_geometryScale != scale) {
        generateGeometry();
        m_cachedVertices.assign(generator->getVertexData() + vertex,
            generator->getVertexData() + generator->getCurrentVertexCount());
        m_cachedIndices.assign(generator->getIndexData() + index,
            generator->getIndexData() + generator->getCurrentIndexCount());
        m_cachedShapes.clear();
        for (const auto *shape : shapes) {
            if (!shape) continue;
            auto local = *shape;
            local.BaseVertex -= vertex; local.BaseIndex -= index; local.VertexData = nullptr;
            m_cachedShapes.push_back(local);
        }
        m_geometryScale = scale;
    }
    else {
        if (!generator->appendGeometry(m_cachedVertices.data(), static_cast<int>(m_cachedVertices.size()),
                m_cachedIndices.data(), static_cast<int>(m_cachedIndices.size())))
            throw desktop::DesktopError("geometry", "Cached engine geometry exceeds presentation capacity");
        std::size_t i = 0;
        for (auto *shape : shapes) {
            if (!shape) continue;
            *shape = m_cachedShapes[i++];
            shape->BaseVertex += vertex; shape->BaseIndex += index;
            shape->VertexData = generator->getVertexData() + shape->BaseVertex;
        }
    }
}
