#include "../include/desktop_host.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace desktop {
Host::Host() : window(*this) {}
void Host::loadModels(const engine_sim::CompiledScene &scene) {
    models.clear();
    for (const auto &mesh : scene.meshes) models.push_back({mesh.name, mesh.indices.size()/3});
}
Model *Host::GetModelAsset(const char *name) {
    if (!name) throw DesktopError("model", "missing model name");
    const auto found = std::find_if(models.begin(), models.end(), [name](const Model &model) { return model.name == name; });
    if (found == models.end()) throw DesktopError("model", std::string("missing scene model ") + name);
    return &*found;
}
void Host::DrawModel(StageEnableFlags stage, const Model *model, int layer) {
    if (!model) throw DesktopError("model", "missing model handle");
    if (layer<0 || layer>=256) throw DesktopError("draw", "layer is outside the desktop range");
    DrawPacket packet;
    packet.kind=DrawPacket::Kind::Model; packet.stage=stage; packet.layer=layer;
    packet.state=drawing; packet.model=model->name; packet.faceCount=static_cast<int>(model->faceCount);
    packets.push_back(std::move(packet));
}
void Host::drawGenerated(StageEnableFlags stage, int baseIndex, int baseVertex, int faceCount, int layer) {
    if (layer<0 || layer>=256) throw DesktopError("draw", "layer is outside the desktop range");
    DrawPacket packet;
    packet.stage=stage; packet.layer=layer; packet.state=drawing;
    packet.baseIndex=baseIndex; packet.baseVertex=baseVertex; packet.faceCount=faceCount;
    packets.push_back(std::move(packet));
}
void Host::drawText(const std::string &text, float x, float y, float height, const Vector &color) {
    DrawPacket packet;
    packet.kind=DrawPacket::Kind::Text; packet.stage=TextStage; packet.state=drawing;
    packet.state.color=color; packet.text=text; packet.x=x; packet.y=y; packet.height=height;
    packets.push_back(std::move(packet));
}
void Host::validateGeometry(const Vertex *vertices, std::size_t vertexCount,
                            const std::uint16_t *indices, std::size_t indexCount) const {
    if (vertexCount>100000 || indexCount>200000 || (vertexCount && !vertices) || (indexCount && !indices)) {
        throw DesktopError("geometry", "generated geometry exceeds its buffer");
    }
    for (std::size_t i=0; i<vertexCount; ++i) {
        std::array<float,10> values;
        std::memcpy(values.data(), vertices+i, sizeof(Vertex));
        for (std::size_t component=0; component<values.size(); ++component) {
            if (!std::isfinite(values[component])) {
                throw DesktopError("geometry", "generated vertex "+std::to_string(i)+" component "
                    +std::to_string(component)+" is not finite");
            }
        }
    }
    for (const auto &packet : packets) {
        const auto finiteVector=[](const Vector &v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
        };
        for (const auto *matrix : {&packet.state.transform,&packet.state.scene.view,&packet.state.scene.projection,
                                   &packet.state.ui.view,&packet.state.ui.projection}) {
            for (const auto &row : matrix->rows) if (!finiteVector(row)) throw DesktopError("draw","nonfinite transform or camera");
        }
        for (const auto *value : {&packet.state.color,&packet.state.clearColor,&packet.state.scene.eye,&packet.state.ui.eye}) {
            if (!finiteVector(*value)) throw DesktopError("draw","nonfinite color or eye position");
        }
        if (!std::isfinite(packet.x) || !std::isfinite(packet.y) || !std::isfinite(packet.height)) {
            throw DesktopError("text","nonfinite text placement");
        }
        if (packet.kind == DrawPacket::Kind::Text) continue;
        if (packet.stage!=SceneStage && packet.stage!=UiStage) throw DesktopError("draw", "unsupported stage");
        if (packet.kind == DrawPacket::Kind::Model) continue;
        if (packet.faceCount<0 || packet.baseIndex<0 || packet.baseVertex<0) throw DesktopError("draw", "negative geometry range");
        const auto first=static_cast<std::size_t>(packet.baseIndex), count=static_cast<std::size_t>(packet.faceCount)*3;
        if (first>indexCount || count>indexCount-first) throw DesktopError("draw", "index span exceeds uploaded geometry");
        for (std::size_t i=first; i<first+count; ++i) {
            if (static_cast<std::size_t>(packet.baseVertex)+indices[i]>=vertexCount) throw DesktopError("draw", "index references a missing vertex");
        }
    }
}
float TextRenderer::CalculateWidth(const std::string &text, float height) const {
    if (!host) throw DesktopError("text", "renderer is not initialized");
    return host->measureText(text, height);
}
void TextRenderer::RenderText(const std::string &text, float x, float y, float height) {
    if (!host) throw DesktopError("text", "renderer is not initialized");
    host->drawText(text,x,y,height,color);
}
}
