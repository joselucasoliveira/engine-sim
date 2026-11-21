#ifndef ENGINE_SIM_SHADERS_H
#define ENGINE_SIM_SHADERS_H
#include "desktop_host.h"
#include "ui_math.h"
class Shaders {
public:
    void Initialize(desktop::Host *host) { m_host = host; }
    void SetObjectTransform(const desktop::Matrix &matrix);
    void SetBaseColor(const desktop::Vector &color);
    void ResetBaseColor();
    desktop::StageEnableFlags GetRegularFlags() const { return desktop::SceneStage; }
    desktop::StageEnableFlags GetUiFlags() const { return desktop::UiStage; }
    void CalculateCamera(float width, float height, const Bounds &bounds, float screenWidth,
                         float screenHeight, float angle = 0);
    void CalculateUiCamera(float screenWidth, float screenHeight);
    void SetClearColor(const desktop::Vector &color);
    desktop::Vector m_cameraPosition;
private:
    desktop::Host *m_host = nullptr;
};
#endif
