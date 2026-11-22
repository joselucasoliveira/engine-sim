#ifndef ATG_ENGINE_SIM_CYLINDER_BANK_OBJECT_H
#define ATG_ENGINE_SIM_CYLINDER_BANK_OBJECT_H

#include "simulation_object.h"
#include "desktop_engine_views.h"

#include "geometry_generator.h"
#include "cylinder_bank.h"
#include "cylinder_head.h"

class CylinderBankObject : public SimulationObject {
    public:
        CylinderBankObject();
        virtual ~CylinderBankObject();

        virtual void generateGeometry();
        std::array<GeometryGenerator::GeometryIndices *, 8> staticShapes() override { return {&m_walls}; }
        virtual void render(const ViewParameters *view);
        virtual void process(float dt);
        virtual void destroy();

        desktop::ViewHandle<CylinderBank> m_bank;
        desktop::ViewHandle<CylinderHead> m_head;
        GeometryGenerator::GeometryIndices m_walls;
};

#endif /* ATG_ENGINE_SIM_CYLINDER_BANK_OBJECT_H */
