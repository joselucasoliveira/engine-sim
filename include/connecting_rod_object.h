#ifndef ATG_ENGINE_SIM_CONNECTING_ROD_OBJECT_H
#define ATG_ENGINE_SIM_CONNECTING_ROD_OBJECT_H

#include "simulation_object.h"
#include "desktop_engine_views.h"

#include "connecting_rod.h"
#include "geometry_generator.h"

class ConnectingRodObject : public SimulationObject {
    public:
        ConnectingRodObject();
        virtual ~ConnectingRodObject();

        virtual void generateGeometry();
        std::array<GeometryGenerator::GeometryIndices *, 8> staticShapes() override { return {&m_connectingRodBody, &m_pins}; }
        virtual void render(const ViewParameters *view);
        virtual void process(float dt);
        virtual void destroy();

        desktop::ViewHandle<ConnectingRod> m_connectingRod;

    protected:
        GeometryGenerator::GeometryIndices
            m_connectingRodBody,
            m_pins;
};

#endif /* ATG_ENGINE_SIM_CONNECTING_ROD_OBJECT_H */
