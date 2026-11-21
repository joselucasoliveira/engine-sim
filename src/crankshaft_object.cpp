#include "../include/crankshaft_object.h"

#include "../include/cylinder_bank.h"
#include "../include/engine_sim_application.h"
#include "../include/ui_utilities.h"

CrankshaftObject::CrankshaftObject() {
    m_crankshaft = nullptr;
}

CrankshaftObject::~CrankshaftObject() {
    /* void */
}

void CrankshaftObject::generateGeometry() {
    /* void */
}

void CrankshaftObject::render(const ViewParameters *view) {
    if (view->Sublayer != 2) return;

    const desktop::Vector grey0 = mix(m_app->getBackgroundColor(), m_app->getForegroundColor(), 0.7f);
    const desktop::Vector grey1 = mix(m_app->getBackgroundColor(), m_app->getForegroundColor(), 0.6f);
    const desktop::Vector grey2 = mix(m_app->getBackgroundColor(), m_app->getForegroundColor(), 0.2f);

    const int journalCount = m_crankshaft->getRodJournalCount();
    for (int i = 0; i < journalCount; ++i) {
        const int layer = i;
        if (layer > view->Layer1 || layer < view->Layer0) continue;

        const desktop::Vector col = tintByLayer(grey0, layer - view->Layer0);

        resetShader();
        setTransform(
            &m_crankshaft->m_body,
            (float)m_crankshaft->getThrow(),
            0.0f,
            0.0f,
            (float)m_crankshaft->getRodJournalAngle(i));

        m_app->getShaders()->SetBaseColor(col);
        m_app->getEngine()->DrawModel(
            m_app->getShaders()->GetRegularFlags(),
            m_app->getAssetManager()->GetModelAsset("Crankshaft"),
            0x32 - layer);
    }

    setTransform(
        &m_crankshaft->m_body,
        (float)m_crankshaft->getThrow(),
        0.0f,
        0.0f,
        0.0f);
    m_app->getShaders()->SetBaseColor(grey1);
    m_app->getEngine()->DrawModel(
        m_app->getShaders()->GetRegularFlags(),
        m_app->getAssetManager()->GetModelAsset("CrankSnout"),
        0x32);

    m_app->getShaders()->SetBaseColor(grey2);
    m_app->getEngine()->DrawModel(
        m_app->getShaders()->GetRegularFlags(),
        m_app->getAssetManager()->GetModelAsset("CrankSnoutThreads"),
        0x32);
}

void CrankshaftObject::process(float dt) {
    /* void */
}

void CrankshaftObject::destroy() {
    /* void */
}
