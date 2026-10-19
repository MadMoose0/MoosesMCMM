// types, shared state, inline kernels + prototypes of every part -- inline variables = one copy for all files
#pragma once

#include <cstdio>
#include "engine.h"
#include "enhance.h"
#include "railbuild.h"
#include <cstring>
#include <cstdint>
#include <climits>
#include <limits>
#include <condition_variable>
#include <cmath>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <unordered_map>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <queue>
#include <functional>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/heap.h>
#else
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#endif

#include "../third_party/stb_image.h"
#include "../third_party/stb_image_resize2.h"
#include "../third_party/miniz.h"


//////////////// DEBUG OUTPUT

// 1 = stage timings + heap stats in the browser console, what the user sees stays the same
#define MMMM_VERBOSE 0

#if MMMM_VERBOSE
#define DBG_PRINT(...) printf(__VA_ARGS__)
#else

// a dead branch, not an empty macro: the call still gets type checked, timing locals give no warnings
#define DBG_PRINT(...) do { if (false) printf(__VA_ARGS__); } while (0)
#endif

// heap watermark: HEAP8.length = how big wasm memory got by now (it never shrinks), not bytes in use
#if defined(__EMSCRIPTEN__) && MMMM_VERBOSE
#define DBG_HEAP(label) do { \
    size_t _dbgHeapSz = EM_ASM_INT({ return HEAP8.length; }); \
    DBG_PRINT("[MEMORY STATS] %s: %.2f MB\n", label, (float)_dbgHeapSz / (1024.0f * 1024.0f)); \
} while (0)
#else
#define DBG_HEAP(label) do { if (false) { (void)(label); } } while (0)
#endif


//////////////// TYPES

// overlay side bits; only floor murals get connecting overlays, so row+ = south, column+ = east
inline const int OV_N = 8, OV_S = 4, OV_E = 2, OV_W = 1;

struct TextureRaw {
    int w = 0, h = 0;
    std::vector<unsigned char> data;
};

struct ExtendedBlockInfo {
    bool isPainToGet = false;
    bool deteriorates = false;
    bool moosesChoice = false;
    unsigned int groups = 0;    // a bit per filter group column of allowed_ids.csv, file order
    std::vector<unsigned char> uiRgbaArray;
};

struct ActiveBlock {
    std::string mc_id;

    // ovState = blockstate text from the csv, only for families that dont connect -- rails + dust get it from ovMask
    std::string ovId = "";
    std::string ovState = "";
    std::string family = "none";
    int ovMask = -1;    // -1 = no overlay, else OV_* bits
    int ovBaseIdx = -1;    // palette index of this block without the overlay

    std::vector<unsigned char> rgbArray;
    ColorLab meanLab;
    std::vector<ColorLab> flatLab;
    std::vector<ColorLab> sortedLab;

    unsigned char meanR = 0, meanG = 0, meanB = 0, meanA = 0;
    std::vector<unsigned char> rgbLOD8;
    std::vector<unsigned char> rgbLOD4;
};

struct OverlayState {
    std::string overlay_id;
    std::string blockstate;
    std::vector<std::string> textures;
    std::vector<int> rotations;
    std::string family;

    // the arms this variant draws -- for connecting families its identity, not a demand on neighbours
    int mask = 0;
};

struct BiomeTint {
    float gR, gG, gB, fR, fG, fB;
};


//////////////// SETTINGS
inline char imagePath[256] = "";
inline int targetWidth = 50;
inline int targetHeight = 70;
inline char muralName[128] = "my_mural";
inline bool lockAspect = true;
inline float aspectR = 0.0f;

inline int orientationIdx = 0;
inline int directionIdx = 0;
inline int directionVertIdx = 0;
inline int directionHorzIdx = 0;
inline int biomeIdx = 0;
inline int previewBgMode = 0;

inline int ditherMethod = 0;
inline float ditherIntensity = 0.3f;
inline float weightColor = 5.0f, weightSpatial = 5.0f, weightTexture = 5.0f;

inline bool enablePass2 = true;
inline int pass2ChunkSizeIdx = 8;    // whole image
inline int pass2Passes = 1;    // sweeps per revisited tile

// first pass best blocks per cell for the refinement -- a longer list prunes less, pass 1 slows down
inline const int kMaxPass2Candidates = 16;
inline int pass2Candidates = 10;

// refinement tile side in blocks per entry of the page list, 0 = the whole mural
inline const int kPass2ChunkBlocks[] = { 1, 2, 3, 4, 8, 16, 32, 64, 0 };
inline const int kPass2ChunkChoices = (int)(sizeof(kPass2ChunkBlocks) / sizeof(kPass2ChunkBlocks[0]));
bool pass2WholeImage(int idx);
int pass2ChunkBlocks(int idx, int muralW, int muralH);
inline float pass2Threshold = 0.25f;
inline float weightMSE = 5.0f;
inline float weightSSIM = 5.0f;

// group 7, experimental: the page has a box per feature, enable = any of them ticked
inline bool enableEnhance = false;
inline EnhanceSettings enhanceSettings;

inline float tweakBrightness = 0.0f;
inline float tweakContrast = 1.0f;
inline float tweakSaturation = 1.0f;
inline float tweakHue = 0.0f;
inline float tweakDenoise = 0.0f;
inline float tweakSharpen = 0.0f;


//////////////// GAME VERSIONS

// sorted, from the Version column + the overlay [version] row -- versions travel as indices into it
inline std::vector<std::string> g_gameVersions;
inline std::map<std::string, std::string> g_overlayVersionRaw;    // overlay id to version text
inline std::vector<int> g_overlayVersionIdx;    // in step with g_allOverlayIds
inline int selectedVersionIdx = 0;    // newest once loaded

// export only: mc_id keeps the newest spelling inside the engine, so every lookup matches one name
struct VersionRename {
    std::string from;    // id before the rename
    std::string to;    // newest id, what the csvs + textures use
    int sinceIdx = 0;    // first version where to is right
};
inline std::vector<VersionRename> versionRenames;


//////////////// OVERLAYS
inline bool useOverlays = false;
inline std::vector<std::string> allowedOverlayIds;
inline std::vector<std::string> disallowedOverlayIds;

inline bool cachedUseOverlays = false;
inline std::vector<std::string> cachedAllowedOverlays;


//////////////// SWITCHES

// a/b switches from measuring, set from the bridge -- the settled ones stay to time or check against
inline std::atomic<int> g_legacyOverlayPrep{ 0 };    // a/b switch, settled: off -- old overlay prep, kept to time against

// exact lower-bound prune in the pass 1 loop, must be bit-identical either way
inline std::atomic<int> g_earlyExit{ 1 };    // a/b switch, settled: on

// tested: floor on the two texture terms from the mean difference, 1.23x to 3.84x on 9 cases, bit-identical
inline std::atomic<int> g_meanFloor{ 1 };    // a/b switch, settled: on

// colour sweep reads compact arrays (~96 KB), not ~300-byte ActiveBlocks -- same floats, same order
inline std::atomic<int> g_packedSweep{ 1 };    // a/b switch, settled: on

// the mean floor on four regions for the 256-term loops -- tighter, never skips what the loop keeps
inline std::atomic<int> g_coarseBound{ 1 };    // a/b switch, settled: on

// candidates in lower-bound order, stop once the bound passes the worst kept -- ties on palette index
inline std::atomic<int> g_bestFirst{ 1 };    // a/b switch, settled: on

// tested: the region fast path wins at 2/6/8 regions, loses at 12/16/20 -- 9 to 11 not measured
inline std::atomic<int> g_maxFastRegions{ 8 };    // tuned value, settled: 8

// whole-mural composite buffer (~1 GB at 750) instead of per-chunk scratch
inline std::atomic<int> g_legacyFullComposite{ 0 };    // a/b switch, settled: off -- old whole-mural buffer, kept to compare

// 1: dithering shifts the whole cell, so the texture terms aim where the colour term does -- 0 pulled picks back
inline std::atomic<int> g_ditherWholeCell{ 1 };    // a/b switch, settled: on

// every source upscaled to 16 px per block before lab, as before the resolution-aware change
inline std::atomic<int> g_legacyUpscale{ 0 };    // a/b switch, settled: off -- old upscaling, kept to compare

// region fast path off, to check its result against the 256-term loop
inline std::atomic<int> g_legacyTextureLoop{ 0 };    // a/b switch, settled: off -- old texture loop, kept to check the fast path against

// overlay_ids.csv + overlay_states.csv per face
inline std::map<std::string, std::map<std::string, bool>> overlayMatrixTop;
inline std::map<std::string, std::map<std::string, bool>> overlayMatrixSide;
inline std::map<std::string, std::map<std::string, bool>> overlayMatrixBottom;
inline std::vector<OverlayState> overlayStatesTop;
inline std::vector<OverlayState> overlayStatesSide;
inline std::vector<OverlayState> overlayStatesBottom;


//////////////// STATE

// preview flag is set by a request + cleared by the supervisor once nothing is pending; exports hold their own
inline std::atomic<bool> previewBusy{ false };
inline std::atomic<bool> exportBusy{ false };

// kept copy of a big picture fits another mural size -- the page sends the picture again, busy until then
inline std::atomic<bool> g_sourceWanted{ false };
bool engineBusy();
inline std::atomic<long long> g_progressVal{ 0 };
#if MMMM_VERBOSE

// tested: an atomic here slowed pass 1 5-10x, so thread-local sums + one flush per row range
inline std::atomic<long long> g_cullTotal{ 0 };
inline std::atomic<long long> g_cullSurvivors{ 0 };
inline thread_local long long tl_cullTotal = 0;
inline thread_local long long tl_cullSurvivors = 0;
#endif
inline std::atomic<long long> g_progressMax{ 1 };

// bumped by work with no place on the bar (rail builder, exports) -- the page reads it to tell slow from stuck
inline std::atomic<int> g_alive{ 0 };
inline std::atomic<int> currentCalcId{ 0 };


//////////////// PROGRESS WEIGHTS
inline int wtLab = 2150;    // tested: 118 ms at 270x326 against the anchor below
inline int wtPal = 19300;    // tested: 168 ms for 379 blocks
inline int wtPass1 = 500;
inline int wtPass2Rebuild = 27;    // per cell, compositeLab rebuild (tested 2026-10-09)
inline int wtPass2Refine = 4;    // per candidate scanned in refinement (tested 2026-10-09)

// tile scoring + setup per cell; work that cant use every thread is weighed up by the idle ones
inline double WT_P2_RANK = 5.0;
inline double WT_P2_SETUP = 8.0;

// each candidate kept beyond three prunes a little less
inline double WT_P1_PER_EXTRA_CANDIDATE = 0.087;    // tested: 190 ms at three, 306 ms at ten

// pass 1 ms per unit where the weights were measured: 190 ms for 88,020 cells x 94 units, 14 threads
inline double WT_ANCHOR_MS_PER_UNIT = 190.0 / 8273880.0;

// units per weight after pass 1: its cost per unit swings 3x with the picture, the later stages dont
// kept between runs, so the guess before pass 1 starts close
inline double g_laterScale = 1.0;

// this machine against the reference one, from tile scoring (same cost per cell for any picture)
inline double g_machineFactor = 1.0;

// ms + units per phase of the last run: before pass 1, pass 1, rebuild, tile scoring, refinement, experimental
inline double g_phaseMs[6] = { 0 };
inline long long g_phaseUnits[6] = { 0 };

// per enhancement tick -- tested: search 5.8 s, feathering alone 80 ms at 270x326, pass 1 190 ms
inline int wtEnhance = 12;


//////////////// SHARED STATE
inline std::mutex dataMutex;
inline bool tweaksChanged = true;
inline bool databaseLoaded = false;
inline std::atomic<int> previewSerial{ 0 };    // bumped per committed preview
inline auto lastChangeTime = std::chrono::steady_clock::now();

inline std::vector<std::string> consoleLog;
inline std::string calcStatus[4];
inline bool hasCalcStatus = false;
inline std::string deteriorationWarning = "";

// overlay cells of the mural on screen that games before 1.16 treat another way -- text follows the version
struct OldGameCells { int rails = 0, dust = 0, onShulker = 0, onSoulSand = 0; };
inline OldGameCells g_oldGameCells;
std::string oldGameWarning();    // call with dataMutex held


//////////////// BLOCK DATA
inline std::map<std::string, TextureRaw> allFiles;
inline std::map<std::string, ExtendedBlockInfo> extBlockInfo;

// the face table: every side a block can show, as the game draws it (texture + turn)
struct FaceFix { std::string state; std::string texture; int turn = 0; bool mirror = false; };
inline std::unordered_map<std::string, std::vector<FaceFix>> blockFaceRows;    // "placement|block" to its sides in palette order

inline std::vector<BlockData> masterDatabase;
inline std::vector<std::string> g_blockGroupNames;    // filter group column headers
inline const size_t kFirstGroupColumn = 11;    // column of the first group
inline std::vector<BlockData*> allowedBlocks;
inline std::vector<BlockData*> disallowedBlocks;


//////////////// GRIDS

// one read-only palette for the cache, the preview and the exports -- swapped as a whole when a run commits
using PaletteRef = std::shared_ptr<const std::vector<ActiveBlock>>;
inline PaletteRef activePalette = std::make_shared<const std::vector<ActiveBlock>>();
inline std::vector<int> previewGridIndices;

// the engine passes result when the screen shows the enhanced grid, else empty -- a delta run starts here
// the enhanced grid isnt something pass 1 could have made, so it never goes back in
inline std::vector<int> engineGridIndices;
inline int g_committedCands = 0;    // candidate count of the grid on screen: another one = solve everything again
inline EnhanceSettings enhancedWith;    // settings the enhanced grid was made with
inline int previewWidth = 0;
inline int previewHeight = 0;
inline std::vector<unsigned char> processedData;


//////////////// PACKED LAB

// the per-texel buffers are fixed point -- tested: error 0.002 lab, 0.0009 alpha, far under 1 unit you can see
// tested: 7 bytes packed ran as fast as 8 aligned, so packed
#pragma pack(push, 1)
struct ColorLabPacked { uint16_t l; int16_t a; int16_t b; uint8_t alpha; };
#pragma pack(pop)

// ab range: real srgb extremes (-85..97, -107..94) + ~20% headroom -- these only hold 8-bit rgb
constexpr float LAB_L_SCALE = 65535.0f / 100.0f;
constexpr float LAB_AB_SCALE = 32767.0f / 120.0f;
constexpr float LAB_ALPHA_SCALE = 255.0f / 100.0f;

// multiply on every unpack, not divide -- the rounding stays far under the error above
constexpr float LAB_L_INV_SCALE = 1.0f / LAB_L_SCALE;
constexpr float LAB_AB_INV_SCALE = 1.0f / LAB_AB_SCALE;
constexpr float LAB_ALPHA_INV_SCALE = 1.0f / LAB_ALPHA_SCALE;

inline ColorLabPacked packLab(const ColorLab& c) {
    ColorLabPacked p;
    p.l = (uint16_t)std::clamp(std::lround(c.l * LAB_L_SCALE), 0L, 65535L);
    p.a = (int16_t)std::clamp(std::lround(c.a * LAB_AB_SCALE), -32767L, 32767L);
    p.b = (int16_t)std::clamp(std::lround(c.b * LAB_AB_SCALE), -32767L, 32767L);
    p.alpha = (uint8_t)std::clamp(std::lround(c.alpha * LAB_ALPHA_SCALE), 0L, 255L);
    return p;
}
inline ColorLab unpackLab(const ColorLabPacked& p) {
    return ColorLab{ p.l * LAB_L_INV_SCALE, p.a * LAB_AB_INV_SCALE, p.b * LAB_AB_INV_SCALE, p.alpha * LAB_ALPHA_INV_SCALE };
}


//////////////// PICTURE AND BUFFERS
inline std::string cachedImagePath = "";
inline int origW = 0, origH = 0;
inline std::vector<unsigned char> origData;

// origData as stored: the whole picture, or only the size the mural reads when it has over 16 px per block
inline int srcW = 0, srcH = 0;
inline int cachedTW = 0, cachedTH = 0;
inline std::vector<ColorLabPacked> cachedLabData;

// global so a size change can release them all together
inline std::vector<unsigned char> resizedData;
inline std::vector<ColorLabPacked> smallLabData;
inline std::vector<ColorLabPacked> compositeLab;
inline std::string cachedOri = "", cachedDrct = "", cachedBiome = "";
inline int cachedBgMode = -1;
inline std::vector<BlockData*> cachedAllowedBlocks;
inline PaletteRef cachedPalette = std::make_shared<const std::vector<ActiveBlock>>();

// grass rgb, then foliage rgb, per biome
inline std::map<std::string, BiomeTint> biomeTints = {
    {"Plains", {145 / 255.f, 189 / 255.f, 89 / 255.f, 119 / 255.f, 171 / 255.f, 47 / 255.f}},
    {"Forest", {121 / 255.f, 192 / 255.f, 90 / 255.f, 89 / 255.f, 174 / 255.f, 48 / 255.f}},
    {"Jungle", {89 / 255.f, 201 / 255.f, 60 / 255.f, 48 / 255.f, 187 / 255.f, 11 / 255.f}},
    {"Swamp", {106 / 255.f, 112 / 255.f, 57 / 255.f, 106 / 255.f, 112 / 255.f, 57 / 255.f}},
    {"Badlands", {144 / 255.f, 129 / 255.f, 77 / 255.f, 158 / 255.f, 129 / 255.f, 77 / 255.f}},
    {"Desert/Savanna", {191 / 255.f, 183 / 255.f, 85 / 255.f, 174 / 255.f, 164 / 255.f, 42 / 255.f}},
    {"Taiga", {134 / 255.f, 183 / 255.f, 131 / 255.f, 104 / 255.f, 164 / 255.f, 100 / 255.f}},
    {"Dark Forest", {80 / 255.f, 122 / 255.f, 50 / 255.f, 80 / 255.f, 122 / 255.f, 50 / 255.f}}
};


//////////////// THREADS
class WorkerPool {
public:
    WorkerPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
                });
        }
    }
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }
    ~WorkerPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers) worker.join();
    }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};
int getOptimalThreadCount();
WorkerPool* getPool();
void sortBlockList(std::vector<BlockData*>& list);
void Log(const std::string& msg);
std::vector<std::string> splitCSV(const std::string& line, char delim);
char sniffDelim(const std::string& headerLine);
std::pair<int, int> parseVersion(const std::string& v);
int versionPatch(const std::string& v);
std::string applyVersionRename(const std::string& id, int versionIdx);
int versionIndexOf(const std::string& v);


//////////////// COLOUR

// tested: a lookup table here (5e-8 off) still changed 63% of picks -- near ties flip on any bit, keep pow
inline float srgbToLinearExact(float c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline void convertPixelToLAB(float r, float g, float b, float& l_out, float& a_out, float& b_out) {
    r = srgbToLinearExact(r);
    g = srgbToLinearExact(g);
    b = srgbToLinearExact(b);

    float x = (r * 0.4124564f + g * 0.3575761f + b * 0.1804375f) / 0.95047f;
    float y = (r * 0.2126729f + g * 0.7151522f + b * 0.0721750f) / 1.00000f;
    float z = (r * 0.0193339f + g * 0.1191920f + b * 0.9503041f) / 1.08883f;

    float fx = (x > 0.008856f) ? std::cbrt(x) : (7.787f * x) + (16.0f / 116.0f);
    float fy = (y > 0.008856f) ? std::cbrt(y) : (7.787f * y) + (16.0f / 116.0f);
    float fz = (z > 0.008856f) ? std::cbrt(z) : (7.787f * z) + (16.0f / 116.0f);

    l_out = (116.0f * fy) - 16.0f;
    a_out = 500.0f * (fx - fy);
    b_out = 200.0f * (fy - fz);
}

// the way back from convertPixelToLAB, stopping at linear light
inline void labToLinearRgb(float l, float a, float b, float* rgb) {
    auto undo = [](float f) {
        float f3 = f * f * f;
        return (f3 > 0.008856f) ? f3 : (f - 16.0f / 116.0f) / 7.787f;
        };
    float fy = (l + 16.0f) / 116.0f;
    float x = undo(fy + a / 500.0f) * 0.95047f;
    float y = undo(fy);
    float z = undo(fy - b / 200.0f) * 1.08883f;
    rgb[0] = std::clamp(3.2404542f * x - 1.5371385f * y - 0.4985314f * z, 0.0f, 1.0f);
    rgb[1] = std::clamp(-0.9692660f * x + 1.8760108f * y + 0.0415560f * z, 0.0f, 1.0f);
    rgb[2] = std::clamp(0.0556434f * x - 0.2040259f * y + 1.0572252f * z, 0.0f, 1.0f);
}
std::vector<ColorLab> convertRGBtoLAB(const unsigned char* rgbData, int width, int height);
void fastSeparableBlur(const std::vector<unsigned char>& src, std::vector<unsigned char>& dst, int w, int h, int radius);
std::vector<unsigned char> applyImageTweaks(const std::vector<unsigned char>& src, int w, int h);


//////////////// ERROR METRICS

// mse + ssim on the packed ints: the encoding is linear, ssim constants scaled by scale^2 = same ratio
// stabilisers of the paper for lightness 0-100: (0.01 * 100)^2 + (0.03 * 100)^2
constexpr double SSIM_C1_PACKED = 1.0 * (double)LAB_L_SCALE * (double)LAB_L_SCALE;
constexpr double SSIM_C2_PACKED = 9.0 * (double)LAB_L_SCALE * (double)LAB_L_SCALE;

// four accumulators: one sum waits on every add, and clang never reorders a float sum to vectorise it
// another order than a plain loop = last-bit differences, the near-tie kind the region fast path has too
inline float sumTexelBlock64(const ColorLab* B, const ColorLab* S, float alphaWeight) {
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    for (int i = 0; i < 64; i += 4) {
        float dl0 = B[i].l - S[i].l, da0 = B[i].a - S[i].a, db0 = B[i].b - S[i].b, dA0 = B[i].alpha - S[i].alpha;
        float dl1 = B[i + 1].l - S[i + 1].l, da1 = B[i + 1].a - S[i + 1].a, db1 = B[i + 1].b - S[i + 1].b, dA1 = B[i + 1].alpha - S[i + 1].alpha;
        float dl2 = B[i + 2].l - S[i + 2].l, da2 = B[i + 2].a - S[i + 2].a, db2 = B[i + 2].b - S[i + 2].b, dA2 = B[i + 2].alpha - S[i + 2].alpha;
        float dl3 = B[i + 3].l - S[i + 3].l, da3 = B[i + 3].a - S[i + 3].a, db3 = B[i + 3].b - S[i + 3].b, dA3 = B[i + 3].alpha - S[i + 3].alpha;
        a0 += (dl0 * dl0) + (da0 * da0) + (db0 * db0) + (dA0 * dA0 * alphaWeight);
        a1 += (dl1 * dl1) + (da1 * da1) + (db1 * db1) + (dA1 * dA1 * alphaWeight);
        a2 += (dl2 * dl2) + (da2 * da2) + (db2 * db2) + (dA2 * dA2 * alphaWeight);
        a3 += (dl3 * dl3) + (da3 * da3) + (db3 * db3) + (dA3 * dA3 * alphaWeight);
    }
    return (a0 + a1) + (a2 + a3);
}

// as above, colour error faded by the target alpha
inline float sumTexelBlock64Transparent(const ColorLab* B, const ColorLab* S, float alphaWeight) {
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    for (int i = 0; i < 64; i += 4) {
        float c0 = S[i].alpha / 100.0f, c1 = S[i + 1].alpha / 100.0f, c2 = S[i + 2].alpha / 100.0f, c3 = S[i + 3].alpha / 100.0f;
        float dl0 = (B[i].l - S[i].l) * c0, da0 = (B[i].a - S[i].a) * c0, db0 = (B[i].b - S[i].b) * c0, dA0 = B[i].alpha - S[i].alpha;
        float dl1 = (B[i + 1].l - S[i + 1].l) * c1, da1 = (B[i + 1].a - S[i + 1].a) * c1, db1 = (B[i + 1].b - S[i + 1].b) * c1, dA1 = B[i + 1].alpha - S[i + 1].alpha;
        float dl2 = (B[i + 2].l - S[i + 2].l) * c2, da2 = (B[i + 2].a - S[i + 2].a) * c2, db2 = (B[i + 2].b - S[i + 2].b) * c2, dA2 = B[i + 2].alpha - S[i + 2].alpha;
        float dl3 = (B[i + 3].l - S[i + 3].l) * c3, da3 = (B[i + 3].a - S[i + 3].a) * c3, db3 = (B[i + 3].b - S[i + 3].b) * c3, dA3 = B[i + 3].alpha - S[i + 3].alpha;
        a0 += (dl0 * dl0) + (da0 * da0) + (db0 * db0) + (dA0 * dA0 * alphaWeight);
        a1 += (dl1 * dl1) + (da1 * da1) + (db1 * db1) + (dA1 * dA1 * alphaWeight);
        a2 += (dl2 * dl2) + (da2 * da2) + (db2 * db2) + (dA2 * dA2 * alphaWeight);
        a3 += (dl3 * dl3) + (da3 * da3) + (db3 * db3) + (dA3 * dA3 * alphaWeight);
    }
    return (a0 + a1) + (a2 + a3);
}

// renderedBase = the chunk top-left, renderedStride its row step: full composite or chunk scratch, same maths
inline float calculateSubMatrixMSE(const std::vector<ColorLabPacked>& origLab,
    const ColorLabPacked* renderedBase, int renderedStride,
    int targetW_px, int startX, int startY, int width_px, int height_px, int bgMode,
    int ticksPerBlockRow = 0, const int32_t* cellShift = nullptr, int gridW = 0) {

    // ticksPerBlockRow: progress after every 16 texel rows, 0 = the caller ticks
    // cellShift: l, a, b dithering added per cell, or null -- colour error against the picture dithering asked for
    double sumErr = 0.0;
    int totalPx = width_px * height_px;
    float alphaWeight = (bgMode == 3) ? 4.0f : 1.5f;

    // one texel; sl, sa, sb = what dithering added to its cell, 0 without
    auto texel = [&](const ColorLabPacked& o, const ColorLabPacked& r, int sl, int sa, int sb) -> float {
        float colorImp = (bgMode == 3) ? (o.alpha / 255.0f) : 1.0f;    // transparent mode: colour counts less where the picture is see-through

        float dL = (o.l + sl - r.l) * LAB_L_INV_SCALE * colorImp;
        float dA = (o.a + sa - r.a) * LAB_AB_INV_SCALE * colorImp;
        float dB = (o.b + sb - r.b) * LAB_AB_INV_SCALE * colorImp;
        float dAlpha = (o.alpha - r.alpha) * LAB_ALPHA_INV_SCALE;

        return dL * dL + dA * dA + dB * dB + (dAlpha * dAlpha * alphaWeight);
        };

    for (int dy = 0; dy < height_px; ++dy) {
        const ColorLabPacked* o = origLab.data() + (size_t)(startY + dy) * targetW_px + startX;
        const ColorLabPacked* r = renderedBase + (size_t)dy * renderedStride;
        if (!cellShift) {
            for (int dx = 0; dx < width_px; ++dx) sumErr += texel(o[dx], r[dx], 0, 0, 0);
        }
        else {
            const int32_t* rowShift = cellShift + 3 * ((size_t)((startY + dy) >> 4) * gridW);
            for (int dx = 0; dx < width_px; ++dx) {
                const int32_t* sh = rowShift + 3 * ((startX + dx) >> 4);
                sumErr += texel(o[dx], r[dx], sh[0], sh[1], sh[2]);
            }
        }
        if (ticksPerBlockRow && (dy & 15) == 15) g_progressVal += ticksPerBlockRow;
    }
    return (float)(sumErr / (totalPx * 4.0 * 10000.0));
}

// renderedBase/renderedStride as in calculateSubMatrixMSE
inline float calculateSubMatrixSSIM(const std::vector<ColorLabPacked>& origLab,
    const ColorLabPacked* renderedBase, int renderedStride,
    int targetW_px, int startX, int startY, int width_px, int height_px,
    int ticksPerBlockRowA = 0, int ticksPerBlockRowB = 0) {

    // one tick amount for each of the two sweeps over the texels
    double meanX = 0.0, meanY = 0.0;
    int totalPx = width_px * height_px;

    for (int dy = 0; dy < height_px; ++dy) {
        for (int dx = 0; dx < width_px; ++dx) {
            int idx = (startY + dy) * targetW_px + (startX + dx);
            meanX += origLab[idx].l;
            meanY += renderedBase[dy * renderedStride + dx].l;
        }
        if (ticksPerBlockRowA && (dy & 15) == 15) g_progressVal += ticksPerBlockRowA;
    }
    meanX /= totalPx; meanY /= totalPx;

    double varX = 0.0, varY = 0.0, covXY = 0.0;
    for (int dy = 0; dy < height_px; ++dy) {
        for (int dx = 0; dx < width_px; ++dx) {
            int idx = (startY + dy) * targetW_px + (startX + dx);
            double diffX = origLab[idx].l - meanX;
            double diffY = renderedBase[dy * renderedStride + dx].l - meanY;
            varX += diffX * diffX;
            varY += diffY * diffY;
            covXY += diffX * diffY;
        }
        if (ticksPerBlockRowB && (dy & 15) == 15) g_progressVal += ticksPerBlockRowB;
    }
    varX /= totalPx; varY /= totalPx; covXY /= totalPx;

    double num = (2.0 * meanX * meanY + SSIM_C1_PACKED) * (2.0 * covXY + SSIM_C2_PACKED);
    double den = (meanX * meanX + meanY * meanY + SSIM_C1_PACKED) * (varX + varY + SSIM_C2_PACKED);

    return (float)(num / den);
}


//////////////// PALETTE
std::vector<float> getBiomeTintVals(const std::string& baseName, const std::string& biomeName);
std::vector<int> getPreviewBgColor(int mode, const std::string& biomeName);
void loadBlockFaces(const std::string& filepath);
void loadVersionRenames(const std::string& filepath);
void loadOverlayDatabase();
void setDroppedImage(const char* path);
void loadBlockDatabase();
std::vector<ActiveBlock> buildActivePalette(const std::string& ori, const std::string& drct, const std::string& biomeName, const std::vector<BlockData*>& activeBlocks, int bgMode, bool optUseOverlays, const std::vector<std::string>& optAllowedOverlays, bool isDelta);
bool leverPowersThrough(const std::string& id);
bool reactsToPower(const std::string& id);
bool fallsDown(const std::string& id);
bool overlayFitsOnTop(const std::string& rawId, const char* overlayId);


//////////////// RAILS AND POWER
inline const char* const RAIL_KIND_IDS[6] = { "", "rail", "powered_rail", "activator_rail", "detector_rail", "redstone_wire" };
uint8_t railKindOf(const std::string& ovId);
uint8_t railShapeOfMask(int m);
int maskOfRailShape(uint8_t s);
uint8_t dustOfMask(int m);
int maskOfDust(uint8_t d);

struct OverlayVariantIndex {
    std::unordered_map<std::string, std::array<int, 16> > slots;    // key: base mc_id + 0x1f + overlay id, palette index per mask
    std::vector<uint8_t> kind;    // railbuild::Kind of its overlay, NONE otherwise
    std::vector<uint8_t> base;    // the BASE_* bits of the block itself
    bool any = false;    // rails or dust in the palette at all
};
enum : uint8_t { BASE_HOLDS_RAIL = 1, BASE_CONDUCTS = 2, BASE_POWERED = 4, BASE_REACTS = 8 };
OverlayVariantIndex buildOverlayVariantIndex(const std::vector<ActiveBlock>& palette);

// moves that build the rails + dust of one grid -- exports replay them, the game would reshape finished shapes
struct RailPlan {
    int w = 0, h = 0;
    std::vector<railbuild::Op> ops;
    int rails = 0, dust = 0, reshaped = 0, dropped = 0, helpers = 0, levers = 0, leverVisits = 0, mismatched = 0;
    std::vector<uint8_t> wishKind, wishShape, flags;    // what the builder was asked for per cell, diagnostics only
};

// the solver wish per cell, kept while the cell still shows what was built from it
// without it a second settling takes built shapes for wishes and the mural drifts per recalculation
// bits: 0x8000 on record, 0x4000 left out, 0x0700 kind, 0x00F0 wished mask, 0x000F built mask
typedef std::vector<uint16_t> RailWishes;
void resolveOverlayGeometry(std::vector<int>& grid, int tW, int tH,
    const std::vector<ActiveBlock>& palette, const OverlayVariantIndex& index,
    RailWishes& wishes, RailPlan& plan);

// pistons, tnt, bulbs, lamps, crafters, trapdoors: never where something powers them; pumpkins: no golems
enum : uint8_t { PWR_VICTIM = 1, PWR_SOURCE = 2, PWR_OBSERVER = 4, GOLEM_HEAD = 8, GOLEM_COPPER = 16, GOLEM_SNOW = 32, GOLEM_IRON = 64 };
std::vector<uint8_t> powerClasses(const std::vector<ActiveBlock>& palette);
int forbidPoweredBlocks(std::vector<int>& grid, int tW, int tH, const std::vector<ActiveBlock>& palette,
    const std::vector<uint8_t>& cls, const std::vector<int>& cands, int stride);

// wishes behind the committed engine grid + the build plan of the grid on screen, both under dataMutex
inline RailWishes committedRailWishes;
inline RailPlan previewRailPlan;
std::string railPlanNote(const RailPlan& p);
std::string overlayStateForMask(const ActiveBlock& b);
std::string overlayBlockString(const ActiveBlock& b);


//////////////// OVERLAY SCORING

// lab units of colour an overlay has to win by; pass 2 scores in mse/ssim, so scaled
inline std::atomic<float> g_overlayBias{ 2.0f };
inline const float OVERLAY_P2_SCALE = 0.06f;    // mse/ssim units per lab unit


//////////////// DIAGNOSTICS

// only for measuring the cull: a page sweep compares margins against a huge one, 15 ships
inline std::atomic<float> g_cullMargin{ 15.0f };

// one cell whose whole pass 1 ranking goes to the console, -1 = none
inline std::atomic<int> g_debugRow{ -1 };
inline std::atomic<int> g_debugCol{ -1 };

// cull cost in one run: winners with + without the cull counted -- two runs never compared cleanly
inline std::atomic<int> g_cullAudit{ 0 };    // diagnostic counter switch, off
inline std::atomic<long long> g_cullAuditCells{ 0 };
inline std::atomic<long long> g_cullAuditDisagree{ 0 };

#if MMMM_VERBOSE

// time in the pass 1 sort + the pass 2 mse/ssim rescans, ns as integers (atomic double needs c++20)
inline std::atomic<long long> g_sortTimeNs{ 0 };
inline std::atomic<long long> g_mseSsimTimeNs{ 0 };
inline thread_local long long tl_sortTimeNs = 0;
inline thread_local long long tl_mseSsimTimeNs = 0;
#endif
void abandonPreview(const std::string& why);


//////////////// BUFFERS

// release only above twice the need: resize() never gives memory back, a release every time fragments the heap
// tested: freeing before the new allocation helped one step (3,544 to 2,707 MB), a growing chain peaked at 3,978
// this way plateaus at 3,544 MB, and dragging the size up is the common case
template<typename T>
void resizeReclaiming(std::vector<T>& buf, size_t newSize) {
    if (buf.capacity() > newSize * 2) {
        std::vector<T>().swap(buf);
    }
    buf.resize(newSize);
}


//////////////// PREVIEW RUN

// what one calculation leaves for the next: trackers that tell a changed setting, grid buffers that get reused
struct PreviewCarry {

    // cancelled run = no grid, but trackers + picture buffers hold its state -- the next run starts from scratch
    bool lastRunCommitted = true;
    float lastWC = -1, lastWS = -1, lastWT = -1;
    bool lastP2Enabled = false;
    int lastP2Chunk = -1;
    float lastP2Thresh = -1.0f, lastP2MSE = -1.0f, lastP2SSIM = -1.0f;
    int lastP2Passes = -1;
    int lastDMeth = -1;
    float lastDInt = -1.0f;
    int lastDitherWholeCell = -1;
    std::vector<int> tempGridIndices;

    // what the solver wished for where tempGridIndices shows a built rail or dust
    RailWishes tempRailWishes;
    std::vector<float> bestScores;

    // p2Cands palette indices per cell, best first, -1 for an empty slot
    std::vector<int> cellCandidates;
    int candStride = 0;    // how many per cell the two arrays are laid out for right now
    std::vector<ColorLab> errorGrid;

    // what dithering added to each cell in pass 1, packed like the picture -- empty or zero without
    std::vector<int32_t> ditherShift;

    // candidates of the last committed run: a delta run starts from these, not from a cancelled run
    std::vector<int> previewCellCandidates;
    int lastGridW = 0, lastGridH = 0;
};
inline PreviewCarry g_carry;

struct Pass2Weights { int rebuildCell, rankCell, refineScan, setupCell; };

// one calculation of the preview, phase by phase -- members are what a phase hands on to the later ones
struct PreviewRun {

    // carried state under the names the phases know it by
    bool& lastRunCommitted = g_carry.lastRunCommitted;
    float& lastWC = g_carry.lastWC;
    float& lastWS = g_carry.lastWS;
    float& lastWT = g_carry.lastWT;
    bool& lastP2Enabled = g_carry.lastP2Enabled;
    int& lastP2Chunk = g_carry.lastP2Chunk;
    float& lastP2Thresh = g_carry.lastP2Thresh;
    float& lastP2MSE = g_carry.lastP2MSE;
    float& lastP2SSIM = g_carry.lastP2SSIM;
    int& lastP2Passes = g_carry.lastP2Passes;
    int& lastDMeth = g_carry.lastDMeth;
    float& lastDInt = g_carry.lastDInt;
    int& lastDitherWholeCell = g_carry.lastDitherWholeCell;
    std::vector<int>& tempGridIndices = g_carry.tempGridIndices;
    RailWishes& tempRailWishes = g_carry.tempRailWishes;
    std::vector<float>& bestScores = g_carry.bestScores;
    std::vector<int>& cellCandidates = g_carry.cellCandidates;
    int& candStride = g_carry.candStride;
    std::vector<ColorLab>& errorGrid = g_carry.errorGrid;
    std::vector<int32_t>& ditherShift = g_carry.ditherShift;
    std::vector<int>& previewCellCandidates = g_carry.previewCellCandidates;
    int& lastGridW = g_carry.lastGridW;
    int& lastGridH = g_carry.lastGridH;

    // from readSettings
    std::chrono::high_resolution_clock::time_point t_preview_start;
    int myCalcId;
    int tW, tH, oIdx, dIdx, bIdx, dMeth, currentBgMode;
    float wC, wS, wT, dInt;
    std::vector<BlockData*> localAllowedBlocks;
    bool localUseOverlays;
    std::vector<std::string> localAllowedOverlays;
    bool p2Enabled;
    int p2Chunk;
    float p2Thresh, p2MSE, p2SSIM;
    int p2Passes, p2Cands;
    bool enhOn;
    EnhanceSettings enhSet;

    // from prepareImage
    int targetW_px, targetH_px;
    int Neff_x;
    int Neff_y;
    int labW_px, labH_px;

    // from planWork
    std::string biomeStr;
    std::string activeBiomeStr;
    std::string oriStr;
    std::string drctStr;
    double phaseMs[6] = { 0 };
    long long phaseUnits[6] = { 0 };
    std::chrono::high_resolution_clock::time_point phaseClock;
    long long phaseVal;
    bool needsLab;
    int labResizeTickWeight;
    int labRowTickWeight;
    bool needsLabExpand;
    int labExpandTickWeight;
    bool fullPaletteRebuild;
    int ditherWholeCell;
    bool fullGridRecalc;
    long long pass1BudgetUnits;
    int p2TilesX, p2TilesY;
    int p2TilesToRefine;
    long long p2CellsGuess;
    long long p2ScansGuess;
    Pass2Weights p2w;
    int wEnh;
    long long pass2BudgetUnits;
    EnhanceSettings enhEff;
    bool enhRuns;
    long long enhanceBudgetUnits;

    // from prepareGrid
    int errW;
    PaletteRef paletteRef;

    // from resolveLayout
    OverlayVariantIndex ovIndex;
    RailPlan tempPlan, enhPlan;
    bool enhReused;
    std::vector<uint8_t> powerCls;

    // from enhance
    std::vector<int> enhancedGrid;
    bool enhanced;

    void markPhase(int phase) {
        const auto now = std::chrono::high_resolution_clock::now();
        const long long val = g_progressVal.load();
        phaseMs[phase] += std::chrono::duration<double, std::milli>(now - phaseClock).count();
        phaseUnits[phase] += val - phaseVal;
        phaseClock = now; phaseVal = val;
    }

    Pass2Weights pass2WeightsAt(double speed) {
        const double threads = getOptimalThreadCount();
        const double rankSpread = threads / std::max(1.0, std::min(threads, (double)p2TilesY));
        const double refineSpread = threads / std::max(1.0, std::min(threads, (double)p2TilesToRefine));
        auto units = [](double v) { return std::max(1, (int)std::lround(v)); };
        return Pass2Weights{ units(wtPass2Rebuild * speed), units(WT_P2_RANK * rankSpread * speed),
            units(wtPass2Refine * refineSpread * speed), units(WT_P2_SETUP * refineSpread * speed) };
    }

    long long pass2Units(const Pass2Weights& w, long long scans, long long refinedCells) {
        const long long cells = (long long)tW * tH;
        return cells * w.rebuildCell + cells * w.rankCell + scans * w.refineScan * p2Passes + refinedCells * w.setupCell;
    }

    // phases in run order; false = the run ends there (nothing to do, cancelled, or abandoned with a message)
    bool readSettings();
    bool prepareImage();
    bool planWork();
    bool convertLab();
    bool publishStatus();
    bool prepareGrid();
    bool firstPass();
    bool resolveLayout();
    bool refine();
    bool enhance();
    bool commit();
};
void calculatePreview();


//////////////// SUPERVISOR
inline std::mutex supervisorMutex;
inline std::condition_variable supervisorCv;
inline bool pendingPreview = false;
void initSupervisorThread();


//////////////// EXPORTS
std::string latinFor(unsigned int cp);
std::string exportName(const std::string& name, bool lower);
void saveDatapackZip(const std::string& muralName, const std::vector<std::string>& commands, int versionIdx);
std::vector<std::string> exportIds(const std::vector<ActiveBlock>& palette, int versionIdx);
void generateDatapack(const std::string& name);

// litematica nbt, big-endian
struct NBTWriter {
    std::vector<uint8_t> buf;

    void writeByte(int8_t v) { buf.push_back((uint8_t)v); }
    void writeShort(int16_t v) { buf.push_back((v >> 8) & 0xFF); buf.push_back(v & 0xFF); }
    void writeInt(int32_t v) {
        buf.push_back((v >> 24) & 0xFF); buf.push_back((v >> 16) & 0xFF);
        buf.push_back((v >> 8) & 0xFF); buf.push_back(v & 0xFF);
    }
    void writeLong(int64_t v) {
        for (int i = 7; i >= 0; --i) buf.push_back((v >> (i * 8)) & 0xFF);
    }
    void writeString(const std::string& s) {
        writeShort((int16_t)s.length());
        for (char c : s) buf.push_back((uint8_t)c);
    }

    void tagEnd() { writeByte(0); }
    void tagByte(const std::string& name, int8_t v) { writeByte(1); writeString(name); writeByte(v); }
    void tagShort(const std::string& name, int16_t v) { writeByte(2); writeString(name); writeShort(v); }
    void tagInt(const std::string& name, int32_t v) { writeByte(3); writeString(name); writeInt(v); }
    void tagLong(const std::string& name, int64_t v) { writeByte(4); writeString(name); writeLong(v); }
    void tagString(const std::string& name, const std::string& v) { writeByte(8); writeString(name); writeString(v); }

    void beginList(const std::string& name, uint8_t type, int32_t size) {
        writeByte(9); writeString(name); writeByte(type); writeInt(size);
    }
    void beginCompound(const std::string& name) { writeByte(10); writeString(name); }
    void beginLongArray(const std::string& name, int32_t size) { writeByte(12); writeString(name); writeInt(size); }
};
void generateLitematica(const std::string& name);


//////////////// BRIDGE

// getters take dataMutex where they touch engine state, so they are safe during a calculation
// never call Log() with the lock held, it takes dataMutex itself
#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE
#endif

// bits mural_poll() returns
#define MD_DB_READY   (1 << 0)
#define MD_LOG        (1 << 1)
#define MD_PREVIEW    (1 << 2)
#define MD_PROCESSING (1 << 3)

inline const char* kBiomeNames[8] = {
    "Plains", "Forest", "Jungle", "Swamp", "Badlands", "Desert/Savanna", "Taiga", "Dark Forest"
};
const char* biomeName(int idx);

inline std::atomic<bool> g_dbReady{ false };
inline bool g_dbAnnounced = false;
inline bool g_lastProcessing = false;
inline int  g_lastPreviewSerial = -1;
inline size_t g_lastLogCount = 0;

inline std::string g_jsonBlocks, g_jsonOverlays, g_jsonConfig, g_jsonVersions;
inline std::string g_jsonStatus, g_jsonPreviewIds, g_scratch, g_logLine, g_bgColor;

// per preview atlas slot: the overlay block with its state as it will be exported, or ""
inline std::string g_jsonPreviewOverlays = "[]";

inline std::vector<unsigned char> g_blockAtlas;    // 16x16 rgba per block, csv order
inline int g_blockAtlasCols = 32;
inline std::vector<unsigned char> g_overlayAtlas;    // 16x16 rgba per overlay id
inline int g_overlayAtlasCols = 16;
inline std::vector<std::string> g_allOverlayIds;

inline std::vector<int32_t> g_gridSnap;    // compacted palette index per cell
inline std::vector<unsigned char> g_meanSnap;    // previewW * previewH rgba
inline std::vector<unsigned char> g_paletteAtlas;    // 16x16 rgba per palette entry in use
inline int g_paletteAtlasCols = 32;
inline int g_paletteAtlasCount = 0;
inline int g_snapW = 0, g_snapH = 0;
void jsonEscape(const std::string& in, std::string& out);
void jsonStr(const std::string& s, std::string& out);
std::set<std::string> splitIdSet(const char* csv);
void blitTile(std::vector<unsigned char>& atlas, int cols, int slot,
    const unsigned char* src16);
void ensureAtlasSize(std::vector<unsigned char>& atlas, int cols, int count);
void buildBlockIconAtlas();
void buildOverlayIconAtlas(int biomeIdxLocal);
void captureOverlayIds();
void snapshotPreview();
