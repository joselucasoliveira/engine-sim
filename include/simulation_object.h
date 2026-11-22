#ifndef ATG_ENGINE_SIM_SIMULATION_OBJECT_H
#define ATG_ENGINE_SIM_SIMULATION_OBJECT_H

#include "scs.h"
#include "desktop_host.h"
#include "geometry_generator.h"
#include <array>
#include <limits>
#include <vector>

class Piston;
class CylinderBank;
class EngineSimApplication;
class SimulationObject {
    public:
        struct ViewParameters {
            int Layer0;
            int Layer1;
            int Sublayer;
        };

    public:
        SimulationObject();
        virtual ~SimulationObject();

        virtual void initialize(EngineSimApplication *app);
        virtual void generateGeometry();
        void generatePresentationGeometry();
        virtual void render(const ViewParameters *settings);
        virtual void process(float dt);
        virtual void destroy();

        const Piston *getForemostPiston(const CylinderBank *bank, int layer);

    protected:
        virtual std::array<GeometryGenerator::GeometryIndices *, 8> staticShapes() { return {}; }
        void resetShader();
        void setTransform(
            const atg_scs::RigidBody *rigidBody,
            float scale = 1.0f,
            float lx = 0.0f,
            float ly = 0.0f,
            float theta = 0.0f,
            float z = 0.0f);
        desktop::Vector tintByLayer(const desktop::Vector &col, int layers) const;

        EngineSimApplication *m_app;
    private:
        float m_geometryScale = std::numeric_limits<float>::quiet_NaN();
        std::vector<desktop::Vertex> m_cachedVertices;
        std::vector<unsigned short> m_cachedIndices;
        std::vector<GeometryGenerator::GeometryIndices> m_cachedShapes;
};

#endif /* ATG_ENGINE_SIM_SIMULATION_OBJECT_H */
