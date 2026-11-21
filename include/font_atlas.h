#ifndef ENGINE_SIM_FONT_ATLAS_H
#define ENGINE_SIM_FONT_ATLAS_H
#include "desktop_host.h"
#include <array>
#include <filesystem>
#include <vector>

namespace desktop {
class FontAtlas {
public:
    struct Glyph { Vector2 p0, p1, uv0, uv1; float advance = 0; };
    static constexpr int Size = 1024, FontHeight = 64, FirstGlyph = 32, GlyphCount = 96;
    void load(const std::filesystem::path &path);
    float measure(const std::string &text, float height) const;
    const Glyph &glyph(unsigned char character) const;
    const std::vector<std::uint8_t> &pixels() const { return bitmap; }
private:
    std::array<Glyph, GlyphCount> glyphs{};
    std::vector<std::uint8_t> bitmap;
};
}
#endif
