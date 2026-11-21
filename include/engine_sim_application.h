#ifndef ATG_ENGINE_SIM_ENGINE_SIM_APPLICATION_H
#define ATG_ENGINE_SIM_ENGINE_SIM_APPLICATION_H

#include "geometry_generator.h"
#include "simulator.h"
#include "engine.h"
#include "simulation_object.h"
#include "ui_manager.h"
#include "dynamometer.h"
#include "oscilloscope.h"
#include "audio_buffer.h"
#include "convolution_filter.h"
#include "shaders.h"
#include "engine_view.h"
#include "right_gauge_cluster.h"
#include "cylinder_temperature_gauge.h"
#include "synthesizer.h"
#include "oscilloscope_cluster.h"
#include "performance_cluster.h"
#include "load_simulation_cluster.h"
#include "mixer_cluster.h"
#include "info_cluster.h"
#include "application_settings.h"
#include "transmission.h"
#include "engine_session.h"
#include "resource_paths.h"

#include "desktop_host.h"
#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
#include "dtv.h"
#endif

#include <vector>
#include <memory>

class EngineSimApplication {
    private:
        static std::string s_buildVersion;
        struct UserInterface;
        struct PendingSession;
        bool loadEngine(EngineDefinition definition);
        void buildObjects(Engine *engine, std::vector<SimulationObject *> &objects);
        std::unique_ptr<UserInterface> prepareUserInterface(Engine *engine, Simulator *simulator);
        void bindSession();
        void updatePalette();
        void reportLoadFailure(const std::string &message);

    public:
        EngineSimApplication();
        virtual ~EngineSimApplication();

        static std::string getBuildVersion() { return s_buildVersion; }

        void initialize(std::unique_ptr<desktop::Host> host,
                        const engine_sim::ResourceOptions &resources = {});
        void run();
        void destroy();

        // Takes ownership of all three arguments, including on a rejected load.
        bool loadEngine(Engine *engine, Vehicle *vehicle, Transmission *transmission);
        void drawGenerated(
                const GeometryGenerator::GeometryIndices &indices,
                int layer = 0);
        void drawGeneratedUi(
                const GeometryGenerator::GeometryIndices &indices,
                int layer = 0);
        void drawGenerated(
                const GeometryGenerator::GeometryIndices &indices,
                int layer,
                desktop::StageEnableFlags flags);
        void configure(const ApplicationSettings &settings);
        GeometryGenerator *getGeometryGenerator() { return &m_geometryGenerator; }

        Shaders *getShaders() { return &m_shaders; }
        desktop::TextRenderer *getTextRenderer() { return &m_textRenderer; }

        void createObjects(Engine *engine);
        void destroyObjects();
        desktop::Host *getEngine() { return m_host.get(); }

        float pixelsToUnits(float pixels) const;
        float unitsToPixels(float units) const;

        desktop::Vector getBackgroundColor() const { return m_background; }
        desktop::Vector getForegroundColor() const { return m_foreground; }
        desktop::Vector getHightlight1Color() const { return m_highlight1; }
        desktop::Vector getPink() const { return m_pink; }
        desktop::Vector getGreen() const { return m_green; }
        desktop::Vector getYellow() const { return m_yellow; }
        desktop::Vector getRed() const { return m_red; }
        desktop::Vector getOrange() const { return m_orange; }
        desktop::Vector getBlue() const { return m_blue; }

        const SimulationObject::ViewParameters &getViewParameters() const;
        void setViewLayer(int view) { m_viewParameters.Layer0 = view; }

        desktop::Host *getAssetManager() { return m_host.get(); }

        int getScreenWidth() const { return m_screenWidth; }
        int getScreenHeight() const { return m_screenHeight; }

        Simulator *getSimulator() { return m_simulator; }
        InfoCluster *getInfoCluster() { return m_infoCluster; }
        ApplicationSettings* getAppSettings() { return &m_applicationSettings; }

    protected:
        bool loadScript();
        void processEngineInput();
        void renderScene();

        void refreshUserInterface();

    protected:
        double m_speedSetting = 1.0;
        double m_targetSpeedSetting = 1.0;

        double m_clutchPressure = 1.0;
        double m_targetClutchPressure = 1.0;
        int m_lastMouseWheel = 0;

    protected:
        virtual void initialize();
        virtual void process(float dt);
        virtual void render();

        float m_displayAngle;
        float m_displayHeight;
        int m_gameWindowHeight;
        int m_screenWidth;
        int m_screenHeight;
        
        ApplicationSettings m_applicationSettings;
        Shaders m_shaders;

        std::unique_ptr<desktop::Host> m_host;

        std::string m_assetPath;
        engine_sim::ResourcePaths m_resources;


        GeometryGenerator m_geometryGenerator;
        desktop::TextRenderer m_textRenderer;

        std::vector<SimulationObject *> m_objects;
        Engine *m_iceEngine;
        Vehicle *m_vehicle;
        Transmission *m_transmission;
        Simulator *m_simulator;
        double m_dynoSpeed;
        double m_torque;

        std::unique_ptr<EngineSession> m_session;
        std::unique_ptr<UserInterface> m_userInterface;
        EngineView *m_engineView;
        RightGaugeCluster *m_rightGaugeCluster;
        OscilloscopeCluster *m_oscCluster;
        CylinderTemperatureGauge *m_temperatureGauge;
        PerformanceCluster *m_performanceCluster;
        LoadSimulationCluster *m_loadSimulationCluster;
        MixerCluster *m_mixerCluster;
        InfoCluster *m_infoCluster;
        SimulationObject::ViewParameters m_viewParameters;

        bool m_windowCreated = false;
        bool m_destroyed = false;
        std::string m_loadError;

        bool m_paused;

    protected:
        void startRecording();
        void updateScreenSizeStability();
        bool readyToRecord();
        void stopRecording();
        void recordFrame();
        bool isRecording() const { return m_recording; }

        static constexpr int ScreenResolutionHistoryLength = 5;
        int m_screenResolution[ScreenResolutionHistoryLength][2];
        int m_screenResolutionIndex;
        bool m_recording;

        desktop::Vector m_background;
        desktop::Vector m_foreground;
        desktop::Vector m_shadow;
        desktop::Vector m_highlight1;
        desktop::Vector m_highlight2;

        desktop::Vector m_pink;
        desktop::Vector m_orange;
        desktop::Vector m_yellow;
        desktop::Vector m_red;
        desktop::Vector m_green;
        desktop::Vector m_blue;

        AudioBuffer m_audioBuffer;
        std::array<std::int16_t, 44100> m_audioScratch{};

        int m_oscillatorSampleOffset;
        int m_screen;

#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
        atg_dtv::Encoder m_encoder;
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
};

#endif /* ATG_ENGINE_SIM_ENGINE_SIM_APPLICATION_H */
