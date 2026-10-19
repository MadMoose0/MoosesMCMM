#pragma once

#include <functional>
#include <string>
#include <vector>

// optional stage on the finished grid: a wider block search, then feathering of tone edges
struct EnhanceSettings {
    bool search = true;    // the wider block search, the next three are its settings
    float grain = 0.5f;    // texture allowed on flat areas: 0 smooth only, 1 grain wanted
    float flat = 1.0f;    // how much smooth areas pick by colour, not pattern
    float protect = 0.35f;    // 0 off, higher keeps more fine texture as it was
    bool feather = true;
    bool cleanup = true;    // lone stray cells stay out of the feathering

    // set by the engine: lower while dithering mixes tones, 0 = off (dither looks like band edges)
    float featherKeep = 1.0f;

    bool operator==(const EnhanceSettings& o) const {
        return search == o.search && grain == o.grain && flat == o.flat && protect == o.protect
            && feather == o.feather && cleanup == o.cleanup && featherKeep == o.featherKeep;
    }
};

constexpr int ENHANCE_MAX_CELLS = 90000;    // biggest mural the search takes on, feathering has no limit

// what a run this size really does: no search over the limit, protection goes with it
EnhanceSettings enhanceEffective(const EnhanceSettings& s, int cells);

struct EnhanceIO {
    int gw = 0, gh = 0;
    std::vector<const unsigned char*> tiles;    // per palette entry: 16x16 rgba, nullptr = nothing to show
    std::vector<char> placeable;    // plain opaque block the stage may place
    std::vector<int> baseOf;    // plain block under the entry: itself, or the carrier of an overlay; -1 = none
    unsigned char bg[3] = { 128, 128, 128 };    // srgb colour behind see-through texels

    // target texel row y as gw*16 linear rgb + opacity, called from several threads
    std::function<void(int y, float* out)> fetchRow;
    std::function<int(int entry, int base)> rebase;    // same overlay on another base, or -1
    std::function<void(int n, const std::function<void(int)>& body)> parallelFor;
    std::function<bool()> cancelled;
    std::function<void(int ticks)> tick;
};

int enhanceTicks(int cells, const EnhanceSettings& effective);

// rewrites grid; false = cancelled or nothing to do, note only when the user needs telling
bool enhanceMural(std::vector<int>& grid, const EnhanceIO& io, const EnhanceSettings& s, std::string& note);
