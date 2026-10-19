// both stb libraries get built here and nowhere else
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "engine_internal.h"


//////////////// REFINEMENT CHUNKS
bool pass2WholeImage(int idx) {
    return kPass2ChunkBlocks[std::clamp(idx, 0, kPass2ChunkChoices - 1)] == 0;
}
int pass2ChunkBlocks(int idx, int muralW, int muralH) {
    if (pass2WholeImage(idx)) return std::max(1, std::max(muralW, muralH));
    return kPass2ChunkBlocks[std::clamp(idx, 0, kPass2ChunkChoices - 1)];
}


//////////////// THREADS
bool engineBusy() { return previewBusy.load() || exportBusy.load() || g_sourceWanted.load(); }

int getOptimalThreadCount() {
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores <= 0) cores = 4;    // 0 = unknown

    int workers = (int)cores - 1;    // one core stays with the supervisor

    if (workers < 1) workers = 1;
    if (workers > 14) workers = 14;    // keep in step with PTHREAD_POOL_SIZE in build.ps1

    return workers;
}

WorkerPool* getPool() {
    static WorkerPool pool(getOptimalThreadCount());
    return &pool;
}


//////////////// LISTS AND LOG
void sortBlockList(std::vector<BlockData*>& list) {
    std::sort(list.begin(), list.end(), [](BlockData* a, BlockData* b) {
        return a->order < b->order;
        });
}

void Log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(dataMutex);
    consoleLog.push_back(msg);
}


//////////////// CSV
std::vector<std::string> splitCSV(const std::string& line, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, delim)) result.push_back(item);
    return result;
}

// a spreadsheet editor may save it with tabs -- falling back to , puts a whole row in one field
char sniffDelim(const std::string& headerLine) {
    if (headerLine.find('\t') != std::string::npos) return '\t';
    if (headerLine.find(';') != std::string::npos) return ';';
    return ',';
}


//////////////// GAME VERSIONS

// "1.13" = {1,13}, "26.3" = {26,3} -- compared as strings "1.9" would beat "1.21"
std::pair<int, int> parseVersion(const std::string& v) {
    int part[2] = { 0, 0 };
    int which = 0;

    // no stoi: no exceptions, no locale, junk = {0,0}
    for (char c : v) {
        if (c >= '0' && c <= '9') part[which] = part[which] * 10 + (c - '0');
        else if (c == '.' && which == 0) which = 1;
        else break;
    }
    return { part[0], part[1] };
}

// sub-version parseVersion drops: "1.21.9" = 9, "1.21" = 0
int versionPatch(const std::string& v) {
    int dots = 0, patch = 0;
    for (char c : v) {
        if (c == '.') { if (++dots > 2) break; }
        else if (c < '0' || c > '9') break;
        else if (dots == 2) patch = patch * 10 + (c - '0');
    }
    return patch;
}

// newest id to the id this release used (version_renames.csv), the rest stay
std::string applyVersionRename(const std::string& id, int versionIdx) {
    for (const auto& r : versionRenames) {
        if (versionIdx < r.sinceIdx && id == r.to) return r.from;
    }
    return id;
}

// tested: each entry in the first + last game of its range, 10 murals each -- from 1.16 up none of this happens
std::string oldGameWarning() {
    if (selectedVersionIdx < 0 || selectedVersionIdx >= (int)g_gameVersions.size()) return "";
    const auto v = parseVersion(g_gameVersions[selectedVersionIdx]);
    if (v >= parseVersion("1.16")) return "";

    // rails join their neighbours another way up to 1.14, redstone dust up to 1.15
    // 1.13: a shulker box holds a ladder, nothing else -- 1.14 + 1.15: soul sand holds neither rails nor dust
    const OldGameCells& n = g_oldGameCells;
    const bool shaped = n.dust > 0 || (n.rails > 0 && v < parseVersion("1.15"));
    const bool shulker = n.onShulker > 0 && v == parseVersion("1.13");
    const bool soulSand = n.onSoulSand > 0 && (v == parseVersion("1.14") || v == parseVersion("1.15"));
    if (!shaped && !shulker && !soulSand) return "";
    return "CAUTION: In the game versions before 1.16 some overlays may misbehave.";
}

int versionIndexOf(const std::string& v) {
    for (size_t i = 0; i < g_gameVersions.size(); ++i) {
        if (g_gameVersions[i] == v) return (int)i;
    }

    // row says "1.21", list says "1.21.9" -- same major.minor = same entry, not the oldest one
    if (!v.empty()) {
        const auto key = parseVersion(v);
        for (size_t i = 0; i < g_gameVersions.size(); ++i) {
            if (parseVersion(g_gameVersions[i]) == key) return (int)i;
        }
    }
    return 0;
}


//////////////// COLOUR
std::vector<ColorLab> convertRGBtoLAB(const unsigned char* rgbData, int width, int height) {
    std::vector<ColorLab> outLab(width * height);
    for (int i = 0; i < width * height; ++i) {
        float r = rgbData[i * 3] / 255.0f, g = rgbData[i * 3 + 1] / 255.0f, b = rgbData[i * 3 + 2] / 255.0f;
        convertPixelToLAB(r, g, b, outLab[i].l, outLab[i].a, outLab[i].b);
    }
    return outLab;
}


//////////////// PICTURE TWEAKS
void fastSeparableBlur(const std::vector<unsigned char>& src, std::vector<unsigned char>& dst, int w, int h, int radius) {
    if (radius < 1) { dst = src; return; }
    std::vector<unsigned char> temp(w * h * 4);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int k = -radius; k <= radius; ++k) {
                int nx = x + k;
                if (nx >= 0 && nx < w) {
                    int idx = (y * w + nx) * 4;
                    r += src[idx]; g += src[idx + 1]; b += src[idx + 2]; a += src[idx + 3]; count++;
                }
            }
            int dstIdx = (y * w + x) * 4;
            temp[dstIdx] = r / count; temp[dstIdx + 1] = g / count; temp[dstIdx + 2] = b / count; temp[dstIdx + 3] = a / count;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int k = -radius; k <= radius; ++k) {
                int ny = y + k;
                if (ny >= 0 && ny < h) {
                    int idx = (ny * w + x) * 4;
                    r += temp[idx]; g += temp[idx + 1]; b += temp[idx + 2]; a += temp[idx + 3]; count++;
                }
            }
            int dstIdx = (y * w + x) * 4;
            dst[dstIdx] = r / count; dst[dstIdx + 1] = g / count; dst[dstIdx + 2] = b / count; dst[dstIdx + 3] = a / count;
        }
    }
}

std::vector<unsigned char> applyImageTweaks(const std::vector<unsigned char>& src, int w, int h) {
    std::vector<unsigned char> dst = src;
    int total = w * h;

    if (tweakDenoise > 0.0f || tweakSharpen > 0.0f) {
        std::vector<unsigned char> blurred(total * 4);
        int blurRad = (int)std::max(tweakDenoise, tweakSharpen > 0 ? 2.0f : 0.0f);
        fastSeparableBlur(src, blurred, w, h, blurRad);

        for (int i = 0; i < total; ++i) {
            int idx = i * 4;
            float rOrig = src[idx], gOrig = src[idx + 1], bOrig = src[idx + 2];
            float rBlur = blurred[idx], gBlur = blurred[idx + 1], bBlur = blurred[idx + 2];

            if (tweakDenoise > 0.0f) {
                float diff = std::abs(rOrig - rBlur) + std::abs(gOrig - gBlur) + std::abs(bOrig - bBlur);
                if (diff < 40.0f) {    // only smooths what is close already, edges stay
                    rOrig = (rOrig + rBlur * tweakDenoise) / (1.0f + tweakDenoise);
                    gOrig = (gOrig + gBlur * tweakDenoise) / (1.0f + tweakDenoise);
                    bOrig = (bOrig + bBlur * tweakDenoise) / (1.0f + tweakDenoise);
                }
            }
            if (tweakSharpen > 0.0f) {
                rOrig += (rOrig - rBlur) * tweakSharpen;
                gOrig += (gOrig - gBlur) * tweakSharpen;
                bOrig += (bOrig - bBlur) * tweakSharpen;
            }

            dst[idx] = (unsigned char)std::clamp(rOrig, 0.0f, 255.0f);
            dst[idx + 1] = (unsigned char)std::clamp(gOrig, 0.0f, 255.0f);
            dst[idx + 2] = (unsigned char)std::clamp(bOrig, 0.0f, 255.0f);
        }
    }

    if (tweakSaturation != 1.0f || tweakContrast != 1.0f || tweakBrightness != 0.0f || tweakHue != 0.0f) {
        float m00 = 1, m01 = 0, m02 = 0, m10 = 0, m11 = 1, m12 = 0, m20 = 0, m21 = 0, m22 = 1;
        if (tweakHue != 0.0f) {
            float theta = tweakHue * 3.14159265f / 180.0f;
            float c = std::cos(theta);
            float s = std::sin(theta);

            // hue turned around the grey axis (rec 709 weights)
            m00 = 0.213f + 0.787f * c - 0.213f * s; m01 = 0.715f - 0.715f * c - 0.715f * s; m02 = 0.072f - 0.072f * c + 0.928f * s;
            m10 = 0.213f - 0.213f * c + 0.143f * s; m11 = 0.715f + 0.285f * c + 0.140f * s; m12 = 0.072f - 0.072f * c - 0.283f * s;
            m20 = 0.213f - 0.213f * c - 0.787f * s; m21 = 0.715f - 0.715f * c + 0.715f * s; m22 = 0.072f + 0.928f * c + 0.072f * s;
        }

        for (int i = 0; i < total; ++i) {
            int idx = i * 4;
            float r = dst[idx] / 255.0f;
            float g = dst[idx + 1] / 255.0f;
            float b = dst[idx + 2] / 255.0f;

            if (tweakHue != 0.0f) {
                float hr = r * m00 + g * m01 + b * m02;
                float hg = r * m10 + g * m11 + b * m12;
                float hb = r * m20 + g * m21 + b * m22;
                r = hr; g = hg; b = hb;
            }

            r = (r - 0.5f) * tweakContrast + 0.5f + tweakBrightness;
            g = (g - 0.5f) * tweakContrast + 0.5f + tweakBrightness;
            b = (b - 0.5f) * tweakContrast + 0.5f + tweakBrightness;

            float lum = 0.299f * r + 0.587f * g + 0.114f * b;
            r = lum + tweakSaturation * (r - lum);
            g = lum + tweakSaturation * (g - lum);
            b = lum + tweakSaturation * (b - lum);

            dst[idx] = (unsigned char)std::clamp(r * 255.0f, 0.0f, 255.0f);
            dst[idx + 1] = (unsigned char)std::clamp(g * 255.0f, 0.0f, 255.0f);
            dst[idx + 2] = (unsigned char)std::clamp(b * 255.0f, 0.0f, 255.0f);
        }
    }
    return dst;
}
