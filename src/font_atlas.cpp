// Font packing/metrics follow Delta Studio's Font/LoadFont, MIT, Ange Yaghi.
// The original stb 1.24 rasterizer retains its complete MIT notice.
#include "../include/font_atlas.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../include/third_party/stb_truetype.h"
#include <cmath>
#include <fstream>

namespace desktop {
void FontAtlas::load(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() != 18336) throw engine_sim::ResourceError(path, "missing or unsupported original Silkscreen font");
    std::vector<unsigned char> bytes(18336);
    stream.seekg(0); stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    std::uint64_t identity = 14695981039346656037ULL;
    for (auto byte : bytes) identity = (identity ^ byte) * 1099511628211ULL;
    // This backend supports the inherited Silkscreen file and its recorded fingerprint.
    if (!stream || identity != 0x2e94f570a0502eefULL) throw engine_sim::ResourceError(path, "original Silkscreen font bytes differ");
    std::vector<std::uint8_t> candidate(Size * Size);
    std::array<stbtt_packedchar, GlyphCount> packed{};
    stbtt_pack_context context{};
    if (!stbtt_PackBegin(&context, candidate.data(), Size, Size, 0, 16, nullptr)) {
        throw DesktopError("font", "atlas allocation failed");
    }
    const bool packedAll = stbtt_PackFontRange(&context, bytes.data(), 0, FontHeight, FirstGlyph, GlyphCount, packed.data()) != 0;
    stbtt_PackEnd(&context);
    if (!packedAll) throw DesktopError("font", "original glyphs do not fit the atlas");
    std::array<Glyph, GlyphCount> result;
    for (int i = 0; i < GlyphCount; ++i) {
        float x = 0, y = 0;
        stbtt_aligned_quad quad{};
        stbtt_GetPackedQuad(packed.data(), Size, Size, i, &x, &y, &quad, 1);
        result[i] = {{quad.x0, quad.y0}, {quad.x1, quad.y1}, {quad.s0, quad.t0}, {quad.s1, quad.t1}, x};
    }
    glyphs = result; bitmap.swap(candidate);
}
const FontAtlas::Glyph &FontAtlas::glyph(unsigned char character) const {
    if (bitmap.empty()) throw DesktopError("font", "atlas is not initialized");
    if (character < FirstGlyph || character >= FirstGlyph + GlyphCount) {
        throw DesktopError("text", "character is outside the original font glyph range");
    }
    return glyphs[character - FirstGlyph];
}
float FontAtlas::measure(const std::string &text, float height) const {
    if (!std::isfinite(height) || height < 0) throw DesktopError("text", "invalid text height");
    const float scale = height / FontHeight;
    float width = 0;
    for (unsigned char character : text) width += glyph(character).advance * scale;
    return width;
}
}
