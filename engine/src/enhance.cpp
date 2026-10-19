#include "enhance.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>

// block search + feathering, all colour maths in oklab on linear light
namespace {

//////////////// CONSTANTS
constexpr int B = 16;    // texels per block
constexpr int ER = 2;    // eye blur radius
constexpr int SR = 6;    // ssim window radius
constexpr float EYE_SIGMA = 0.6f;
constexpr float SSIM_SIGMA = 1.5f;
constexpr float C1 = 0.01f * 0.01f, C2 = 0.03f * 0.03f;

constexpr float BETA = 5.0f;    // colour error against structure
constexpr float CHROMA = 2.5f;    // hue + saturation against lightness in the score
constexpr float RANGE_CHROMA = 1.5f;    // less in the tone range test, so a pink and a yellow block can both stay and mix to skin
constexpr float CELL_WEIGHT = 0.3f;    // a blocks own mean colour
constexpr float CELL_BOOST = 3.0f;    // extra on it in flat areas
constexpr float STRUCT_FLOOR = 0.15f;    // share of the structure term kept in flat areas
constexpr float COLOUR_FLOOR = 0.40f;    // same for the per-texel colour term
constexpr float DETAIL_TAU = 0.05f;    // cell to cell lightness spread that counts as detail
constexpr float DETAIL_SIGMA = 1.5f;    // blocks
constexpr float TONE_MIN = 0.11f;    // a cells blocks stay within max(TONE_MIN, TONE_REL x best) of its own colour
constexpr float TONE_REL = 1.5f;
constexpr float CHROMA_MARGIN = 0.015f;    // colour variation a block may carry beyond the photos
constexpr float CHROMA_BUDGET = 4.0f;
constexpr float AMOUNT_SHARE = 0.6f;    // share of the photos texture a block should carry
constexpr float COARSE_MARGIN = 0.01f;    // big shapes a block may carry beyond the photo patch
constexpr float COARSE_WEIGHT = 10.0f;
constexpr float FINE_MARGIN = 0.03f;
constexpr int NEAREST = 16;    // nearest-colour blocks tried per cell
constexpr int SWEEPS = 3;
constexpr int REACH = 3;    // blocks a swap can reach in any direction
constexpr int STRIDE = 4;    // cells this far apart never read each other, so they run at the same time (over REACH)
constexpr int CHUNK = 32;    // cells per thread hand-out

constexpr float FEATHER_DETAIL = 0.35f;    // feather only where the detail map is below this
constexpr float FEATHER_JUMP = 0.04f;    // smallest mural tone step worth softening
constexpr int FEATHER_MAX = 8;    // widest band each side of an edge (blocks)
constexpr float FEATHER_LINE = 0.05f;    // how far off the line between two tones a middle step may be
constexpr float FEATHER_TEXTURE = 0.25f;    // how unlike the two tones texture a middle step may be
constexpr float FEATHER_TRIM = 0.15f;    // clean-up: cells less than this far towards the other tone stay
constexpr float CALM_COARSE = 0.03f;    // a middle step has no big shapes
constexpr float CALM_COLOUR = 0.02f;    // and little colour variation
constexpr float SHAPE_EPS = 0.15f;
constexpr int DESC = 96;    // six maps pooled on a 4x4 grid

// tested: progress ticks per cell follow the time taken (270x326: search 5.8 s, feathering alone 80 ms)
constexpr int TICKS_PREP_SEARCH = 6, TICKS_PREP_LIGHT = 2, TICKS_SWEEP = 77, TICKS_FEATHER = 2;
constexpr float Q15 = 32767.0f;


//////////////// HELPERS
struct Kernel {
    int r = 0;
    std::vector<float> w;    // 2r+1 weights summing to 1
};

Kernel gauss(float sigma, int r) {
    Kernel k;
    k.r = r;
    k.w.resize(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += (k.w[i + r] = std::exp(-(float)(i * i) / (2 * sigma * sigma)));
    for (float& v : k.w) v /= sum;
    return k;
}

inline float srgbToLinear(float c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline void toOklab(const float* c, float* out) {
    float l = 0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2];
    float m = 0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2];
    float s = 0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2];
    l = std::cbrt(l); m = std::cbrt(m); s = std::cbrt(s);
    out[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

inline int16_t pack15(float v) {
    return (int16_t)std::lround(std::clamp(v, -1.0f, 1.0f) * Q15);
}

inline float dist3(const float* a, const float* b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}


//////////////// BLOCK FACES

// what a 16x16 oklab patch carries: texture, colour variation, and lightness contrast
// split into big shapes (4x4 texel chunks) and the fine grain left
struct PatchStats { float busy, colour, coarse, fine; };

PatchStats patchStats(const float* lab) {
    double sl = 0, sll = 0, sa = 0, saa = 0, sb = 0, sbb = 0;
    float chunk[16] = { 0 };
    for (int y = 0; y < B; ++y) {
        for (int x = 0; x < B; ++x) {
            const float* v = lab + (y * B + x) * 3;
            sl += v[0]; sll += v[0] * v[0];
            sa += v[1]; saa += v[1] * v[1];
            sb += v[2]; sbb += v[2] * v[2];
            chunk[(y / 4) * 4 + x / 4] += v[0];
        }
    }
    const double n = B * B;
    double ml = sl / n, ma = sa / n, mb = sb / n;
    double varL = std::max(sll / n - ml * ml, 0.0);
    double varC = std::max(saa / n - ma * ma, 0.0) + std::max(sbb / n - mb * mb, 0.0);
    double cv = 0;
    for (int i = 0; i < 16; ++i) { double c = chunk[i] / 16.0 - ml; cv += c * c; }
    cv /= 16.0;
    PatchStats s;
    s.busy = (float)std::sqrt(varL + CHROMA * CHROMA * varC);
    s.colour = (float)std::sqrt(varC);
    s.coarse = (float)std::sqrt(cv);
    s.fine = (float)std::sqrt(std::max(varL - cv, 0.0));
    return s;
}

// lighter, darker + edge energy in four directions, pooled at 4x4 points (after Xu, Zhang + Wong)
void shapeDescriptor(const float* light, const Kernel& pool, float* out) {
    float mean = 0;
    for (int i = 0; i < B * B; ++i) mean += light[i];
    mean /= B * B;
    static thread_local std::vector<float> maps;
    maps.assign(6 * B * B, 0.0f);
    for (int y = 0; y < B; ++y) {
        for (int x = 0; x < B; ++x) {
            int i = y * B + x;
            float z = light[i] - mean;
            float gx = (x == 0) ? light[i + 1] - light[i] : (x == B - 1) ? light[i] - light[i - 1] : 0.5f * (light[i + 1] - light[i - 1]);
            float gy = (y == 0) ? light[i + B] - light[i] : (y == B - 1) ? light[i] - light[i - B] : 0.5f * (light[i + B] - light[i - B]);
            maps[0 * B * B + i] = std::max(z, 0.0f);
            maps[1 * B * B + i] = std::max(-z, 0.0f);
            maps[2 * B * B + i] = std::fabs(gx);
            maps[3 * B * B + i] = std::fabs(gy);
            maps[4 * B * B + i] = std::fabs(gx + gy) * 0.7071f;
            maps[5 * B * B + i] = std::fabs(gx - gy) * 0.7071f;
        }
    }
    int o = 0;
    for (int m = 0; m < 6; ++m) {
        const float* mp = &maps[m * B * B];
        for (int py = 2; py < B; py += 4) {
            for (int px = 2; px < B; px += 4) {
                float acc = 0;
                for (int y = std::max(0, py - pool.r); y <= std::min(B - 1, py + pool.r); ++y) {
                    float wy = pool.w[y - py + pool.r], row = 0;
                    for (int x = std::max(0, px - pool.r); x <= std::min(B - 1, px + pool.r); ++x)
                        row += pool.w[x - px + pool.r] * mp[y * B + x];
                    acc += wy * row;
                }
                out[o++] = acc;
            }
        }
    }
}

// edge repeated, nothing from outside comes in -- a face blurred alone looks the same wherever it goes
void blurClamped(const float* in, int p, const Kernel& k, float* tmp, float* out) {
    for (int y = 0; y < p; ++y)
        for (int x = 0; x < p; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                float acc = 0;
                for (int i = -k.r; i <= k.r; ++i) acc += k.w[i + k.r] * in[(y * p + std::clamp(x + i, 0, p - 1)) * 3 + ch];
                tmp[(y * p + x) * 3 + ch] = acc;
            }
    for (int y = 0; y < p; ++y)
        for (int x = 0; x < p; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                float acc = 0;
                for (int i = -k.r; i <= k.r; ++i) acc += k.w[i + k.r] * tmp[(std::clamp(y + i, 0, p - 1) * p + x) * 3 + ch];
                out[(y * p + x) * 3 + ch] = acc;
            }
}

// ssim window over a p x p patch with zeros around it, result on the (p + 2*SR)^2 frame
void spread(const float* v, int p, const float* ker, float* hp, float* out) {
    const int Q = p + 2 * SR;
    std::fill(hp, hp + p * Q, 0.0f);
    for (int y = 0; y < p; ++y) {
        float* row = hp + y * Q;
        for (int x = 0; x < p; ++x) {
            const float val = v[y * p + x];
            for (int k = 0; k <= 2 * SR; ++k) row[x + k] += ker[k] * val;
        }
    }
    std::fill(out, out + Q * Q, 0.0f);
    for (int y = 0; y < p; ++y) {
        const float* src = hp + y * Q;
        for (int k = 0; k <= 2 * SR; ++k) {
            const float g = ker[k];
            float* dst = out + (y + k) * Q;
            for (int j = 0; j < Q; ++j) dst[j] += g * src[j];
        }
    }
}

struct Tiles {
    int n = 0;    // faces the stage has to know the look of
    int m = 0;    // the first m are plain blocks it may place
    std::vector<int> pal;    // palette index behind each face, -1 = empty cell
    std::vector<float> lab[2];    // n * p*p*3: blurred oklab, hue weighted, per scale
    std::vector<float> p1[2], p2[2];    // m * (p+12)^2: window sums of lightness and its square
    std::vector<float> meanW, meanR;    // n*3: mean colour, weighted for the score / the range test
    std::vector<float> busy, colour, coarse, fine;
    std::vector<float> desc, descSum;    // m*DESC, m (feathering only)
    std::vector<char> calm;
};

const int SCALE_P[2] = { 16, 4 };

struct TileScratch {
    float lin[B * B * 3], lab[B * B * 3], light[B * B], small[4 * 4 * 3], tmp[B * B * 3], blur[B * B * 3];
    float sq[B * B], hp[B * (B + 2 * SR)];
};

void buildTile(int t, const unsigned char* src, const unsigned char* bg, bool wantDesc, bool wantScales, Tiles& T,
    const Kernel& eye, const Kernel& win, const Kernel& pool, TileScratch& S) {
    float mean[3] = { 0, 0, 0 };
    for (int i = 0; i < B * B; ++i) {
        const float a = src[i * 4 + 3] / 255.0f;
        for (int ch = 0; ch < 3; ++ch) {
            float v = (src[i * 4 + ch] * a + bg[ch] * (1.0f - a)) / 255.0f;
            S.lin[i * 3 + ch] = srgbToLinear(v);
            mean[ch] += S.lin[i * 3 + ch];
        }
        toOklab(&S.lin[i * 3], &S.lab[i * 3]);
        S.light[i] = S.lab[i * 3];
    }
    for (float& v : mean) v /= B * B;
    float ml[3];
    toOklab(mean, ml);
    T.meanW[t * 3] = ml[0]; T.meanW[t * 3 + 1] = ml[1] * CHROMA; T.meanW[t * 3 + 2] = ml[2] * CHROMA;
    T.meanR[t * 3] = ml[0]; T.meanR[t * 3 + 1] = ml[1] * RANGE_CHROMA; T.meanR[t * 3 + 2] = ml[2] * RANGE_CHROMA;

    const bool plain = t < T.m;
    if (plain) {
        PatchStats ps = patchStats(S.lab);
        T.busy[t] = ps.busy; T.colour[t] = ps.colour; T.coarse[t] = ps.coarse; T.fine[t] = ps.fine;
        T.calm[t] = (ps.coarse <= CALM_COARSE && ps.colour <= CALM_COLOUR) ? 1 : 0;
        if (wantDesc) {
            float* d = &T.desc[(size_t)t * DESC];
            shapeDescriptor(S.light, pool, d);
            float sum = 0;
            for (int i = 0; i < DESC; ++i) sum += d[i];
            T.descSum[t] = sum;
        }
    }

    // only the search looks at a face texel by texel
    for (int s = 0; s < 2 && wantScales; ++s) {
        const int p = SCALE_P[s], f = B / p, Q = p + 2 * SR;
        const float* in = S.lin;
        if (f > 1) {
            for (int y = 0; y < p; ++y)
                for (int x = 0; x < p; ++x)
                    for (int ch = 0; ch < 3; ++ch) {
                        float acc = 0;
                        for (int yy = 0; yy < f; ++yy)
                            for (int xx = 0; xx < f; ++xx) acc += S.lin[((y * f + yy) * B + x * f + xx) * 3 + ch];
                        S.small[(y * p + x) * 3 + ch] = acc / (f * f);
                    }
            in = S.small;
        }
        blurClamped(in, p, eye, S.tmp, S.blur);
        float* out = &T.lab[s][(size_t)t * p * p * 3];
        for (int i = 0; i < p * p; ++i) {
            float v[3];
            toOklab(&S.blur[i * 3], v);
            out[i * 3] = v[0]; out[i * 3 + 1] = v[1] * CHROMA; out[i * 3 + 2] = v[2] * CHROMA;
            S.light[i] = v[0];
            S.sq[i] = v[0] * v[0];
        }
        if (plain) {
            spread(S.light, p, win.w.data(), S.hp, &T.p1[s][(size_t)t * Q * Q]);
            spread(S.sq, p, win.w.data(), S.hp, &T.p2[s][(size_t)t * Q * Q]);
        }
    }
}


//////////////// PHOTO

// what the photo asks of each cell
struct Cells {
    std::vector<float> labW, labR;    // mean colour, weighted for the score / the range test
    std::vector<float> light;
    std::vector<float> busy, colour, coarse, fine;
    std::vector<float> flat;    // 0 on detail, towards 1 where neighbouring cells are alike
    std::vector<float> detail;
    std::vector<char> opaque;
    std::vector<char> mess;    // fine detail without a bigger shape, like hair
};

struct Scale {
    int p = 0, W = 0, H = 0;
    std::vector<int16_t> tgt;    // the photo at this scale, blurred, oklab (H*W*3)
    std::vector<float> invX, invY;    // 1 / window weight inside the image
};

void windowNorms(Scale& sc, const Kernel& win) {
    auto fill = [&](std::vector<float>& inv, int n) {
        inv.resize(n);
        for (int x = 0; x < n; ++x) {
            float sum = 0;
            for (int k = -SR; k <= SR; ++k) if (x + k >= 0 && x + k < n) sum += win.w[k + SR];
            inv[x] = 1.0f / sum;
        }
    };
    fill(sc.invX, sc.W);
    fill(sc.invY, sc.H);
}


//////////////// SEARCH

// per thread, one scale of one cell
struct ScaleScratch {
    int p, F, Q;
    std::vector<float> a, b, aa, ab, bb;    // frame rows: mural lightness with the cell blanked, photo
    std::vector<float> h;    // 5 maps after the horizontal pass
    std::vector<float> rA, rAA, rAB;    // window sums without the cells own block
    std::vector<float> muB, kB, kV, w, inv;
    std::vector<float> tc;    // photo colour on the cell
    std::vector<float> prod, hp, conv;

    explicit ScaleScratch(int p_) : p(p_), F(p_ + 4 * SR), Q(p_ + 2 * SR) {
        a.resize(F * F); b.resize(F * F); aa.resize(F); ab.resize(F); bb.resize(F);
        h.resize(5 * F * Q);
        for (auto* v : { &rA, &rAA, &rAB, &muB, &kB, &kV, &w, &inv, &conv }) v->resize(Q * Q);
        tc.resize(p * p * 3); prod.resize(p * p); hp.resize(p * Q);
    }
};

struct Scratch {
    ScaleScratch s[2] = { ScaleScratch(SCALE_P[0]), ScaleScratch(SCALE_P[1]) };
    std::vector<std::pair<float, int>> ranked;
    std::vector<int> cand;
    std::vector<float> d2;
};

struct Search {
    int gw, gh;
    const Tiles& T;
    const Cells& C;
    const Scale* sc;    // two of them
    const Kernel& win;
    const std::vector<int>& g;
    float busyMargin, amount, grainFloor;

    // everything around the cell that doesnt depend on the block in it
    void prepare(int s, int r, int c, ScaleScratch& S) const {
        const Scale& L = sc[s];
        const int p = S.p, F = S.F, Q = S.Q;
        const int fy0 = r * p - 2 * SR, fx0 = c * p - 2 * SR;
        const float* ker = win.w.data();
        const float* lab = T.lab[s].data();

        std::fill(S.a.begin(), S.a.end(), 0.0f);
        std::fill(S.b.begin(), S.b.end(), 0.0f);
        for (int fy = 0; fy < F; ++fy) {
            const int y = fy0 + fy;
            if (y < 0 || y >= L.H) continue;
            const int x0 = std::max(fx0, 0), x1 = std::min(fx0 + F, L.W);
            const int16_t* src = &L.tgt[((size_t)y * L.W + x0) * 3];
            float* dst = &S.b[fy * F + (x0 - fx0)];
            for (int x = x0; x < x1; ++x, src += 3) *dst++ = *src * (1.0f / Q15);
        }
        const int k = (2 * SR + p - 1) / p;
        for (int rr = std::max(0, r - k); rr <= std::min(gh - 1, r + k); ++rr) {
            for (int cc = std::max(0, c - k); cc <= std::min(gw - 1, c + k); ++cc) {
                if (rr == r && cc == c) continue;
                const float* tl = lab + (size_t)g[rr * gw + cc] * p * p * 3;
                const int oy = rr * p - fy0, ox = cc * p - fx0;
                for (int ty = std::max(0, -oy); ty < std::min(p, F - oy); ++ty) {
                    float* dst = &S.a[(oy + ty) * F];
                    for (int tx = std::max(0, -ox); tx < std::min(p, F - ox); ++tx) dst[ox + tx] = tl[(ty * p + tx) * 3];
                }
            }
        }

        // horizontal pass of the window over five maps at once
        float* h = S.h.data();
        std::fill(S.h.begin(), S.h.end(), 0.0f);
        for (int fy = 0; fy < F; ++fy) {
            const float* ar = &S.a[fy * F];
            const float* br = &S.b[fy * F];
            float* aa = S.aa.data(); float* ab = S.ab.data(); float* bb = S.bb.data();
            for (int x = 0; x < F; ++x) { aa[x] = ar[x] * ar[x]; ab[x] = ar[x] * br[x]; bb[x] = br[x] * br[x]; }
            float* h0 = h + (0 * F + fy) * Q; float* h1 = h + (1 * F + fy) * Q; float* h2 = h + (2 * F + fy) * Q;
            float* h3 = h + (3 * F + fy) * Q; float* h4 = h + (4 * F + fy) * Q;
            for (int i = 0; i <= 2 * SR; ++i) {
                const float gk = ker[i];
                for (int j = 0; j < Q; ++j) {
                    h0[j] += gk * ar[j + i]; h1[j] += gk * aa[j + i]; h2[j] += gk * ab[j + i];
                    h3[j] += gk * br[j + i]; h4[j] += gk * bb[j + i];
                }
            }
        }

        // vertical pass, then all the ssim formula needs of the photo
        const int qy0 = r * p - SR, qx0 = c * p - SR;
        for (int qy = 0; qy < Q; ++qy) {
            float* oA = &S.rA[qy * Q]; float* oAA = &S.rAA[qy * Q]; float* oAB = &S.rAB[qy * Q];
            float* oB = &S.muB[qy * Q]; float* oBB = &S.kV[qy * Q];
            std::fill(oA, oA + Q, 0.0f); std::fill(oAA, oAA + Q, 0.0f); std::fill(oAB, oAB + Q, 0.0f);
            std::fill(oB, oB + Q, 0.0f); std::fill(oBB, oBB + Q, 0.0f);
            for (int i = 0; i <= 2 * SR; ++i) {
                const float gk = ker[i];
                const float* h0 = h + (0 * F + qy + i) * Q; const float* h1 = h + (1 * F + qy + i) * Q;
                const float* h2 = h + (2 * F + qy + i) * Q; const float* h3 = h + (3 * F + qy + i) * Q;
                const float* h4 = h + (4 * F + qy + i) * Q;
                for (int j = 0; j < Q; ++j) {
                    oA[j] += gk * h0[j]; oAA[j] += gk * h1[j]; oAB[j] += gk * h2[j];
                    oB[j] += gk * h3[j]; oBB[j] += gk * h4[j];
                }
            }
            const int y = qy0 + qy;
            const bool rowIn = (y >= 0 && y < L.H);
            const float iy = rowIn ? L.invY[y] : 0.0f;
            const int cellRow = rowIn ? y / p : 0;
            for (int j = 0; j < Q; ++j) {
                const int x = qx0 + j;
                const bool in = rowIn && x >= 0 && x < L.W;
                const float inv = in ? iy * L.invX[x] : 0.0f;
                const float mu = oB[j] * inv;
                S.inv[qy * Q + j] = inv;
                S.muB[qy * Q + j] = mu;
                S.kB[qy * Q + j] = mu * mu + C1;
                S.kV[qy * Q + j] = oBB[j] * inv - mu * mu + C2;
                S.w[qy * Q + j] = in ? 1.0f - C.flat[cellRow * gw + x / p] * (1.0f - STRUCT_FLOOR) : 0.0f;
            }
        }

        // photo on the cell itself
        for (int y = 0; y < p; ++y) {
            const int16_t* src = &L.tgt[((size_t)(r * p + y) * L.W + c * p) * 3];
            float* dst = &S.tc[y * p * 3];
            for (int x = 0; x < p; ++x, src += 3, dst += 3) {
                dst[0] = src[0] * (1.0f / Q15);
                dst[1] = src[1] * (CHROMA / Q15);
                dst[2] = src[2] * (CHROMA / Q15);
            }
        }
    }

    // structure + colour over the area a block here can reach
    float areaCost(int s, int t, float wColour, ScaleScratch& S) const {
        const int p = S.p, Q = S.Q;
        const float* tl = &T.lab[s][(size_t)t * p * p * 3];
        float colour = 0;
        for (int i = 0; i < p * p; ++i) {
            const float dl = tl[i * 3] - S.tc[i * 3], da = tl[i * 3 + 1] - S.tc[i * 3 + 1], db = tl[i * 3 + 2] - S.tc[i * 3 + 2];
            colour += std::sqrt(dl * dl + da * da + db * db);
            S.prod[i] = tl[i * 3] * S.tc[i * 3];
        }
        spread(S.prod.data(), p, win.w.data(), S.hp.data(), S.conv.data());

        const float* p1 = &T.p1[s][(size_t)t * Q * Q];
        const float* p2 = &T.p2[s][(size_t)t * Q * Q];
        const float* rA = S.rA.data(); const float* rAA = S.rAA.data(); const float* rAB = S.rAB.data();
        const float* muB = S.muB.data(); const float* kB = S.kB.data(); const float* kV = S.kV.data();
        const float* inv = S.inv.data(); const float* w = S.w.data(); const float* conv = S.conv.data();
        float acc = 0;
        for (int i = 0; i < Q * Q; ++i) {
            const float mu = (rA[i] + p1[i]) * inv[i];
            const float va = (rAA[i] + p2[i]) * inv[i] - mu * mu;
            const float cov = (rAB[i] + conv[i]) * inv[i] - mu * muB[i];
            const float ssim = ((2.0f * mu * muB[i] + C1) * (2.0f * cov + C2)) / ((mu * mu + kB[i]) * (va + kV[i]));
            acc += (1.0f - ssim) * w[i];
        }
        return (acc + BETA * colour * wColour) * (100.0f / (p * p));
    }

    // what the block itself costs here, whatever its neighbours
    float ownCost(int cell, int t, float d2) const {
        const float fl = C.flat[cell];
        float cost = CELL_WEIGHT * d2 * (1.0f + CELL_BOOST * fl);
        const float over = std::max(T.busy[t] - C.busy[cell] - busyMargin, 0.0f);
        cost += over * over;
        const float vivid = std::max(T.colour[t] - C.colour[cell] - CHROMA_MARGIN, 0.0f);
        cost += CHROMA_BUDGET * vivid * vivid;
        if (amount > 0) {
            const float want = std::max(AMOUNT_SHARE * C.busy[cell], grainFloor * fl);
            const float under = std::max(want - T.busy[t], 0.0f);
            cost += amount * under * under;
        }
        const float big = std::max(T.coarse[t] - C.coarse[cell] - COARSE_MARGIN, 0.0f);
        const float fineOver = std::max(T.fine[t] - C.fine[cell] - FINE_MARGIN, 0.0f);
        const float fineUnder = std::max(AMOUNT_SHARE * C.fine[cell] - T.fine[t], 0.0f);
        cost += COARSE_WEIGHT * big * big + fineOver * fineOver + fineUnder * fineUnder;
        return cost * 1e4f;
    }

    int pick(int r, int c, Scratch& S) const {
        const int cell = r * gw + c, cur = g[cell];
        const float* cw = &C.labW[cell * 3];
        const float* cr = &C.labR[cell * 3];

        // blocks within the cells tone range, nearest in colour first
        S.d2.resize(T.m);
        S.ranked.clear();
        float best = std::numeric_limits<float>::max();
        for (int t = 0; t < T.m; ++t) {
            const float* tr = &T.meanR[t * 3];
            const float x = cr[0] - tr[0], y = cr[1] - tr[1], z = cr[2] - tr[2];
            S.d2[t] = x * x + y * y + z * z;
            best = std::min(best, S.d2[t]);
        }
        const float lim = std::max(TONE_MIN, TONE_REL * std::sqrt(best));
        const float lim2 = lim * lim;
        auto score = [&](int t) {
            const float* tw = &T.meanW[t * 3];
            const float x = cw[0] - tw[0], y = cw[1] - tw[1], z = cw[2] - tw[2];
            return x * x + y * y + z * z;
        };
        for (int t = 0; t < T.m; ++t) if (S.d2[t] <= lim2) S.ranked.emplace_back(score(t), t);
        const int keep = std::min((int)S.ranked.size(), NEAREST);
        std::partial_sort(S.ranked.begin(), S.ranked.begin() + keep, S.ranked.end());

        S.cand.clear();
        S.cand.push_back(cur);
        auto add = [&](int t) {
            if (std::find(S.cand.begin(), S.cand.end(), t) == S.cand.end()) S.cand.push_back(t);
        };
        for (int i = 0; i < keep; ++i) add(S.ranked[i].second);
        const int nb[4][2] = { { r - 1, c }, { r + 1, c }, { r, c - 1 }, { r, c + 1 } };
        for (const auto& q : nb) {
            if (q[0] < 0 || q[0] >= gh || q[1] < 0 || q[1] >= gw) continue;
            const int t = g[q[0] * gw + q[1]];
            if (t < T.m && S.d2[t] <= lim2) add(t);
        }
        if (S.cand.size() < 2) return cur;

        prepare(0, r, c, S.s[0]);
        prepare(1, r, c, S.s[1]);
        const float wColour = 1.0f - C.flat[cell] * (1.0f - COLOUR_FLOOR);
        float curCost = 0, bestCost = std::numeric_limits<float>::max();
        int bestTile = cur;
        for (size_t i = 0; i < S.cand.size(); ++i) {
            const int t = S.cand[i];
            const float cost = ownCost(cell, t, score(t)) + areaCost(0, t, wColour, S.s[0]) + areaCost(1, t, wColour, S.s[1]);
            if (i == 0) curCost = cost;
            if (cost < bestCost) { bestCost = cost; bestTile = t; }
        }

        // a swap has to be a real gain, not rounding
        return (curCost - bestCost > 1e-5f * (1.0f + std::fabs(curCost))) ? bestTile : cur;
    }
};


//////////////// FEATHERING
uint64_t nextRandom(uint64_t& s) {    // splitmix64
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void blurCells(const std::vector<float>& in, int gw, int gh, const Kernel& k, std::vector<float>& out) {
    std::vector<float> tmp(in.size());
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            float acc = 0, sum = 0;
            for (int i = -k.r; i <= k.r; ++i) {
                if (x + i < 0 || x + i >= gw) continue;
                acc += k.w[i + k.r] * in[y * gw + x + i]; sum += k.w[i + k.r];
            }
            tmp[y * gw + x] = acc / sum;
        }
    out.resize(in.size());
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            float acc = 0, sum = 0;
            for (int i = -k.r; i <= k.r; ++i) {
                if (y + i < 0 || y + i >= gh) continue;
                acc += k.w[i + k.r] * tmp[(y + i) * gw + x]; sum += k.w[i + k.r];
            }
            out[y * gw + x] = acc / sum;
        }
}

// mixes the two tones either side of an edge the mural has and the photo doesnt; fg = plain block per cell or -1
int feather(std::vector<int>& fg, const Tiles& T, const Cells& C, const std::vector<char>& smooth,
    float trim, float keep, int gw, int gh) {
    const int cells = gw * gh;
    const float* tm = T.meanR.data();

    // where: a step between neighbours the photo covers gradually -- cells near one keep its two blocks
    std::vector<float> nearest(cells, std::numeric_limits<float>::max());
    std::vector<int> lo(cells, -1), hi(cells, -1);
    for (int pass = 0; pass < 2; ++pass) {
        const int dy = pass, dx = 1 - pass;
        for (int y = 0; y + dy < gh; ++y) {
            for (int x = 0; x + dx < gw; ++x) {
                const int i = y * gw + x, j = (y + dy) * gw + x + dx;
                if (!smooth[i] || !smooth[j]) continue;
                const int a = fg[i], b = fg[j];
                const float jump = dist3(tm + a * 3, tm + b * 3);
                if (jump <= FEATHER_JUMP) continue;
                const float step = dist3(&C.labR[i * 3], &C.labR[j * 3]);
                if (!(step < 0.5f * jump)) continue;

                // as many cells as the photo needs to cover that step
                const int rFull = std::clamp((int)std::ceil(jump / std::max(step, 1e-3f)), 1, FEATHER_MAX);

                // narrower when dithering is mixing already (keep < 1)
                const int r = (int)std::lround(rFull * keep);
                if (r < 1) continue;
                const float cy = y + dy * 0.5f, cx = x + dx * 0.5f;
                for (int yy = std::max(0, y - r); yy < std::min(gh, y + dy + r + 1); ++yy) {
                    for (int xx = std::max(0, x - r); xx < std::min(gw, x + dx + r + 1); ++xx) {
                        const float d = std::hypot(yy - cy, xx - cx);
                        const int k = yy * gw + xx;
                        if (d <= r + 0.5f && d < nearest[k]) { nearest[k] = d; lo[k] = a; hi[k] = b; }
                    }
                }
            }
        }
    }

    // an irregular threshold pattern: high-passed noise ranked to an even spread, no clumps, no grid
    std::vector<float> noise(cells), low;
    uint64_t seed = 1;
    for (int i = 0; i < cells; ++i) {
        const double u1 = ((nextRandom(seed) >> 11) + 1.0) / 9007199254740993.0;
        const double u2 = (nextRandom(seed) >> 11) / 9007199254740992.0;
        noise[i] = (float)(std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2));
    }
    blurCells(noise, gw, gh, gauss(1.5f, 6), low);
    std::vector<int> order(cells);
    for (int i = 0; i < cells; ++i) { noise[i] -= low[i]; order[i] = i; }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return noise[a] < noise[b] || (noise[a] == noise[b] && a < b); });
    std::vector<float> mask(cells);
    for (int i = 0; i < cells; ++i) mask[order[i]] = (i + 0.5f) / cells;

    // what: the two tones + a calm in-between block with the same kind of texture, when there is one
    auto alike = [&](int i, int j) {
        const float* a = &T.desc[(size_t)i * DESC];
        const float* b = &T.desc[(size_t)j * DESC];
        float d = 0;
        for (int k = 0; k < DESC; ++k) d += std::fabs(a[k] - b[k]);
        return d / (T.descSum[i] + T.descSum[j] + SHAPE_EPS) <= FEATHER_TEXTURE;
    };
    struct Ramp { int mid = -1; float at = 0; };    // at: how far from the lower-numbered tone
    std::map<std::pair<int, int>, Ramp> ramps;
    auto rampFor = [&](int a, int b) -> const Ramp& {
        auto key = std::make_pair(std::min(a, b), std::max(a, b));
        auto it = ramps.find(key);
        if (it != ramps.end()) return it->second;
        const float* pa = tm + key.first * 3;
        const float* pb = tm + key.second * 3;
        const float ab[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
        const float den = std::max(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], 1e-9f);
        std::vector<std::pair<float, int>> mids;
        for (int t = 0; t < T.m; ++t) {
            if (!T.calm[t]) continue;
            const float* pt = tm + t * 3;
            const float v[3] = { pt[0] - pa[0], pt[1] - pa[1], pt[2] - pa[2] };
            const float pos = (v[0] * ab[0] + v[1] * ab[1] + v[2] * ab[2]) / den;
            if (!(pos > 0.2f && pos < 0.8f)) continue;
            const float o[3] = { v[0] - pos * ab[0], v[1] - pos * ab[1], v[2] - pos * ab[2] };
            if (std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) > FEATHER_LINE) continue;
            if (!alike(t, key.first) || !alike(t, key.second)) continue;
            mids.emplace_back(pos, t);
        }
        Ramp ramp;
        if (!mids.empty()) {
            std::sort(mids.begin(), mids.end());
            ramp.mid = mids[mids.size() / 2].second;
            ramp.at = mids[mids.size() / 2].first;
        }
        return ramps.emplace(key, ramp).first->second;
    };

    std::vector<int> out = fg;
    int moved = 0;
    for (int i = 0; i < cells; ++i) {
        if (lo[i] < 0 || !smooth[i]) continue;
        const int a = lo[i], b = hi[i];
        const Ramp& ramp = rampFor(a, b);
        int steps[3]; float at[3]; int n = 0;
        steps[n] = a; at[n++] = 0.0f;
        if (ramp.mid >= 0) { steps[n] = ramp.mid; at[n++] = (a < b) ? ramp.at : 1.0f - ramp.at; }
        steps[n] = b; at[n++] = 1.0f;
        if (std::find(steps, steps + n, fg[i]) == steps + n) continue;    // a third tone isnt part of this edge

        const float* pa = tm + a * 3;
        const float* pb = tm + b * 3;
        const float* pc = &C.labR[i * 3];
        const float ab[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
        const float den = std::max(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], 1e-9f);
        const float where = std::clamp(((pc[0] - pa[0]) * ab[0] + (pc[1] - pa[1]) * ab[1] + (pc[2] - pa[2]) * ab[2]) / den, 0.0f, 1.0f);
        const int k = (n == 3 && where >= at[1]) ? 1 : 0;
        float frac = (where - at[k]) / std::max(at[k + 1] - at[k], 1e-9f);
        frac = std::clamp((frac - trim) / std::max(1.0f - 2.0f * trim, 1e-9f), 0.0f, 1.0f);
        out[i] = (frac > mask[i]) ? steps[k + 1] : steps[k];
        if (out[i] != fg[i]) ++moved;
    }
    fg.swap(out);
    return moved;
}

}    // namespace


//////////////// STAGE
EnhanceSettings enhanceEffective(const EnhanceSettings& s, int cells) {
    EnhanceSettings e = s;
    if (cells > ENHANCE_MAX_CELLS) e.search = false;
    if (!e.search) e.protect = 0.0f;
    if (e.featherKeep <= 0.0f) e.feather = false;
    return e;
}

int enhanceTicks(int cells, const EnhanceSettings& effective) {
    return cells * ((effective.search ? TICKS_PREP_SEARCH + SWEEPS * TICKS_SWEEP : TICKS_PREP_LIGHT) + TICKS_FEATHER);
}

bool enhanceMural(std::vector<int>& grid, const EnhanceIO& io, const EnhanceSettings& asked, std::string& note) {
    const int gw = io.gw, gh = io.gh;
    const int cells = gw * gh;
    if (gw <= 0 || gh <= 0 || (int)grid.size() != cells || !io.fetchRow) return false;
    const EnhanceSettings s = enhanceEffective(asked, cells);
    if (asked.search && !s.search) {
        note = "Grain and colour-over-texture were left out: they work on murals of up to "
            + std::to_string(ENHANCE_MAX_CELLS) + " blocks and this one has " + std::to_string(cells) + ".";
    }
    if (!s.search && !s.feather) return false;

    std::function<void(int, const std::function<void(int)>&)> par = io.parallelFor;
    if (!par) par = [](int n, const std::function<void(int)>& body) { for (int i = 0; i < n; ++i) body(i); };
    auto cancelled = [&]() { return io.cancelled && io.cancelled(); };
    auto tick = [&](int n) { if (io.tick && n > 0) io.tick(n); };

    const int np = (int)io.tiles.size();
    const Kernel eye = gauss(EYE_SIGMA, ER), win = gauss(SSIM_SIGMA, SR), pool = gauss(2.0f, 8);

    // block faces: plain blocks first, identical faces share a slot, then whatever else the grid shows
    static const unsigned char nothing[B * B * 4] = { 0 };
    Tiles T;
    std::vector<const unsigned char*> src;
    std::vector<int> slot(np, -1);
    {
        std::unordered_map<std::string, int> seen;
        for (int i = 0; i < np; ++i) {
            if (!io.placeable[i] || !io.tiles[i]) continue;
            auto ins = seen.emplace(std::string((const char*)io.tiles[i], B * B * 4), (int)src.size());
            if (ins.second) { src.push_back(io.tiles[i]); T.pal.push_back(i); }
            slot[i] = ins.first->second;
        }
    }
    T.m = (int)src.size();
    if (T.m < 2) {
        note = "The experimental features were skipped: fewer than two plain blocks are allowed.";
        return false;
    }
    std::vector<int> g(cells);
    int emptySlot = -1;
    for (int i = 0; i < cells; ++i) {
        const int idx = grid[i];
        if (idx < 0 || idx >= np || !io.tiles[idx]) {
            if (emptySlot < 0) { emptySlot = (int)src.size(); src.push_back(nothing); T.pal.push_back(-1); }
            g[i] = emptySlot;
            continue;
        }
        if (slot[idx] < 0) { slot[idx] = (int)src.size(); src.push_back(io.tiles[idx]); T.pal.push_back(idx); }
        g[i] = slot[idx];
    }
    T.n = (int)src.size();
    for (int sI = 0; sI < 2 && s.search; ++sI) {
        const int p = SCALE_P[sI], Q = p + 2 * SR;
        T.lab[sI].resize((size_t)T.n * p * p * 3);
        T.p1[sI].resize((size_t)T.m * Q * Q);
        T.p2[sI].resize((size_t)T.m * Q * Q);
    }
    T.meanW.resize(T.n * 3); T.meanR.resize(T.n * 3);
    T.busy.resize(T.m); T.colour.resize(T.m); T.coarse.resize(T.m); T.fine.resize(T.m); T.calm.resize(T.m);
    if (s.feather) { T.desc.resize((size_t)T.m * DESC); T.descSum.resize(T.m); }
    par((T.n + 15) / 16, [&](int k) {
        TileScratch* S = new TileScratch;
        for (int t = k * 16; t < std::min(T.n, k * 16 + 16); ++t) buildTile(t, src[t], io.bg, s.feather, s.search, T, eye, win, pool, *S);
        delete S;
        });
    if (cancelled()) return false;

    Scale sc[2];
    for (int sI = 0; sI < 2 && s.search; ++sI) {
        sc[sI].p = SCALE_P[sI];
        sc[sI].W = gw * SCALE_P[sI]; sc[sI].H = gh * SCALE_P[sI];
        sc[sI].tgt.resize((size_t)sc[sI].W * sc[sI].H * 3);
        windowNorms(sc[sI], win);
    }
    Cells C;
    C.labW.resize((size_t)cells * 3); C.labR.resize((size_t)cells * 3); C.light.resize(cells);
    if (s.search) { C.busy.resize(cells); C.colour.resize(cells); C.coarse.resize(cells); C.fine.resize(cells); }
    C.opaque.resize(cells); C.mess.assign(cells, 0);

    // the photo: feathering alone needs each cells mean colour, the texel work is for the search
    const int W = gw * B, H = gh * B, W4 = gw * 4, H4 = gh * 4;
    const int margin = s.search ? ER : 0;
    std::vector<float> lin4(s.search ? (size_t)W4 * H4 * 3 : 0);

    // more protection = more cells count as fine mess (hair) and stay as they are
    const float messFine = 0.05f * std::pow(0.24f, s.protect);
    const float messRatio = 1.5f - 0.7f * s.protect;

    par(gh, [&](int r) {
        if (cancelled()) return;
        const int rows = B + 2 * margin;
        std::vector<float> raw((size_t)rows * W * 4), hb(s.search ? (size_t)rows * W * 3 : 0);
        for (int i = 0; i < rows; ++i) io.fetchRow(std::clamp(r * B - margin + i, 0, H - 1), &raw[(size_t)i * W * 4]);

        float lab[B * B * 3];
        for (int c = 0; c < gw; ++c) {
            float mean[3] = { 0, 0, 0 }, quarter[4 * 4 * 3] = { 0 }, alpha = 1.0f;
            for (int y = 0; y < B; ++y) {
                const float* px = &raw[((size_t)(y + margin) * W + c * B) * 4];
                for (int x = 0; x < B; ++x, px += 4) {
                    if (s.search) toOklab(px, &lab[(y * B + x) * 3]);
                    float* q = &quarter[((y / 4) * 4 + x / 4) * 3];
                    for (int ch = 0; ch < 3; ++ch) { mean[ch] += px[ch]; q[ch] += px[ch]; }
                    alpha = std::min(alpha, px[3]);
                }
            }
            const int cell = r * gw + c;
            for (int y = 0; y < 4 && s.search; ++y)
                for (int x = 0; x < 4; ++x)
                    for (int ch = 0; ch < 3; ++ch)
                        lin4[((size_t)(r * 4 + y) * W4 + c * 4 + x) * 3 + ch] = quarter[(y * 4 + x) * 3 + ch] / 16.0f;
            for (float& v : mean) v /= B * B;
            float ml[3];
            toOklab(mean, ml);
            C.light[cell] = ml[0];
            C.labW[cell * 3] = ml[0]; C.labW[cell * 3 + 1] = ml[1] * CHROMA; C.labW[cell * 3 + 2] = ml[2] * CHROMA;
            C.labR[cell * 3] = ml[0]; C.labR[cell * 3 + 1] = ml[1] * RANGE_CHROMA; C.labR[cell * 3 + 2] = ml[2] * RANGE_CHROMA;
            C.opaque[cell] = alpha > 0.99f ? 1 : 0;
            if (!s.search) continue;
            const PatchStats ps = patchStats(lab);
            C.busy[cell] = ps.busy; C.colour[cell] = ps.colour; C.coarse[cell] = ps.coarse; C.fine[cell] = ps.fine;
            if (s.protect > 0) C.mess[cell] = (ps.fine > messFine && ps.fine > messRatio * ps.coarse) ? 1 : 0;
        }
        if (!s.search) { tick(gw * TICKS_PREP_LIGHT); return; }

        // the eye blur at full size, then into oklab
        for (int i = 0; i < rows; ++i) {
            const float* in = &raw[(size_t)i * W * 4];
            float* out = &hb[(size_t)i * W * 3];
            for (int x = 0; x < W; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    float acc = 0;
                    for (int k = -ER; k <= ER; ++k) acc += eye.w[k + ER] * in[std::clamp(x + k, 0, W - 1) * 4 + ch];
                    out[x * 3 + ch] = acc;
                }
        }
        for (int y = 0; y < B; ++y) {
            int16_t* dst = &sc[0].tgt[((size_t)(r * B + y) * W) * 3];
            for (int x = 0; x < W; ++x) {
                float v[3] = { 0, 0, 0 }, o[3];
                for (int k = 0; k <= 2 * ER; ++k) {
                    const float* in = &hb[((size_t)(y + k) * W + x) * 3];
                    v[0] += eye.w[k] * in[0]; v[1] += eye.w[k] * in[1]; v[2] += eye.w[k] * in[2];
                }
                toOklab(v, o);
                dst[x * 3] = pack15(o[0]); dst[x * 3 + 1] = pack15(o[1]); dst[x * 3 + 2] = pack15(o[2]);
            }
        }
        tick(gw * TICKS_PREP_SEARCH);
        });
    if (cancelled()) return false;

    // the same at a quarter size, what the mural looks like from afar
    if (s.search) {
        std::vector<float> tmp(lin4.size());
        par(H4, [&](int y) {
            for (int x = 0; x < W4; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    float acc = 0;
                    for (int k = -ER; k <= ER; ++k) acc += eye.w[k + ER] * lin4[((size_t)y * W4 + std::clamp(x + k, 0, W4 - 1)) * 3 + ch];
                    tmp[((size_t)y * W4 + x) * 3 + ch] = acc;
                }
            });
        par(H4, [&](int y) {
            int16_t* dst = &sc[1].tgt[(size_t)y * W4 * 3];
            for (int x = 0; x < W4; ++x) {
                float v[3] = { 0, 0, 0 }, o[3];
                for (int k = -ER; k <= ER; ++k) {
                    const float* in = &tmp[((size_t)std::clamp(y + k, 0, H4 - 1) * W4 + x) * 3];
                    v[0] += eye.w[k + ER] * in[0]; v[1] += eye.w[k + ER] * in[1]; v[2] += eye.w[k + ER] * in[2];
                }
                toOklab(v, o);
                dst[x * 3] = pack15(o[0]); dst[x * 3 + 1] = pack15(o[1]); dst[x * 3 + 2] = pack15(o[2]);
            }
            });
        std::vector<float>().swap(lin4);
    }

    {

        // detail or flat, judged between cells: fine strands inside one cell arent structure
        const Kernel k = gauss(DETAIL_SIGMA, 6);
        std::vector<float> sq(cells), m, m2;
        for (int i = 0; i < cells; ++i) sq[i] = C.light[i] * C.light[i];
        blurCells(C.light, gw, gh, k, m);
        blurCells(sq, gw, gh, k, m2);
        C.detail.resize(cells); C.flat.resize(cells);
        const float amountOfFlat = std::clamp(s.flat, 0.0f, 1.0f);
        for (int i = 0; i < cells; ++i) {
            const float v = std::max(m2[i] - m[i] * m[i], 0.0f);
            C.detail[i] = v / (v + DETAIL_TAU * DETAIL_TAU);
            C.flat[i] = amountOfFlat * (1.0f - C.detail[i]);
        }
    }

    // search
    const std::vector<int> start = g;
    int swapped = 0;
    if (s.search) {

    // overlays, see-through cells + mess stay as they are
    std::vector<char> frozen(cells);
    for (int i = 0; i < cells; ++i) frozen[i] = (g[i] >= T.m || !C.opaque[i] || C.mess[i]) ? 1 : 0;

    const float grain = std::clamp(s.grain, 0.0f, 1.0f);
    Search search{ gw, gh, T, C, sc, win, g,
        0.14f * grain,    // texture a block may carry beyond the photos
        std::clamp((grain - 0.5f) / 0.25f, 0.0f, 1.0f),    // past the middle too little texture costs too
        std::max(0.0f, 0.12f * (grain - 0.5f)) };    // and flat areas are asked for some

    std::vector<char> dirty(cells, 1), changed(cells, 0);
    std::vector<int> list;
    uint64_t seed = 0;
    for (int sweep = 0; sweep < SWEEPS; ++sweep) {
        std::fill(changed.begin(), changed.end(), 0);

        // cell classes in random order, the cells of one class run in parallel
        int order[STRIDE * STRIDE];
        for (int i = 0; i < STRIDE * STRIDE; ++i) order[i] = i;
        for (int i = STRIDE * STRIDE - 1; i > 0; --i) std::swap(order[i], order[nextRandom(seed) % (i + 1)]);

        for (int cls : order) {
            list.clear();
            int inClass = 0;
            for (int r = cls / STRIDE; r < gh; r += STRIDE)
                for (int c = cls % STRIDE; c < gw; c += STRIDE) {
                    ++inClass;
                    if (dirty[r * gw + c] && !frozen[r * gw + c]) list.push_back(r * gw + c);
                }
            const int n = (int)list.size();
            par((n + CHUNK - 1) / CHUNK, [&](int k) {
                if (cancelled()) return;
                Scratch S;
                const int end = std::min(n, (k + 1) * CHUNK);
                for (int i = k * CHUNK; i < end; ++i) {
                    const int cell = list[i];
                    const int best = search.pick(cell / gw, cell % gw, S);
                    if (best != g[cell]) { g[cell] = best; changed[cell] = 1; }
                }
                tick((end - k * CHUNK) * TICKS_SWEEP);
                });
            if (cancelled()) return false;
            tick((inClass - n) * TICKS_SWEEP);
        }

        // only cells within REACH of a change look again next sweep
        bool any = false;
        std::fill(dirty.begin(), dirty.end(), 0);
        for (int r = 0; r < gh; ++r)
            for (int c = 0; c < gw; ++c) {
                if (!changed[r * gw + c]) continue;
                any = true; ++swapped;
                for (int rr = std::max(0, r - REACH); rr <= std::min(gh - 1, r + REACH); ++rr)
                    for (int cc = std::max(0, c - REACH); cc <= std::min(gw - 1, c + REACH); ++cc) dirty[rr * gw + cc] = 1;
            }
        if (!any) {
            tick((SWEEPS - 1 - sweep) * cells * TICKS_SWEEP);
            break;
        }
    }
    }
    (void)swapped;

    // feathering
    int mixed = 0;
    std::vector<int> fg, fgStart;
    if (s.feather) {

        // plain block per cell, the carrier under an overlay
        fg.assign(cells, -1);
        std::vector<char> smooth(cells, 0);
        for (int i = 0; i < cells; ++i) {
            if (g[i] < T.m) fg[i] = g[i];
            else if (T.pal[g[i]] >= 0 && T.pal[g[i]] < (int)io.baseOf.size()) {
                const int base = io.baseOf[T.pal[g[i]]];
                if (base >= 0 && base < np && slot[base] >= 0 && slot[base] < T.m) fg[i] = slot[base];
            }
            smooth[i] = (fg[i] >= 0 && C.opaque[i] && !C.mess[i] && C.detail[i] < FEATHER_DETAIL) ? 1 : 0;
        }
        fgStart = fg;
        mixed = feather(fg, T, C, smooth, s.cleanup ? FEATHER_TRIM : 0.0f, s.featherKeep, gw, gh);
    }
    tick(cells * TICKS_FEATHER);
    if (cancelled()) return false;

    // back to palette entries
    for (int i = 0; i < cells; ++i) {
        int entry = grid[i];
        const bool feathered = s.feather && fg[i] != fgStart[i];
        if (g[i] < T.m) {

            // a cell that ends on the face it started with keeps its own entry: two blocks can share a face
            const int face = feathered ? fg[i] : g[i];
            if (face != start[i]) entry = T.pal[face];
        }
        else if (feathered) {

            // an overlay keeps its place and gets the new block underneath, where the palette has that pair
            const int moved = io.rebase ? io.rebase(entry, T.pal[fg[i]]) : -1;
            if (moved >= 0) entry = moved;
        }
        grid[i] = entry;
    }
    (void)mixed;
    return true;
}
