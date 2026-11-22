#ifndef ATG_ENGINE_SIM_PISTON_OBJECT_H
#define ATG_ENGINE_SIM_PISTON_OBJECT_H

#include "simulation_object.h"
#include "desktop_engine_views.h"

#include "piston.h"
#include "geometry_generator.h"

class PistonObject : public SimulationObject {
    public:
        PistonObject();
        virtual ~PistonObject();

        virtual void generateGeometry();
        std::array<GeometryGenerator::GeometryIndices *, 8> staticShapes() override { return {&m_wristPinHole}; }
        virtual void render(const ViewParameters *view);
        virtual void process(float dt);
        virtual void destroy();

        desktop::ViewHandle<Piston> m_piston;

    protected:
        GeometryGenerator::GeometryIndices
            m_wristPinHole;
};

#endif /* ATG_ENGINE_SIM_PISTON_OBJECT_H */
