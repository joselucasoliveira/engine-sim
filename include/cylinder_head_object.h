#ifndef ATG_ENGINE_SIM_CYLINDER_HEAD_OBJECT_H
#define ATG_ENGINE_SIM_CYLINDER_HEAD_OBJECT_H

#include "simulation_object.h"
#include "desktop_engine_views.h"

#include "cylinder_head.h"
#include "geometry_generator.h"
#include "engine.h"

class CylinderHeadObject : public SimulationObject {
    public:
        CylinderHeadObject();
        virtual ~CylinderHeadObject();

        virtual void generateGeometry();
        virtual void render(const ViewParameters *view);
        virtual void process(float dt);
        virtual void destroy();

        desktop::ViewHandle<CylinderHead> m_head;
        std::array<GeometryGenerator::GeometryIndices *, 8> staticShapes() override {
            return {&m_valveShadow, &m_valveRoller, &m_valveRollerShadow, &m_valveRollerPin, &m_camCenter};
        }

    protected:
        GeometryGenerator::GeometryIndices m_valveShadow, m_valveRoller, m_valveRollerShadow, m_valveRollerPin, m_camCenter;
        void generateCamshaft(
            const Camshaft *camshaft,
            double padding,
            double rollerRadius,
            GeometryGenerator::GeometryIndices *indices);
};

#endif /* ATG_ENGINE_SIM_CYLINDER_HEAD_OBJECT_H */
