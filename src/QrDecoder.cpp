#include "QrDecoder.h"

#include "Logger.h"

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <propidl.h>
#pragma comment(lib, "gdiplus.lib")
#include <gdiplus.h>

namespace clipsync {

namespace {

// ================================================================
//  GF(2^8) + Reed-Solomon 解码
// ================================================================

uint8_t g_exp[512];
uint8_t g_log[256];
bool g_gfReady = false;

void initGF() {
    if (g_gfReady) return;
    int x = 1;
    for (int i = 0; i < 255; ++i) {
        g_exp[i] = static_cast<uint8_t>(x);
        g_log[x] = static_cast<uint8_t>(i);
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; ++i) g_exp[i] = g_exp[i - 255];
    g_gfReady = true;
}

inline uint8_t gfMul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return g_exp[g_log[a] + g_log[b]];
}

inline uint8_t gfDiv(uint8_t a, uint8_t b) {
    if (a == 0) return 0;
    if (b == 0) return 0;
    return g_exp[(g_log[a] + 255 - g_log[b]) % 255];
}

inline uint8_t gfPow(uint8_t a, int n) {
    if (n == 0) return 1;
    if (a == 0) return 0;
    return g_exp[(g_log[a] * n) % 255];
}

bool rsDecodeBlock(std::vector<uint8_t>& data, int ecLen) {
    if (ecLen <= 0 || static_cast<int>(data.size()) <= ecLen) return false;
    const int n = static_cast<int>(data.size());
    std::vector<uint8_t> synd(ecLen, 0);
    bool hasErr = false;
    for (int i = 0; i < ecLen; ++i) {
        uint8_t s = 0;
        for (int j = 0; j < n; ++j) {
            s = gfMul(s, g_exp[i + 1]) ^ data[static_cast<size_t>(j)];
        }
        synd[static_cast<size_t>(i)] = s;
        if (s != 0) hasErr = true;
    }
    if (!hasErr) return true;

    std::vector<uint8_t> errLoc(ecLen + 1, 0);
    errLoc[0] = 1;
    std::vector<uint8_t> oldLoc(ecLen + 1, 0);
    oldLoc[0] = 1;
    int errLocLen = 1;
    int oldLocLen = 1;
    int discrMismatch = 0;

    for (int r = 0; r < ecLen; ++r) {
        const uint8_t discr = synd[static_cast<size_t>(r)];
        if (discr != 0) {
            std::vector<uint8_t> temp(errLoc);
            int tempLen = errLocLen;
            for (int i = 0; i < oldLocLen; ++i) {
                if (i + r - discrMismatch >= tempLen) temp.resize(static_cast<size_t>(i + r - discrMismatch + 1));
                temp[static_cast<size_t>(i + r - discrMismatch)] ^=
                    gfMul(oldLoc[static_cast<size_t>(i)], discr);
            }
            if (2 * discrMismatch > r) {
                const uint8_t scale = gfDiv(1, discr);
                for (int i = 0; i < oldLocLen; ++i) oldLoc[static_cast<size_t>(i)] = gfMul(oldLoc[static_cast<size_t>(i)], scale);
                oldLocLen = errLocLen;
                errLoc = temp;
                errLocLen = static_cast<int>(temp.size());
                discrMismatch = r + 1 - discrMismatch;
            } else {
                oldLoc.resize(static_cast<size_t>(errLocLen + 1));
                for (int i = 0; i < errLocLen; ++i) oldLoc[static_cast<size_t>(i)] = errLoc[static_cast<size_t>(i)];
                oldLocLen = errLocLen;
                if (r - discrMismatch + 1 >= errLocLen) errLoc.resize(static_cast<size_t>(r - discrMismatch + 2));
                errLoc[static_cast<size_t>(r - discrMismatch + 1)] = discr;
                errLocLen = static_cast<int>(errLoc.size());
            }
        } else {
            discrMismatch += 1;
        }
    }

    int numErrors = errLocLen - 1;
    if (numErrors <= 0) return false;
    if (numErrors > ecLen / 2) return false;

    std::vector<int> errPos(static_cast<size_t>(numErrors));
    int found = 0;
    for (int i = 0; i < n; ++i) {
        if (gfPow(static_cast<uint8_t>(i < 255 ? i : 255), 1) == 0) continue;
        uint8_t sum = 0;
        for (int j = 0; j < errLocLen; ++j) {
            sum ^= gfMul(errLoc[static_cast<size_t>(j)], gfPow(static_cast<uint8_t>(i % 255), j));
        }
        if (sum == 0) {
            if (found >= numErrors) return false;
            errPos[static_cast<size_t>(found++)] = n - 1 - i;
        }
    }
    if (found != numErrors) return false;

    std::vector<uint8_t> errMag(static_cast<size_t>(numErrors));
    for (int i = 0; i < numErrors; ++i) {
        uint8_t xi = gfPow(static_cast<uint8_t>((n - 1 - errPos[static_cast<size_t>(i)]) % 255), 1);
        uint8_t denom = 1;
        for (int j = 0; j < numErrors; ++j) {
            if (i == j) continue;
            uint8_t xj = gfPow(static_cast<uint8_t>((n - 1 - errPos[static_cast<size_t>(j)]) % 255), 1);
            denom = gfMul(denom, (xi ^ xj));
        }
        uint8_t num = 0;
        for (int j = 0; j < ecLen; ++j) {
            num ^= gfMul(synd[static_cast<size_t>(j)], gfPow(xi, j));
        }
        errMag[static_cast<size_t>(i)] = gfDiv(num, denom);
    }

    for (int i = 0; i < numErrors; ++i) {
        data[static_cast<size_t>(errPos[static_cast<size_t>(i)])] ^=
            errMag[static_cast<size_t>(i)];
    }
    return true;
}

// ================================================================
//  QR 规格表
// ================================================================

constexpr uint16_t kFormatInfo[32] = {
    0x5412, 0x5125, 0x5E7C, 0x5B4B, 0x76F3, 0x737E, 0x7265, 0x7050,
    0x50D0, 0x5E77, 0x5A4E, 0x567B, 0x72E3, 0x7076, 0x7341, 0x7574,
    0x41C0, 0x4D97, 0x49AE, 0x47DB, 0x6363, 0x6506, 0x6431, 0x6204,
    0x4310, 0x4547, 0x417E, 0x4D4B, 0x69F3, 0x6BE6, 0x6A81, 0x68B4,
};

struct EcSpec {
    int totalCodewords;
    int ecPerBlock;
    int numBlocks1;
    int dataPerBlock1;
    int numBlocks2;
    int dataPerBlock2;
};

// [version-1][ecLevel]
constexpr EcSpec kEcSpec[40][4] = {
    {{26,7,1,19,0,0},{26,10,1,16,0,0},{26,13,1,13,0,0},{26,17,1,9,0,0}},
    {{44,10,1,34,0,0},{44,16,1,28,0,0},{44,22,1,22,0,0},{44,28,1,16,0,0}},
    {{70,15,1,55,0,0},{70,26,1,44,0,0},{70,36,1,34,0,0},{70,44,1,26,0,0}},
    {{100,20,1,80,0,0},{100,36,1,64,0,0},{100,52,1,46,0,0},{100,64,1,36,0,0}},
    {{134,26,1,108,0,0},{134,48,1,86,0,0},{134,72,1,62,0,0},{134,88,1,46,0,0}},
    {{172,36,1,136,0,0},{172,64,1,108,0,0},{172,96,1,76,0,0},{172,112,1,60,0,0}},
    {{196,40,2,43,0,0},{196,72,2,36,0,0},{196,108,2,27,0,0},{196,130,2,19,0,0}},
    {{242,48,2,53,0,0},{242,88,2,45,0,0},{242,132,2,33,0,0},{242,156,2,25,0,0}},
    {{292,60,2,64,0,0},{292,110,2,54,0,0},{292,160,2,40,0,0},{292,192,2,30,0,0}},
    {{346,72,2,76,0,0},{346,130,2,64,0,0},{346,192,2,48,0,0},{346,224,2,36,0,0}},
    {{404,80,4,57,0,0},{404,150,4,48,0,0},{404,224,4,34,0,0},{404,264,4,26,0,0}},
    {{466,96,4,66,0,0},{466,176,4,56,0,0},{466,260,4,40,0,0},{466,310,4,30,0,0}},
    {{532,104,4,76,0,0},{532,198,4,64,0,0},{532,288,4,46,0,0},{532,338,4,34,0,0}},
    {{581,120,5,72,1,73},{581,216,5,60,1,61},{581,320,5,44,1,45},{581,380,5,34,1,35}},
    {{655,132,5,81,1,82},{655,240,5,68,1,69},{655,360,5,50,1,51},{655,432,5,38,1,39}},
    {{733,144,5,91,1,92},{733,280,5,76,1,77},{733,400,5,56,1,57},{733,480,5,44,1,45}},
    {{815,168,5,101,1,102},{815,308,5,86,1,87},{815,450,5,64,1,65},{815,538,5,50,1,51}},
    {{901,180,6,91,2,92},{901,338,6,77,2,78},{901,504,6,57,2,58},{901,600,6,45,2,46}},
    {{991,196,6,101,2,102},{991,364,6,85,2,86},{991,546,6,63,2,64},{991,650,6,51,2,52}},
    {{1085,224,7,103,3,104},{1085,416,7,87,3,88},{1085,600,7,65,3,66},{1085,700,7,53,3,54}},
    {{1156,224,8,103,4,104},{1156,442,8,87,4,88},{1156,644,8,65,4,66},{1156,750,8,53,4,54}},
    {{1258,252,8,115,4,116},{1258,476,8,97,4,98},{1258,690,8,71,4,72},{1258,800,8,57,4,58}},
    {{1364,270,8,125,4,126},{1364,504,8,105,4,106},{1364,750,8,77,4,78},{1364,900,8,63,4,64}},
    {{1462,300,8,139,4,140},{1462,560,8,117,4,118},{1462,810,8,87,4,88},{1462,960,8,69,4,70}},
    {{1528,312,8,145,4,146},{1528,588,8,123,4,124},{1528,870,8,91,4,92},{1528,1050,8,73,4,74}},
    {{1628,336,8,155,4,156},{1628,644,8,131,4,132},{1628,952,8,99,4,100},{1628,1110,8,79,4,80}},
    {{1724,360,8,165,4,166},{1724,700,8,139,4,140},{1724,1020,8,105,4,106},{1724,1200,8,85,4,86}},
    {{1802,390,8,173,4,174},{1802,750,8,147,4,148},{1802,1100,8,111,4,112},{1802,1260,8,89,4,90}},
    {{1932,420,8,185,4,186},{1932,800,8,157,4,158},{1932,1170,8,119,4,120},{1932,1380,8,95,4,96}},
    {{2045,450,8,197,4,198},{2045,850,8,167,4,168},{2045,1230,8,127,4,128},{2045,1500,8,103,4,104}},
    {{2158,480,8,209,4,210},{2158,900,8,177,4,178},{2158,1350,8,135,4,136},{2158,1620,8,109,4,110}},
    {{2302,510,8,223,4,224},{2302,950,8,189,4,190},{2302,1410,8,143,4,144},{2302,1680,8,115,4,116}},
    {{2432,540,8,235,4,236},{2432,1000,8,199,4,200},{2432,1470,8,151,4,152},{2432,1770,8,123,4,124}},
    {{2566,570,8,249,4,250},{2566,1060,8,211,4,212},{2566,1560,8,159,4,160},{2566,1860,8,129,4,130}},
    {{2702,570,8,249,4,250},{2702,1120,8,211,4,212},{2702,1650,8,159,4,160},{2702,1950,8,129,4,130}},
    {{2812,600,8,261,4,262},{2812,1190,8,221,4,222},{2812,1740,8,167,4,168},{2812,2100,8,135,4,136}},
    {{2956,630,8,275,4,276},{2956,1260,8,233,4,234},{2956,1830,8,177,4,178},{2956,2220,8,143,4,144}},
    {{3081,660,8,287,4,288},{3081,1310,8,243,4,244},{3081,1920,8,185,4,186},{3081,2340,8,151,4,152}},
    {{3244,720,8,301,4,302},{3244,1370,8,255,4,256},{3244,2010,8,195,4,196},{3244,2430,8,159,4,160}},
    {{3417,750,8,317,4,318},{3417,1452,8,269,4,270},{3417,2130,8,207,4,208},{3417,2550,8,169,4,170}},
};

bool applyMask(int mask, int row, int col) {
    switch (mask) {
        case 0: return (row + col) % 2 == 0;
        case 1: return row % 2 == 0;
        case 2: return col % 3 == 0;
        case 3: return (row + col) % 3 == 0;
        case 4: return (row / 2 + col / 3) % 2 == 0;
        case 5: return ((row * col) % 2) + ((row * col) % 3) == 0;
        case 6: return (((row * col) % 2) + ((row * col) % 3)) % 2 == 0;
        case 7: return (((row + col) % 2) + ((row * col) % 3)) % 2 == 0;
        default: return false;
    }
}

// ================================================================
//  二值图像
// ================================================================

class BitMatrix {
public:
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bits;

    bool get(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return false;
        return bits[static_cast<size_t>(y) * width + static_cast<size_t>(x)] != 0;
    }
    void set(int x, int y, bool v) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        bits[static_cast<size_t>(y) * width + static_cast<size_t>(x)] = v ? 1 : 0;
    }
};

ULONG_PTR g_gdiToken = 0;

bool ensureGdiPlus() {
    if (g_gdiToken) return true;
    Gdiplus::GdiplusStartupInput input{};
    return Gdiplus::GdiplusStartup(&g_gdiToken, &input, nullptr) == Gdiplus::Ok;
}

bool loadBitmap(Gdiplus::Bitmap& bmp, BitMatrix& out) {
    const int w = static_cast<int>(bmp.GetWidth());
    const int h = static_cast<int>(bmp.GetHeight());
    if (w <= 0 || h <= 0) return false;

    const int maxDim = 1200;
    int tw = w, th = h;
    if (w > maxDim || h > maxDim) {
        const double s = static_cast<double>(maxDim) / std::max(w, h);
        tw = static_cast<int>(w * s);
        th = static_cast<int>(h * s);
    }

    std::vector<uint8_t> gray(static_cast<size_t>(tw) * th);
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            const int sx = static_cast<int>((static_cast<double>(x) + 0.5) * w / tw);
            const int sy = static_cast<int>((static_cast<double>(y) + 0.5) * h / th);
            Gdiplus::Color c;
            bmp.GetPixel(sx, sy, &c);
            gray[static_cast<size_t>(y) * tw + static_cast<size_t>(x)] =
                static_cast<uint8_t>((c.GetR() * 77 + c.GetG() * 150 + c.GetB() * 29) >> 8);
        }
    }

    std::vector<int> hist(256, 0);
    for (uint8_t v : gray) hist[v]++;
    int total = tw * th;
    float sum = 0;
    for (int i = 0; i < 256; ++i) sum += i * hist[i];
    float sumB = 0;
    int wB = 0;
    float maxVar = 0;
    int threshold = 128;
    for (int i = 0; i < 256; ++i) {
        wB += hist[i];
        if (wB == 0) continue;
        const int wF = total - wB;
        if (wF == 0) break;
        sumB += static_cast<float>(i * hist[i]);
        const float mB = sumB / wB;
        const float mF = (sum - sumB) / wF;
        const float varBetween = static_cast<float>(wB) * wF * (mB - mF) * (mB - mF);
        if (varBetween > maxVar) {
            maxVar = varBetween;
            threshold = i;
        }
    }

    out.width = tw;
    out.height = th;
    out.bits.resize(static_cast<size_t>(tw) * th);
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            out.set(x, y, gray[static_cast<size_t>(y) * tw + static_cast<size_t>(x)] < threshold);
        }
    }
    return true;
}

// ================================================================
//  定位图案检测
// ================================================================

struct Finder {
    float x = 0;
    float y = 0;
    float moduleSize = 0;
};

bool ratioMatch(const int* counts, int total, float moduleSize) {
    const float maxDev = moduleSize * 0.5f;
    const float unit = static_cast<float>(total) / 7.0f;
    const float expected[5] = {1, 1, 3, 1, 1};
    for (int i = 0; i < 5; ++i) {
        if (std::fabs(counts[i] - expected[i] * unit) > maxDev) return false;
    }
    return true;
}

bool findPatternInLine(const int* counts, int start, int fixed, bool horizontal,
                       float& cx, float& cy, float& modSize) {
    int total = 0;
    for (int i = 0; i < 5; ++i) total += counts[i];
    if (total < 7) return false;
    const float unit = static_cast<float>(total) / 7.0f;
    if (!ratioMatch(counts, total, unit)) return false;

    int end = start;
    for (int i = 0; i < 5; ++i) end += counts[i];
    const float center = static_cast<float>(start + end) / 2.0f;
    if (horizontal) {
        cx = center;
        cy = static_cast<float>(fixed);
    } else {
        cx = static_cast<float>(fixed);
        cy = center;
    }
    modSize = unit;
    return true;
}

void scanLine(const BitMatrix& img, int fixed, bool horizontal, std::vector<Finder>& out) {
    const int len = horizontal ? img.width : img.height;
    int counts[5]{};
    int idx = 0;
    bool current = false;
    int run = 0;

    auto flush = [&](int pos) {
        if (idx < 5) return;
        float cx = 0, cy = 0, ms = 0;
        int start = pos;
        for (int i = 4; i >= 0; --i) start -= counts[i];
        if (findPatternInLine(counts, start, fixed, horizontal, cx, cy, ms)) {
            Finder f;
            f.x = horizontal ? cx : static_cast<float>(fixed);
            f.y = horizontal ? static_cast<float>(fixed) : cy;
            f.moduleSize = ms;
            out.push_back(f);
        }
        counts[0] = counts[2];
        counts[1] = counts[3];
        counts[2] = counts[4];
        counts[3] = run;
        counts[4] = 0;
        idx = 4;
    };

    for (int i = 0; i < len; ++i) {
        const bool pix = horizontal ? img.get(i, fixed) : img.get(fixed, i);
        if (pix == current) {
            counts[idx]++;
            run++;
        } else {
            if (idx == 5) flush(i);
            idx++;
            counts[idx] = 1;
            run = 1;
            current = pix;
        }
    }
    if (idx == 5) flush(len);
}

float dist(const Finder& a, const Finder& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool clusterFinders(const std::vector<Finder>& cands, Finder& tl, Finder& tr, Finder& bl) {
    if (cands.size() < 3) return false;
    float bestScore = -1;
    bool found = false;
    for (size_t i = 0; i < cands.size(); ++i) {
        for (size_t j = i + 1; j < cands.size(); ++j) {
            for (size_t k = j + 1; k < cands.size(); ++k) {
                const Finder* p[3] = {&cands[i], &cands[j], &cands[k]};
                for (int perm = 0; perm < 6; ++perm) {
                    const Finder* a = p[perm % 3];
                    const Finder* b = p[(perm + 1) % 3];
                    const Finder* c = p[(perm + 2) % 3];
                    const float ab = dist(*a, *b);
                    const float ac = dist(*a, *c);
                    const float bc = dist(*b, *c);
                    const float maxD = std::max({ab, ac, bc});
                    const float minD = std::min({ab, ac, bc});
                    const float midD = ab + ac + bc - maxD - minD;
                    if (std::fabs(maxD - midD) > maxD * 0.2f) continue;
                    if (std::fabs(minD - midD) > midD * 0.2f) continue;
                    const float modDiff = std::max({
                        std::fabs(a->moduleSize - b->moduleSize),
                        std::fabs(a->moduleSize - c->moduleSize),
                        std::fabs(b->moduleSize - c->moduleSize)});
                    if (modDiff > (a->moduleSize + b->moduleSize + c->moduleSize) / 6.0f) continue;
                    const float score = maxD - modDiff;
                    if (score > bestScore) {
                        bestScore = score;
                        const Finder* corner = a;
                        const Finder* p1 = b;
                        const Finder* p2 = c;
                        if (p1->x > p2->x) std::swap(p1, p2);
                        tl = *corner;
                        tr = *p2;
                        bl = *p1;
                        if (bl.y < tr.y) std::swap(tr, bl);
                        found = true;
                    }
                }
            }
        }
    }
    return found;
}

// ================================================================
//  矩阵采样与解码
// ================================================================

bool sampleMatrix(const BitMatrix& img, const Finder& tl, const Finder& tr, const Finder& bl,
                  int dimension, std::vector<uint8_t>& modules) {
    const float modSize = (dist(tl, tr) + dist(tl, bl)) / (dimension * 2.0f - 14.0f);
    const float topLen = dist(tl, tr);
    const float leftLen = dist(tl, bl);
    const float estDim = (topLen + leftLen) / (2.0f * modSize) + 7.0f;
    if (std::fabs(estDim - dimension) > 4.0f) return false;

    const float dxX = (tr.x - tl.x) / static_cast<float>(dimension - 1);
    const float dxY = (tr.y - tl.y) / static_cast<float>(dimension - 1);
    const float dyX = (bl.x - tl.x) / static_cast<float>(dimension - 1);
    const float dyY = (bl.y - tl.y) / static_cast<float>(dimension - 1);

    modules.assign(static_cast<size_t>(dimension) * dimension, 0);
    for (int row = 0; row < dimension; ++row) {
        for (int col = 0; col < dimension; ++col) {
            const float px = tl.x - 3.0f * dxX - 3.0f * dyX +
                             static_cast<float>(col) * dxX + static_cast<float>(row) * dyX;
            const float py = tl.y - 3.0f * dxY - 3.0f * dyY +
                             static_cast<float>(col) * dxY + static_cast<float>(row) * dyY;
            int dark = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (img.get(static_cast<int>(px + dx * modSize * 0.3f),
                                static_cast<int>(py + dy * modSize * 0.3f))) {
                        dark++;
                    }
                }
            }
            modules[static_cast<size_t>(row) * dimension + static_cast<size_t>(col)] =
                (dark >= 5) ? 1 : 0;
        }
    }
    return true;
}

bool isFunctionModule(int row, int col, int sz) {
    if (row == 6 || col == 6) return true;
    if (row < 9 && col < 9) return true;
    if (row < 9 && col >= sz - 8) return true;
    if (row >= sz - 8 && col < 9) return true;
    if (row == 8 && col == 8) return true;
    if (row == 8 && (col < 9 || col >= sz - 8)) return true;
    if (col == 8 && (row < 9 || row >= sz - 8)) return true;
    return false;
}

bool readFormatInfo(const std::vector<uint8_t>& modules, int sz, int& ecLevel, int& mask) {
    std::vector<bool> bits;
    bits.reserve(30);
    for (int i = 0; i < 15; ++i) {
        int r, c;
        if (i < 6) { r = 8; c = i; }
        else if (i < 8) { r = 8; c = i + 1; }
        else if (i == 8) { r = 7; c = 8; }
        else { r = 14 - i; c = 8; }
        bits.push_back(modules[static_cast<size_t>(r) * sz + static_cast<size_t>(c)] != 0);
    }
    for (int i = 0; i < 15; ++i) {
        int r, c;
        if (i < 8) { r = sz - 1 - i; c = 8; }
        else if (i < 9) { r = 8; c = sz - 7 + (i - 8); }
        else { r = 8; c = 14 - i; }
        bits.push_back(modules[static_cast<size_t>(r) * sz + static_cast<size_t>(c)] != 0);
    }

    int bestDist = 99;
    int bestEc = 0, bestMask = 0;
    for (int idx = 0; idx < 32; ++idx) {
        const uint16_t fmt = kFormatInfo[idx];
        int dist = 0;
        for (int i = 0; i < 15; ++i) {
            const bool b = ((fmt >> (14 - i)) & 1) != 0;
            if (bits[static_cast<size_t>(i)] != b) dist++;
            if (bits[static_cast<size_t>(15 + i)] != b) dist++;
        }
        if (dist < bestDist) {
            bestDist = dist;
            bestEc = idx / 8;
            bestMask = idx % 8;
        }
    }
    if (bestDist > 6) return false;
    ecLevel = bestEc;
    mask = bestMask;
    return true;
}

void unmaskModules(std::vector<uint8_t>& modules, int sz, int mask) {
    for (int r = 0; r < sz; ++r) {
        for (int c = 0; c < sz; ++c) {
            if (isFunctionModule(r, c, sz)) continue;
            if (applyMask(mask, r, c)) {
                const size_t idx = static_cast<size_t>(r) * sz + static_cast<size_t>(c);
                modules[idx] ^= 1;
            }
        }
    }
}

bool extractCodewords(const std::vector<uint8_t>& modules, int sz, std::vector<uint8_t>& out) {
    out.clear();
    int bitIdx = 0;
    uint8_t current = 0;
    const int totalModules = sz * sz;
    std::vector<bool> bits;
    bits.reserve(static_cast<size_t>(totalModules));

    for (int right = sz - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < sz; ++vert) {
            const bool upward = ((sz - 1 - right) / 2) % 2 == 0;
            const int row = upward ? (sz - 1 - vert) : vert;
            for (int colOff = 0; colOff < 2; ++colOff) {
                const int col = right - colOff;
                if (!isFunctionModule(row, col, sz)) {
                    bits.push_back(modules[static_cast<size_t>(row) * sz + static_cast<size_t>(col)] != 0);
                }
            }
        }
    }

    for (bool b : bits) {
        current = static_cast<uint8_t>((current << 1) | (b ? 1 : 0));
        bitIdx++;
        if (bitIdx == 8) {
            out.push_back(current);
            current = 0;
            bitIdx = 0;
        }
    }
    return !out.empty();
}

struct BitReader {
    const std::vector<uint8_t>& data;
    size_t bitPos = 0;

    int readBits(int count) {
        int val = 0;
        for (int i = 0; i < count; ++i) {
            const size_t byteIdx = bitPos / 8;
            if (byteIdx >= data.size()) return -1;
            const int bitIdx = 7 - static_cast<int>(bitPos % 8);
            val = (val << 1) | ((data[byteIdx] >> bitIdx) & 1);
            bitPos++;
        }
        return val;
    }
};

bool decodeDataCodewords(const std::vector<uint8_t>& dataBytes, int version,
                         std::string& outUtf8) {
    if (dataBytes.empty()) return false;
    BitReader br{dataBytes};
    const int mode = br.readBits(4);
    if (mode == 0) return false;
    if (mode != 4) return false;

    const int cciBits = (version <= 9) ? 8 : 16;
    const int count = br.readBits(cciBits);
    if (count <= 0) return false;

    outUtf8.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int ch = br.readBits(8);
        if (ch < 0) break;
        outUtf8.push_back(static_cast<char>(ch));
    }
    return !outUtf8.empty();
}

bool decodeModules(const std::vector<uint8_t>& modules, int sz, std::string& outUtf8) {
    initGF();
    int ecLevel = 0, mask = 0;
    if (!readFormatInfo(modules, sz, ecLevel, mask)) return false;

    auto working = modules;
    unmaskModules(working, sz, mask);

    std::vector<uint8_t> codewords;
    if (!extractCodewords(working, sz, codewords)) return false;

    const int version = (sz - 17) / 4;
    if (version < 1 || version > 40) return false;
    const EcSpec& spec = kEcSpec[version - 1][ecLevel];
    if (static_cast<int>(codewords.size()) < spec.totalCodewords) return false;
    codewords.resize(static_cast<size_t>(spec.totalCodewords));

    const int totalBlocks = spec.numBlocks1 + spec.numBlocks2;
    const int ecLen = spec.ecPerBlock;
    const int maxData = std::max(spec.dataPerBlock1, spec.dataPerBlock2);

    std::vector<int> dataLens(static_cast<size_t>(totalBlocks));
    for (int i = 0; i < spec.numBlocks1; ++i) {
        dataLens[static_cast<size_t>(i)] = spec.dataPerBlock1;
    }
    for (int i = 0; i < spec.numBlocks2; ++i) {
        dataLens[static_cast<size_t>(spec.numBlocks1 + i)] = spec.dataPerBlock2;
    }

    std::vector<std::vector<uint8_t>> blocks(static_cast<size_t>(totalBlocks));
    int offset = 0;
    for (int byteIdx = 0; byteIdx < maxData; ++byteIdx) {
        for (int b = 0; b < totalBlocks; ++b) {
            if (byteIdx < dataLens[static_cast<size_t>(b)]) {
                blocks[static_cast<size_t>(b)].push_back(
                    codewords[static_cast<size_t>(offset++)]);
            }
        }
    }
    for (int ecIdx = 0; ecIdx < ecLen; ++ecIdx) {
        for (int b = 0; b < totalBlocks; ++b) {
            blocks[static_cast<size_t>(b)].push_back(
                codewords[static_cast<size_t>(offset++)]);
        }
    }
    if (offset != spec.totalCodewords) return false;

    std::vector<uint8_t> dataBytes;
    for (int b = 0; b < totalBlocks; ++b) {
        auto& block = blocks[static_cast<size_t>(b)];
        const int dataLen = dataLens[static_cast<size_t>(b)];
        if (!rsDecodeBlock(block, ecLen)) return false;
        dataBytes.insert(dataBytes.end(), block.begin(), block.begin() + dataLen);
    }

    return decodeDataCodewords(dataBytes, version, outUtf8);
}

bool decodeImage(Gdiplus::Bitmap& bmp, std::string& outUtf8, std::wstring& errMsg) {
    if (!ensureGdiPlus()) {
        errMsg = L"GDI+ 初始化失败";
        return false;
    }

    BitMatrix img;
    if (!loadBitmap(bmp, img)) {
        errMsg = L"无法读取图片";
        return false;
    }

    std::vector<Finder> cands;
    for (int y = 0; y < img.height; ++y) scanLine(img, y, true, cands);
    for (int x = 0; x < img.width; ++x) scanLine(img, x, false, cands);
    if (cands.empty()) {
        errMsg = L"未检测到二维码定位图案";
        return false;
    }

    Finder tl, tr, bl;
    if (!clusterFinders(cands, tl, tr, bl)) {
        errMsg = L"无法识别二维码角点";
        return false;
    }

    const float modEst = (tl.moduleSize + tr.moduleSize + bl.moduleSize) / 3.0f;
    const float dimEst = (dist(tl, tr) + dist(tl, bl)) / (2.0f * modEst) + 7.0f;
    const int dimension = static_cast<int>(std::lround(dimEst));
    if (dimension < 21 || dimension > 177 || (dimension % 4) != 1) {
        errMsg = L"二维码尺寸异常";
        return false;
    }

    std::vector<uint8_t> modules;
    if (!sampleMatrix(img, tl, tr, bl, dimension, modules)) {
        errMsg = L"二维码采样失败";
        return false;
    }

    if (!decodeModules(modules, dimension, outUtf8) || outUtf8.empty()) {
        errMsg = L"二维码解码失败，请尝试更清晰的图片";
        return false;
    }
    return true;
}

}  // namespace

bool QrDecoder::decodeFromFile(const std::wstring& path, std::string& outUtf8,
                               std::wstring& errMsg) {
    if (!ensureGdiPlus()) {
        errMsg = L"GDI+ 初始化失败";
        return false;
    }
    Gdiplus::Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) {
        errMsg = L"无法打开图片文件";
        Logger::warn(L"QrDecoder: 无法打开 " + path);
        return false;
    }
    const bool ok = decodeImage(bmp, outUtf8, errMsg);
    if (ok) Logger::info(L"二维码解码成功");
    return ok;
}

bool QrDecoder::decodeFromHBITMAP(HBITMAP hBmp, std::string& outUtf8, std::wstring& errMsg) {
    if (!hBmp) {
        errMsg = L"无效位图";
        return false;
    }
    if (!ensureGdiPlus()) {
        errMsg = L"GDI+ 初始化失败";
        return false;
    }
    Gdiplus::Bitmap bmp(hBmp, nullptr);
    if (bmp.GetLastStatus() != Gdiplus::Ok) {
        errMsg = L"无法读取位图";
        return false;
    }
    const bool ok = decodeImage(bmp, outUtf8, errMsg);
    if (ok) Logger::info(L"二维码解码成功");
    return ok;
}

}  // namespace clipsync
