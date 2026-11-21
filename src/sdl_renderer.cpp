// SDL GPU stage/buffer conventions follow the MIT Open Engine Simulator renderer
// at 1e226ee7. Original Delta color, geometry and text conventions are retained.
#include "../include/sdl_renderer.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>

namespace desktop {
namespace {
void checked(bool value, const char *operation) { if (!value) throw DesktopError(operation, SDL_GetError()); }
template<class T> T *required(T *value, const char *operation) { checked(value != nullptr, operation); return value; }
std::vector<std::uint8_t> binary(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 1024 * 1024) throw engine_sim::ResourceError(path, "missing or oversized GPU shader");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(input.tellg())); input.seekg(0);
    input.read(reinterpret_cast<char *>(result.data()), result.size());
    if (!input) throw engine_sim::ResourceError(path, "truncated GPU shader");
    if (path.extension()==".spv") {
        const auto word=[&](std::size_t offset) {
            return static_cast<std::uint32_t>(result[offset]) | (static_cast<std::uint32_t>(result[offset+1])<<8)
                | (static_cast<std::uint32_t>(result[offset+2])<<16) | (static_cast<std::uint32_t>(result[offset+3])<<24);
        };
        if (result.size()<20 || result.size()%4 || word(0)!=0x07230203 || word(12)>1000000) {
            throw engine_sim::ResourceError(path,"invalid SPIR-V module header");
        }
        for (std::size_t offset=20;offset<result.size();) {
            const auto count=word(offset)>>16;
            if (count==0 || count>(result.size()-offset)/4) throw engine_sim::ResourceError(path,"invalid SPIR-V instruction span");
            offset+=count*4;
        }
    }
    return result;
}
struct Commands {
    SDL_GPUCommandBuffer *value;
    explicit Commands(SDL_GPUDevice *device) : value(required(SDL_AcquireGPUCommandBuffer(device), "acquire GPU commands")) {}
    ~Commands() { if (value) SDL_CancelGPUCommandBuffer(value); }
    void submit() { auto *pending=value; value=nullptr; checked(SDL_SubmitGPUCommandBuffer(pending), "submit GPU commands"); }
};
struct Buffers {
    SDL_GPUBuffer *vertices=nullptr, *indices=nullptr;
    SDL_GPUTransferBuffer *vertexUpload=nullptr, *indexUpload=nullptr;
    std::size_t vertexCapacity=0, indexCapacity=0;
    void initialize(SDL_GPUDevice *device, std::size_t v, std::size_t i) {
        vertexCapacity=v; indexCapacity=i;
        SDL_GPUBufferCreateInfo info{}; info.usage=SDL_GPU_BUFFERUSAGE_VERTEX; info.size=static_cast<Uint32>(v*sizeof(Vertex));
        vertices=required(SDL_CreateGPUBuffer(device,&info),"create vertex buffer");
        info.usage=SDL_GPU_BUFFERUSAGE_INDEX; info.size=static_cast<Uint32>(i*sizeof(std::uint16_t));
        indices=required(SDL_CreateGPUBuffer(device,&info),"create index buffer");
        SDL_GPUTransferBufferCreateInfo transfer{}; transfer.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer.size=static_cast<Uint32>(v*sizeof(Vertex));
        vertexUpload=required(SDL_CreateGPUTransferBuffer(device,&transfer),"create vertex transfer");
        transfer.size=static_cast<Uint32>(i*sizeof(std::uint16_t));
        indexUpload=required(SDL_CreateGPUTransferBuffer(device,&transfer),"create index transfer");
    }
    void release(SDL_GPUDevice *device) noexcept {
        if (vertices) SDL_ReleaseGPUBuffer(device,vertices);
        if (indices) SDL_ReleaseGPUBuffer(device,indices);
        if (vertexUpload) SDL_ReleaseGPUTransferBuffer(device,vertexUpload);
        if (indexUpload) SDL_ReleaseGPUTransferBuffer(device,indexUpload);
        *this={};
    }
    void upload(SDL_GPUDevice *device, SDL_GPUCommandBuffer *commands, const std::vector<Vertex> &v,
                const std::vector<std::uint16_t> &i) {
        if (v.size()>vertexCapacity || i.size()>indexCapacity) throw DesktopError("geometry","GPU upload exceeds its capacity");
        if (v.empty() || i.empty()) return;
        auto *data=required(SDL_MapGPUTransferBuffer(device,vertexUpload,true),"map vertices");
        std::memcpy(data,v.data(),v.size()*sizeof(Vertex)); SDL_UnmapGPUTransferBuffer(device,vertexUpload);
        data=required(SDL_MapGPUTransferBuffer(device,indexUpload,true),"map indices");
        std::memcpy(data,i.data(),i.size()*sizeof(std::uint16_t)); SDL_UnmapGPUTransferBuffer(device,indexUpload);
        auto *copy=required(SDL_BeginGPUCopyPass(commands),"begin geometry upload");
        const SDL_GPUTransferBufferLocation vs{vertexUpload,0}, is{indexUpload,0};
        const SDL_GPUBufferRegion vd{vertices,0,static_cast<Uint32>(v.size()*sizeof(Vertex))};
        const SDL_GPUBufferRegion id{indices,0,static_cast<Uint32>(i.size()*sizeof(std::uint16_t))};
        SDL_UploadToGPUBuffer(copy,&vs,&vd,true); SDL_UploadToGPUBuffer(copy,&is,&id,true); SDL_EndGPUCopyPass(copy);
    }
};
struct Range { int index=0, vertex=0, faces=0; };
struct FragmentUniforms { Vector color; std::uint32_t glyph=0, padding[3]{}; };
static_assert(sizeof(FragmentUniforms)==32);
}
struct SdlRenderer::State {
    const std::thread::id owner=std::this_thread::get_id();
    SDL_Window *window=nullptr;
    SDL_GPUDevice *device=nullptr;
    SDL_GPUGraphicsPipeline *pipeline=nullptr;
    SDL_GPUTexture *target=nullptr, *atlas=nullptr;
    SDL_GPUSampler *sampler=nullptr;
    Buffers models, generated, text;
    FontAtlas font;
    Statistics statistics;
    std::map<std::string,Range> meshes;
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    bool claimed=false;
    void requireOwner() const {
        if (owner!=std::this_thread::get_id()) throw DesktopError("GPU","renderer requires its owning thread");
    }
    void resize(int width,int height) {
        if (width<=0 || height<=0 || width>16384 || height>16384) throw DesktopError("GPU","unsupported framebuffer dimensions");
        if (target && width==statistics.width && height==statistics.height) return;
        SDL_GPUTextureCreateInfo info{}; info.type=SDL_GPU_TEXTURETYPE_2D; info.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width=width; info.height=height; info.layer_count_or_depth=1; info.num_levels=1;
        auto *candidate=required(SDL_CreateGPUTexture(device,&info),"create frame target");
        if (target) SDL_ReleaseGPUTexture(device,target);
        target=candidate; statistics.width=width; statistics.height=height;
    }
};
SdlRenderer::SdlRenderer() : state(std::make_unique<State>()) {}
SdlRenderer::~SdlRenderer() { shutdown(); }
void SdlRenderer::initialize(SDL_Window *window,const engine_sim::ResourcePaths &resources,const engine_sim::CompiledScene &scene) {
    state->requireOwner();
    if (!window || state->device) throw DesktopError("GPU initialize","missing window or duplicate initialization");
    state->font.load(resources.font());
    const auto shaders=resources.desktop/"shaders/sdl";
#ifdef __APPLE__
    constexpr auto format=SDL_GPU_SHADERFORMAT_MSL;
    constexpr const char *driver="metal", *extension="msl";
#else
    constexpr auto format=SDL_GPU_SHADERFORMAT_SPIRV;
    constexpr const char *driver="vulkan", *extension="spv";
#endif
    const auto vertexCode=binary(shaders/(std::string("engine_sim.vertex.")+extension));
    const auto fragmentCode=binary(shaders/(std::string("engine_sim.fragment.")+extension));
    if (scene.meshes.empty()) throw DesktopError("models","scene contains no meshes");
    std::vector<Vertex> modelVertices; std::vector<std::uint16_t> modelIndices;
    std::map<std::string,Range> meshRanges;
    for (const auto &mesh:scene.meshes) {
        if (mesh.name.empty() || mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size()%3
            || meshRanges.count(mesh.name) || modelVertices.size()+mesh.vertices.size()>1000000
            || modelIndices.size()+mesh.indices.size()>3000000) throw DesktopError("models","invalid or oversized mesh");
        for (auto i:mesh.indices) if (i>=mesh.vertices.size()) throw DesktopError("models","mesh index exceeds its vertices");
        meshRanges.emplace(mesh.name,Range{static_cast<int>(modelIndices.size()),static_cast<int>(modelVertices.size()),static_cast<int>(mesh.indices.size()/3)});
        for (const auto &v:mesh.vertices) modelVertices.push_back({{v[0],v[1],v[2],v[3]},{v[4],v[5]},{v[6],v[7],v[8],v[9]}});
        modelIndices.insert(modelIndices.end(),mesh.indices.begin(),mesh.indices.end());
    }
    try {
        state->device=required(SDL_CreateGPUDevice(format,true,driver),"create GPU device");
        state->window=window;
        checked(SDL_ClaimWindowForGPUDevice(state->device,window),"claim GPU window"); state->claimed=true;
        state->statistics={}; state->statistics.driver=SDL_GetGPUDeviceDriver(state->device);
        if (state->statistics.driver!=driver) throw DesktopError("GPU","unexpected graphics driver");
        SDL_GPUShaderCreateInfo shaderInfo{}; shaderInfo.code_size=vertexCode.size(); shaderInfo.code=vertexCode.data();
        shaderInfo.entrypoint="VSMain"; shaderInfo.format=format; shaderInfo.stage=SDL_GPU_SHADERSTAGE_VERTEX; shaderInfo.num_uniform_buffers=1;
        auto *vertexShader=required(SDL_CreateGPUShader(state->device,&shaderInfo),"create vertex shader");
        SDL_GPUShader *fragmentShader=nullptr;
        try {
            shaderInfo.code_size=fragmentCode.size(); shaderInfo.code=fragmentCode.data(); shaderInfo.entrypoint="PSMain";
            shaderInfo.stage=SDL_GPU_SHADERSTAGE_FRAGMENT; shaderInfo.num_samplers=1;
            fragmentShader=required(SDL_CreateGPUShader(state->device,&shaderInfo),"create fragment shader");
            const SDL_GPUVertexBufferDescription buffer{0,sizeof(Vertex),SDL_GPU_VERTEXINPUTRATE_VERTEX,0};
            const SDL_GPUVertexAttribute attributes[]{{0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(Vertex,Pos)},
                {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(Vertex,TexCoord)}};
            SDL_GPUColorTargetDescription color{}; color.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            color.blend_state.enable_blend=true; color.blend_state.src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; color.blend_state.color_blend_op=SDL_GPU_BLENDOP_ADD;
            color.blend_state.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_DST_ALPHA; color.blend_state.alpha_blend_op=SDL_GPU_BLENDOP_ADD;
            SDL_GPUGraphicsPipelineCreateInfo info{}; info.vertex_shader=vertexShader; info.fragment_shader=fragmentShader;
            info.vertex_input_state={&buffer,1,attributes,2}; info.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_BACK; info.rasterizer_state.front_face=SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            info.rasterizer_state.enable_depth_clip=true;
            info.target_info.color_target_descriptions=&color; info.target_info.num_color_targets=1;
            state->pipeline=required(SDL_CreateGPUGraphicsPipeline(state->device,&info),"create graphics pipeline");
        } catch (...) {
            SDL_ReleaseGPUShader(state->device,vertexShader);
            if (fragmentShader) SDL_ReleaseGPUShader(state->device,fragmentShader);
            throw;
        }
        SDL_ReleaseGPUShader(state->device,vertexShader); SDL_ReleaseGPUShader(state->device,fragmentShader);
        state->models.initialize(state->device,modelVertices.size(),modelIndices.size());
        state->generated.initialize(state->device,100000,200000); state->text.initialize(state->device,65536,98304);
        state->meshes=std::move(meshRanges);
        SDL_GPUTextureCreateInfo atlasInfo{}; atlasInfo.type=SDL_GPU_TEXTURETYPE_2D; atlasInfo.format=SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        atlasInfo.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER; atlasInfo.width=FontAtlas::Size; atlasInfo.height=FontAtlas::Size;
        atlasInfo.layer_count_or_depth=1; atlasInfo.num_levels=1;
        state->atlas=required(SDL_CreateGPUTexture(state->device,&atlasInfo),"create glyph atlas");
        SDL_GPUSamplerCreateInfo samplerInfo{}; samplerInfo.min_filter=samplerInfo.mag_filter=SDL_GPU_FILTER_LINEAR;
        samplerInfo.address_mode_u=samplerInfo.address_mode_v=samplerInfo.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        state->sampler=required(SDL_CreateGPUSampler(state->device,&samplerInfo),"create glyph sampler");
        Commands commands(state->device); state->models.upload(state->device,commands.value,modelVertices,modelIndices);
        SDL_GPUTransferBufferCreateInfo transferInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,FontAtlas::Size*FontAtlas::Size,0};
        auto *transfer=required(SDL_CreateGPUTransferBuffer(state->device,&transferInfo),"create atlas transfer");
        try {
            auto *data=required(SDL_MapGPUTransferBuffer(state->device,transfer,false),"map glyph atlas");
            std::memcpy(data,state->font.pixels().data(),state->font.pixels().size()); SDL_UnmapGPUTransferBuffer(state->device,transfer);
            auto *copy=required(SDL_BeginGPUCopyPass(commands.value),"begin atlas upload");
            const SDL_GPUTextureTransferInfo source{transfer,0,FontAtlas::Size,FontAtlas::Size};
            const SDL_GPUTextureRegion target{state->atlas,0,0,0,0,0,FontAtlas::Size,FontAtlas::Size,1};
            SDL_UploadToGPUTexture(copy,&source,&target,false); SDL_EndGPUCopyPass(copy); commands.submit();
        } catch (...) { SDL_ReleaseGPUTransferBuffer(state->device,transfer); throw; }
        SDL_ReleaseGPUTransferBuffer(state->device,transfer);
    } catch (...) { shutdown(); throw; }
}
void SdlRenderer::shutdown() noexcept {
    if (state->owner!=std::this_thread::get_id()) std::terminate();
    if (state->device) {
        SDL_WaitForGPUIdle(state->device);
        state->models.release(state->device); state->generated.release(state->device); state->text.release(state->device);
        if (state->target) SDL_ReleaseGPUTexture(state->device,state->target);
        if (state->atlas) SDL_ReleaseGPUTexture(state->device,state->atlas);
        if (state->sampler) SDL_ReleaseGPUSampler(state->device,state->sampler);
        if (state->pipeline) SDL_ReleaseGPUGraphicsPipeline(state->device,state->pipeline);
        if (state->claimed) SDL_ReleaseWindowFromGPUDevice(state->device,state->window);
        SDL_DestroyGPUDevice(state->device);
    }
    state->device=nullptr; state->window=nullptr; state->target=state->atlas=nullptr; state->sampler=nullptr; state->pipeline=nullptr;
    state->claimed=false; state->meshes.clear(); state->vertices.clear(); state->indices.clear();
}
void SdlRenderer::uploadGeometry(const Vertex *vertices,std::size_t vertexCount,const std::uint16_t *indices,std::size_t indexCount) {
    state->requireOwner();
    if (vertexCount>100000 || indexCount>200000 || (vertexCount&&!vertices) || (indexCount&&!indices)) throw DesktopError("geometry","invalid generated upload");
    state->vertices.clear(); state->indices.clear();
    if (vertexCount) state->vertices.assign(vertices,vertices+vertexCount);
    if (indexCount) state->indices.assign(indices,indices+indexCount);
}
void SdlRenderer::render(const Host &host,const FrameState &frame,bool present) {
    state->requireOwner();
    if (!state->device) throw DesktopError("render","GPU is not initialized");
    if (!frame.window.open || frame.window.framebufferWidth==0 || frame.window.framebufferHeight==0) return;
    if (frame.window.gameWidth<=0 || frame.window.gameHeight<=0) throw DesktopError("render","invalid logical dimensions");
    host.validateGeometry(state->vertices.data(),state->vertices.size(),state->indices.data(),state->indices.size());
    struct Item { const DrawPacket *packet; const Buffers *buffers; Range range; };
    std::vector<Item> items;
    std::vector<Vertex> textVertices;
    std::vector<std::uint16_t> textIndices;
    std::uint64_t glyphCount=0;
    for (const auto &packet:host.drawPackets()) {
        if (packet.kind==DrawPacket::Kind::Model) {
            const auto found=state->meshes.find(packet.model);
            if (found==state->meshes.end()) throw DesktopError("model","missing GPU mesh "+packet.model);
            items.push_back({&packet,&state->models,found->second});
        } else if (packet.kind==DrawPacket::Kind::Generated) {
            items.push_back({&packet,&state->generated,{packet.baseIndex,packet.baseVertex,packet.faceCount}});
        } else {
            if (packet.height<0) throw DesktopError("text","invalid glyph height");
            const auto firstIndex=static_cast<int>(textIndices.size());
            const float scale=packet.height/FontAtlas::FontHeight;
            float x=packet.x,y=packet.y;
            for (unsigned char character:packet.text) {
                if (character=='\n') { y-=packet.height; x=packet.x; continue; }
                if (textVertices.size()+4>65536) throw DesktopError("text","frame exceeds the glyph buffer");
                const auto &glyph=state->font.glyph(character);
                const auto base=static_cast<std::uint16_t>(textVertices.size());
                const float x0=glyph.p0.x*scale+x,x1=glyph.p1.x*scale+x;
                const float y0=-glyph.p0.y*scale+y,y1=-glyph.p1.y*scale+y;
                textVertices.insert(textVertices.end(),{{{x0,y0,-1,1},{glyph.uv0.x,glyph.uv0.y},{0,0,1,0}},
                    {{x1,y0,-1,1},{glyph.uv1.x,glyph.uv0.y},{0,0,1,0}},
                    {{x1,y1,-1,1},{glyph.uv1.x,glyph.uv1.y},{0,0,1,0}},
                    {{x0,y1,-1,1},{glyph.uv0.x,glyph.uv1.y},{0,0,1,0}}});
                for (int offset:{2,1,0,3,2,0}) textIndices.push_back(static_cast<std::uint16_t>(base+offset));
                x+=glyph.advance*scale; ++glyphCount;
            }
            items.push_back({&packet,&state->text,{firstIndex,0,static_cast<int>((textIndices.size()-firstIndex)/3)}});
        }
    }
    std::stable_sort(items.begin(),items.end(),[](const Item &a,const Item &b) {
        if (a.packet->stage!=b.packet->stage) return a.packet->stage<b.packet->stage;
        return a.packet->layer<b.packet->layer;
    });
    state->resize(frame.window.framebufferWidth,frame.window.framebufferHeight);
    Commands commands(state->device);
    state->generated.upload(state->device,commands.value,state->vertices,state->indices);
    state->text.upload(state->device,commands.value,textVertices,textIndices);
    SDL_GPUColorTargetInfo color{}; color.texture=state->target;
    const auto &clear=host.drawing.clearColor;
    color.clear_color={clear.x,clear.y,clear.z,clear.w}; color.load_op=SDL_GPU_LOADOP_CLEAR; color.store_op=SDL_GPU_STOREOP_STORE;
    auto *pass=required(SDL_BeginGPURenderPass(commands.value,&color,1,nullptr),"begin frame rendering");
    SDL_BindGPUGraphicsPipeline(pass,state->pipeline);
    const SDL_GPUTextureSamplerBinding atlasBinding{state->atlas,state->sampler};
    SDL_BindGPUFragmentSamplers(pass,0,&atlasBinding,1);
    std::uint64_t draws=0;
    for (const auto &item:items) {
        if (item.range.faces==0) continue;
        const auto &packet=*item.packet;
        const auto &camera=packet.stage==SceneStage?packet.state.scene:packet.state.ui;
        const Matrix matrices[]{packet.kind==DrawPacket::Kind::Text?math::LoadIdentity():packet.state.transform,camera.view,camera.projection};
        const FragmentUniforms fragment{packet.state.color,packet.kind==DrawPacket::Kind::Text?1U:0U,{}};
        SDL_PushGPUVertexUniformData(commands.value,0,matrices,sizeof(matrices));
        SDL_PushGPUFragmentUniformData(commands.value,0,&fragment,sizeof(fragment));
        const float scaleX=static_cast<float>(frame.window.framebufferWidth)/frame.window.gameWidth;
        const float scaleY=static_cast<float>(frame.window.framebufferHeight)/frame.window.gameHeight;
        const auto &view=packet.state.viewport;
        const SDL_GPUViewport viewport=packet.stage==SceneStage
            ?SDL_GPUViewport{view.x*scaleX,view.y*scaleY,view.width*scaleX,view.height*scaleY,0,1}
            :SDL_GPUViewport{0,0,static_cast<float>(frame.window.framebufferWidth),static_cast<float>(frame.window.framebufferHeight),0,1};
        if (viewport.w<=0 || viewport.h<=0) continue;
        SDL_SetGPUViewport(pass,&viewport);
        const SDL_GPUBufferBinding vb{item.buffers->vertices,0},ib{item.buffers->indices,0};
        SDL_BindGPUVertexBuffers(pass,0,&vb,1); SDL_BindGPUIndexBuffer(pass,&ib,SDL_GPU_INDEXELEMENTSIZE_16BIT);
        SDL_DrawGPUIndexedPrimitives(pass,static_cast<Uint32>(item.range.faces)*3,1,item.range.index,item.range.vertex,0); ++draws;
    }
    SDL_EndGPURenderPass(pass);
    bool presented=false;
    if (present) {
        SDL_GPUTexture *swapchain=nullptr; Uint32 width=0,height=0;
        checked(SDL_WaitAndAcquireGPUSwapchainTexture(commands.value,state->window,&swapchain,&width,&height),"acquire swapchain");
        if (swapchain) {
            SDL_GPUBlitInfo blit{};
            blit.source={state->target,0,0,0,0,static_cast<Uint32>(state->statistics.width),static_cast<Uint32>(state->statistics.height)};
            blit.destination={swapchain,0,0,0,0,width,height}; blit.load_op=SDL_GPU_LOADOP_DONT_CARE; blit.filter=SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(commands.value,&blit); presented=true;
        }
    }
    commands.submit(); ++state->statistics.submittedFrames;
    if (presented) ++state->statistics.presentedFrames;
    state->statistics.draws+=draws; state->statistics.glyphs+=glyphCount;
}
std::vector<std::uint8_t> SdlRenderer::readPixels() {
    state->requireOwner();
    if (!state->target) throw DesktopError("readback","no rendered frame is available");
    const auto width=static_cast<Uint32>(state->statistics.width),height=static_cast<Uint32>(state->statistics.height);
    const Uint32 stride=(width+63U)&~63U;
    const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,stride*height*4,0};
    auto *transfer=required(SDL_CreateGPUTransferBuffer(state->device,&info),"create readback transfer");
    SDL_GPUFence *fence=nullptr;
    try {
        Commands commands(state->device);
        auto *copy=required(SDL_BeginGPUCopyPass(commands.value),"begin frame download");
        const SDL_GPUTextureRegion source{state->target,0,0,0,0,0,width,height,1};
        const SDL_GPUTextureTransferInfo destination{transfer,0,stride,height};
        SDL_DownloadFromGPUTexture(copy,&source,&destination); SDL_EndGPUCopyPass(copy);
        auto *pending=commands.value; commands.value=nullptr;
        fence=required(SDL_SubmitGPUCommandBufferAndAcquireFence(pending),"submit frame download");
        checked(SDL_WaitForGPUFences(state->device,true,&fence,1),"wait for frame download");
        std::vector<std::uint8_t> result(static_cast<std::size_t>(width)*height*4);
        const auto *data=static_cast<const std::uint8_t *>(required(SDL_MapGPUTransferBuffer(state->device,transfer,false),"map downloaded frame"));
        for (Uint32 row=0;row<height;++row) std::memcpy(result.data()+static_cast<std::size_t>(row)*width*4,data+static_cast<std::size_t>(row)*stride*4,width*4);
        SDL_UnmapGPUTransferBuffer(state->device,transfer); SDL_ReleaseGPUFence(state->device,fence); fence=nullptr;
        SDL_ReleaseGPUTransferBuffer(state->device,transfer); return result;
    } catch (...) {
        if (fence) { SDL_WaitForGPUFences(state->device,true,&fence,1); SDL_ReleaseGPUFence(state->device,fence); }
        SDL_ReleaseGPUTransferBuffer(state->device,transfer); throw;
    }
}
float SdlRenderer::measureText(const std::string &text,float height) const { state->requireOwner(); return state->font.measure(text,height); }
const SdlRenderer::Statistics &SdlRenderer::statistics() const { state->requireOwner(); return state->statistics; }
}
