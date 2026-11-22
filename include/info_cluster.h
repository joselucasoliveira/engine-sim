#ifndef ATG_ENGINE_SIM_INFO_CLUSTER_H
#define ATG_ENGINE_SIM_INFO_CLUSTER_H

#include "ui_element.h"
#include "desktop_engine_views.h"

#include "engine.h"

#include <string>

class InfoCluster : public UiElement {
public:
    InfoCluster();
    virtual ~InfoCluster();

    virtual void initialize(EngineSimApplication *app);
    virtual void destroy();

    virtual void update(float dt);
    virtual void render();

    void setEngine(desktop::ViewHandle<Engine> engine) { m_engine = engine; }
    void setLogMessage(const std::string &logMessage) { m_logMessage = logMessage; }
    std::string getLogMessage() const { return m_logMessage; }

protected:
    desktop::ViewHandle<Engine> m_engine;

    std::string m_logMessage;
};

#endif /* ATG_ENGINE_SIM_INFO_CLUSTER_H */
