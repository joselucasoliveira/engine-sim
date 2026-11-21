#include "../include/shaders.h"
void Shaders::SetObjectTransform(const desktop::Matrix &matrix) { m_host->drawing.transform = matrix; }
void Shaders::SetBaseColor(const desktop::Vector &color) { m_host->drawing.color = color; }
void Shaders::ResetBaseColor() { m_host->drawing.color = desktop::math::Constants::One; }
void Shaders::SetClearColor(const desktop::Vector &color) { m_host->drawing.clearColor = color; }

void Shaders::CalculateCamera(
    float width,
    float height,
    const Bounds &cameraBounds,
    float screenWidth,
    float screenHeight,
    float angle)
{
    const desktop::Matrix projection = desktop::math::OrthographicProjection(
        width,
        height,
        0.001f,
        500.0f);
    const Point scale = Point(screenWidth, screenHeight);
    const Point center =
        (cameraBounds.getPosition() - Point(screenWidth / 2, screenHeight / 2))
        / scale;

    m_host->drawing.scene.projection = desktop::math::Transpose(
        desktop::math::MatMult(
            projection,
            desktop::math::MatMult(
                desktop::math::ScaleTransform(desktop::math::LoadVector(
                    cameraBounds.width() / screenWidth,
                    cameraBounds.height() / screenHeight,
                    1.0f)),
                desktop::math::TranslationTransform(desktop::math::LoadVector(center.x, center.y, 0.0f))
            )
        )
    );

    m_host->drawing.scene.projection = desktop::math::Transpose(projection);

    const float sinAngle = std::sin(angle);
    const float cosAngle = std::cos(angle);

    const desktop::Vector cameraEye =
        desktop::math::Add(
                desktop::math::LoadVector(10.0f * sinAngle, 0.0f, 10.0f * cosAngle, 1.0f),
                m_cameraPosition);
    const desktop::Vector cameraTarget =
        desktop::math::Add(
                desktop::math::LoadVector(0.0f, 0.0f, 0.0f, 1.0f),
                m_cameraPosition);
    const desktop::Vector up = desktop::math::LoadVector(0.0f, 1.0f);

    m_host->drawing.scene.view =
        desktop::math::Transpose(desktop::math::CameraTarget(cameraEye, cameraTarget, up));
    m_host->drawing.scene.eye = desktop::math::LoadVector(cameraEye);
}

void Shaders::CalculateUiCamera(float screenWidth, float screenHeight) {
    m_host->drawing.ui.projection = desktop::math::Transpose(
        desktop::math::OrthographicProjection(
            screenWidth,
            screenHeight,
            0.001f,
            500.0f));

    const desktop::Vector cameraEye =
        desktop::math::LoadVector(0.0f, 0.0f, 10.0f, 1.0f);
    const desktop::Vector cameraTarget =
        desktop::math::LoadVector(0.0f, 0.0f, 0.0f, 1.0f);
    const desktop::Vector up = desktop::math::LoadVector(0.0f, 1.0f);

    m_host->drawing.ui.view =
        desktop::math::Transpose(desktop::math::CameraTarget(cameraEye, cameraTarget, up));
    m_host->drawing.ui.eye = desktop::math::LoadVector(cameraEye);
}
