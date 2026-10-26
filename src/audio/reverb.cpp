#include "reverb.h"
#include "../game/layout.h"
#include <cmath>
#include <cstring>
#include <mutex>

namespace audio {
using namespace dsp;

namespace {
bool isPrime(int n) {
    if (n < 2) return false;
    for (int d = 2; d * d <= n; ++d)
        if (n % d == 0) return false;
    return true;
}
int nearestPrime(int n) {
    for (int k = 0;; ++k) {
        if (isPrime(n + k)) return n + k;
        if (n - k > 1 && isPrime(n - k)) return n - k;
    }
}
// In-place fast Walsh-Hadamard transform of 16 values, scaled to be orthonormal.
inline void fwht16(float* v) {
    for (int h = 1; h < 16; h <<= 1)
        for (int i = 0; i < 16; i += h << 1)
            for (int j = i; j < i + h; ++j) {
                float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    for (int i = 0; i < 16; ++i) v[i] *= 0.25f;
}

// Line lengths (ms): spread 22-76 ms (mean free path of the hall ~26 ms), made prime per rate.
const float kLineMs[HallReverb::kLines] = {22.1f, 24.9f, 27.3f, 29.9f, 32.6f, 35.2f, 38.1f, 41.3f,
                                           44.6f, 48.0f, 51.9f, 56.1f, 60.4f, 65.3f, 70.7f, 76.4f};
const float kOutSignL[HallReverb::kLines] = {1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, -1, 1, -1};
const float kInSign[HallReverb::kLines] = {1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, 1, -1, 1, -1, -1};
constexpr float kOnset = 0.35f;  // level of the diffuse onset relative to the line sum (structure, not level)

constexpr double kPiD = 3.14159265358979323846;
constexpr float kMaxDelaySec = 0.4f;     // longest early reflection / late pre-delay kept
constexpr float kRebuildMove = 0.05f;    // m: listener or talker displacement that rebuilds the taps
constexpr float kRebuildTurn = 0.99939f; // cos 2 degrees: listener turn
constexpr float kRebuildFace = 0.98481f; // cos 10 degrees: talker turn
constexpr float kDarkBelow = 0.9f;       // hfGain / gain under which a tap goes through the shared low-pass
constexpr float kHfHz = 4000.0f;         // Reflection::hfGain's frequency
constexpr float kMinLateShare = 0.3f;    // the late field never gives up more than 70% of its energy to the taps

// ---- Line absorption fit -------------------------------------------------------------------
// A line of d samples must lose, per pass and in octave band b, ln G_b = -3 ln 10 d / (fs RT60_b)
// (nepers, amplitude). The line's filter is g * lowpass(fcA) * shelf(1 + k below fcB): the shape
// ln|H(w)| is evaluated in closed form at the band centres, g follows from the least squares of the
// RELATIVE error of ln G_b (a relative error of ln G is the same relative error of the RT60), and
// (fcA, fcB, k) are found by Nelder-Mead from twelve fixed starting points over the parameter box.
struct Shape { double a, p, k; };

double shapeLog(const Shape& s, double w) {
    const double cw = std::cos(w), sw = std::sin(w);
    const double lpA = std::log(1.0 - s.a) - 0.5 * std::log(1.0 - 2.0 * s.a * cw + s.a * s.a);
    const double dr = 1.0 - s.p * cw, di = s.p * sw, d2 = dr * dr + di * di;
    const double gr = 1.0 + s.k * (1.0 - s.p) * dr / d2, gi = -s.k * (1.0 - s.p) * di / d2;
    return lpA + 0.5 * std::log(gr * gr + gi * gi);
}

struct Target {
    int n = 0;
    double w[kRoomBands] = {}, lnG[kRoomBands] = {}, weight[kRoomBands] = {};
};

// Cost of a shape (and its best overall log gain, 'c').
double fitCost(const Shape& s, const Target& t, double& c) {
    double num = 0.0, den = 0.0;
    double lab[kRoomBands];
    for (int b = 0; b < t.n; ++b) {
        lab[b] = shapeLog(s, t.w[b]);
        const double inv = t.weight[b] / (t.lnG[b] * t.lnG[b]);
        num += (t.lnG[b] - lab[b]) * inv;
        den += inv;
    }
    c = num / den;
    double cost = 0.0;
    for (int b = 0; b < t.n; ++b) {
        const double r = (c + lab[b] - t.lnG[b]) / t.lnG[b];
        cost += t.weight[b] * r * r;
    }
    return cost;
}

struct LineFit { double a, p, k, c; };

// Shape of the parameters x = (ln fcA, ln fcB, k in dB).
Shape shapeOf(const double x[3], double fs) {
    Shape s;
    s.a = std::exp(-2.0 * kPiD * std::exp(x[0]) / fs);
    s.p = std::exp(-2.0 * kPiD * std::exp(x[1]) / fs);
    s.k = std::pow(10.0, x[2] / 20.0) - 1.0;
    return s;
}

LineFit fitLine(const Target& t, float fsf) {
    const double fs = double(fsf);
    const double lo[3] = {std::log(300.0), std::log(30.0), 0.0};
    const double hi[3] = {std::log(0.49 * fs), std::log(0.45 * fs), 40.0};
    auto cost = [&](const double x[3]) {
        for (int d = 0; d < 3; ++d)
            if (!(x[d] >= lo[d] && x[d] <= hi[d])) return 1e9;
        double c;
        return fitCost(shapeOf(x, fs), t, c);
    };
    // Nelder-Mead from a few fixed starting points spread over the box (deterministic).
    double bestX[3] = {lo[0], lo[1], 0.0}, best = 1e30;
    uint32_t seed = 12345u;
    auto uni = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) * (1.0 / 16777216.0);
    };
    for (int start = 0; start < 12; ++start) {
        double sx[4][3], sv[4];
        for (int d = 0; d < 3; ++d) sx[0][d] = lo[d] + (hi[d] - lo[d]) * (0.1 + 0.8 * uni());
        for (int v = 1; v < 4; ++v)
            for (int d = 0; d < 3; ++d) sx[v][d] = sx[0][d] + (v - 1 == d ? (d == 2 ? 3.0 : 0.5) : 0.0);
        for (int v = 0; v < 4; ++v) sv[v] = cost(sx[v]);
        for (int it = 0; it < 220; ++it) {
            int order[4] = {0, 1, 2, 3};
            std::sort(order, order + 4, [&](int a, int b) { return sv[a] < sv[b]; });
            double nx[4][3], nv[4];
            for (int v = 0; v < 4; ++v) {
                std::copy(sx[order[v]], sx[order[v]] + 3, nx[v]);
                nv[v] = sv[order[v]];
            }
            std::copy(&nx[0][0], &nx[0][0] + 12, &sx[0][0]);
            std::copy(nv, nv + 4, sv);
            double c[3] = {0, 0, 0};
            for (int v = 0; v < 3; ++v)
                for (int d = 0; d < 3; ++d) c[d] += sx[v][d] / 3.0;
            auto point = [&](double tt, double* out) {
                for (int d = 0; d < 3; ++d) out[d] = c[d] + tt * (sx[3][d] - c[d]);
            };
            double r[3], e[3], k[3];
            point(-1.0, r);
            const double fr = cost(r);
            if (fr < sv[0]) {
                point(-2.0, e);
                const double fe = cost(e);
                std::copy(fe < fr ? e : r, (fe < fr ? e : r) + 3, sx[3]);
                sv[3] = std::min(fe, fr);
            } else if (fr < sv[2]) {
                std::copy(r, r + 3, sx[3]);
                sv[3] = fr;
            } else {
                point(fr < sv[3] ? -0.5 : 0.5, k);
                const double fk = cost(k);
                if (fk < std::min(fr, sv[3])) {
                    std::copy(k, k + 3, sx[3]);
                    sv[3] = fk;
                } else {
                    for (int v = 1; v < 4; ++v) {
                        for (int d = 0; d < 3; ++d) sx[v][d] = sx[0][d] + 0.5 * (sx[v][d] - sx[0][d]);
                        sv[v] = cost(sx[v]);
                    }
                }
            }
        }
        for (int v = 0; v < 4; ++v)
            if (sv[v] < best) {
                best = sv[v];
                std::copy(sx[v], sx[v] + 3, bestX);
            }
    }
    const Shape s = shapeOf(bestX, fs);
    double c;
    fitCost(s, t, c);
    // Stability: the loop gain must stay below one at every frequency (the matrix is orthogonal).
    double mx = std::max(shapeLog(s, 0.0), shapeLog(s, kPiD));
    for (int j = 0; j < 96; ++j) mx = std::max(mx, shapeLog(s, kPiD * std::pow(10.0, -3.0 * double(j) / 95.0)));
    return {s.a, s.p, s.k, std::min(c, std::log(0.9998) - mx)};
}

struct LineCoef { float fa, fb, sp, sk; };
struct Design {
    float fs;
    LineCoef c[HallReverb::kLines];
    float kMid;
};
std::mutex gDesignMutex;
std::vector<Design>& designs() {
    static std::vector<Design> d;
    return d;
}

// Block accumulation of a tap set: bright taps read 'bright', dark taps 'dark'. The rings carry a
// guard of kBlock samples after their end that mirrors their start, so a tap's n samples are
// always contiguous. Eight output samples at a time are summed over all taps in registers.
void addTaps(int count, const int* delay, const float* gL, const float* gR, const uint8_t* dark, const float* bright,
             const float* darkBuf, uint32_t mask, uint32_t w0, int n, float scale, float* outL, float* outR) {
    const float* src[kMaxReflections];
    float gl[kMaxReflections], gr[kMaxReflections];
    for (int t = 0; t < count; ++t) {
        src[t] = (dark[t] ? darkBuf : bright) + ((w0 - uint32_t(delay[t])) & mask);
        gl[t] = gL[t] * scale;
        gr[t] = gR[t] * scale;
    }
    int k = 0;
    for (; k + 8 <= n; k += 8) {
        float aL[8] = {}, aR[8] = {};
        for (int t = 0; t < count; ++t) {
            const float* s = src[t] + k;
            for (int j = 0; j < 8; ++j) {
                aL[j] += s[j] * gl[t];
                aR[j] += s[j] * gr[t];
            }
        }
        for (int j = 0; j < 8; ++j) {
            outL[k + j] += aL[j];
            outR[k + j] += aR[j];
        }
    }
    for (; k < n; ++k) {
        float aL = 0.0f, aR = 0.0f;
        for (int t = 0; t < count; ++t) {
            aL += src[t][k] * gl[t];
            aR += src[t][k] * gr[t];
        }
        outL[k] += aL;
        outR[k] += aR;
    }
}
}  // namespace

void HallReverb::prepare(float sampleRate) {
    fs_ = sampleRate;
    inHp_.setCutoff(90.0f, fs_);

    maxDelay_ = std::max(kBlock, int(kMaxDelaySec * fs_));
    for (BankState& bk : bank_) {
        bk = BankState();
        bk.ring.init(maxDelay_ + kBlock + 2);
        bk.ringDark.init(maxDelay_ + kBlock + 2);
        bk.ring.buf.resize(bk.ring.buf.size() + size_t(kBlock), 0.0f);  // guard, see addTaps
        bk.ringDark.buf.resize(bk.ringDark.buf.size() + size_t(kBlock), 0.0f);
        bk.pre.init(maxDelay_ + 2);
        bk.preCur = bk.prePrev = std::max(1, int(std::lround(0.024f * fs_)));  // until the geometry is known
    }
    speechActive_ = false;
    const float apMs[4] = {2.96f, 2.23f, 7.9f, 5.8f};
    const float apG[4] = {0.62f, 0.62f, 0.55f, 0.55f};
    for (int i = 0; i < 4; ++i) ap_[i].init(nearestPrime(int(apMs[i] * 0.001f * fs_)), apG[i]);
    onsetL_.init(nearestPrime(int(0.0113f * fs_)), 0.55f);
    onsetR_.init(nearestPrime(int(0.0137f * fs_)), 0.55f);

    int maxLen = 0;
    for (int i = 0; i < kLines; ++i) {
        len_[i] = nearestPrime(int(kLineMs[i] * 0.001f * fs_));
        maxLen = std::max(maxLen, len_[i]);
    }
    // Slow modulation on four lines: +-0.2..0.4 ms at 0.09..0.6 Hz.
    const int modLines[kModLines] = {3, 7, 10, 14};
    const float modMs[kModLines] = {0.2f, 0.33f, 0.27f, 0.4f}, modHz[kModLines] = {0.6f, 0.23f, 0.41f, 0.09f};
    float maxDepth = 0.0f;
    for (int k = 0; k < kModLines; ++k) {
        modLine_[k] = modLines[k];
        modDepth_[k] = modMs[k] * 0.001f * fs_;
        modInc_[k] = kTau * modHz[k] / fs_;
        maxDepth = std::max(maxDepth, 2.0f * modDepth_[k]);
    }
    lineSize_ = 1;
    while (lineSize_ < uint32_t(maxLen + int(maxDepth) + 4)) lineSize_ <<= 1;
    lineMask_ = lineSize_ - 1;
    // Storage: the ring, then the block scratch, each part on a 64-byte boundary.
    lineStride_ = lineSize_ + uint32_t(kBlock);  // a line's ring and its guard (see tank())
    const size_t ringFloats = size_t(lineStride_) * kLines, rowFloats = size_t(kBlock) * kLines;
    const size_t total = ringFloats + rowFloats + 8 * size_t(kBlock) + 16 * 8;
    mem_.assign(total + 16, 0.0f);
    {
        float* p = mem_.data();
        p += ((64 - (reinterpret_cast<uintptr_t>(p) & 63u)) & 63u) / sizeof(float);
        auto take = [&p](size_t n) {
            float* r = p;
            p += (n + 15) & ~size_t(15);
            return r;
        };
        ring_ = take(ringFloats);
        rows_ = take(rowFloats);
        xd_ = take(size_t(kBlock));
        oL_ = take(size_t(kBlock));
        oR_ = take(size_t(kBlock));
        sl_ = take(size_t(kBlock));
        sr_ = take(size_t(kBlock));
        xin_ = take(size_t(kBlock));
        tmp_ = take(size_t(kBlock));
    }
    // The lines are advanced a run at a time: no line may be read within the run that writes it.
    int minLen = maxLen;
    for (int i = 0; i < kLines; ++i) minLen = std::min(minLen, len_[i]);
    chunk_ = std::clamp(minLen - 2, 1, kBlock);
    lineW_ = 0;
    clear();

    // The absorption fit and the energy gain depend on the rate only: done once per rate.
    std::lock_guard<std::mutex> lock(gDesignMutex);
    std::vector<Design>& all = designs();
    const Design* found = nullptr;
    for (const Design& d : all)
        if (d.fs == fs_) found = &d;
    if (found) {
        for (int i = 0; i < kLines; ++i) {
            fa_[i] = found->c[i].fa;
            fb_[i] = found->c[i].fb;
            sp_[i] = found->c[i].sp;
            sk_[i] = found->c[i].sk;
        }
        kMid_ = found->kMid;
        for (int i = 0; i < kLines; ++i) osp_[i] = 1.0f - sp_[i];
    } else {
        fitAbsorption();
        for (int i = 0; i < kLines; ++i) osp_[i] = 1.0f - sp_[i];
        measureLateGain();
        Design d;
        d.fs = fs_;
        for (int i = 0; i < kLines; ++i) d.c[i] = {fa_[i], fb_[i], sp_[i], sk_[i]};
        d.kMid = kMid_;
        all.push_back(d);
    }
}

void HallReverb::fitAbsorption() {
    const RoomAcoustics& room = hallAcoustics();
    constexpr float kBandWeight[kRoomBands] = {2.0f, 1.0f, 1.0f, 2.0f, 1.0f, 1.0f, 2.0f};
    for (int i = 0; i < kLines; ++i) {
        Target t;
        for (int b = 0; b < kRoomBands; ++b) {
            if (kRoomBandHz[b] >= 0.45f * fs_) continue;
            const double rt = std::max(0.05, double(room.rt60[size_t(b)]));
            t.w[t.n] = 2.0 * kPiD * double(kRoomBandHz[b]) / double(fs_);
            t.lnG[t.n] = -3.0 * std::log(10.0) * double(len_[i]) / (double(fs_) * rt);
            t.weight[t.n] = kBandWeight[b];
            ++t.n;
        }
        const LineFit f = fitLine(t, fs_);
        fa_[i] = float(f.a);
        fb_[i] = float(std::exp(f.c) * (1.0 - f.a));
        sp_[i] = float(f.p);
        sk_[i] = float(f.k);
    }
}

// Energy gain of the late field at 1 kHz: an impulse on the Effects late input, the octave band
// around 1 kHz of both outputs against the same band of the impulse (the room constant R is the
// 1 kHz one, and the band energy is what the late level is set at).
void HallReverb::measureLateGain() {
    const float savedLate = lateGain, savedEarly = earlyGain;
    lateGain = 1.0f;
    earlyGain = 1.0f;
    float rtMax = 0.0f;
    for (float r : hallAcoustics().rt60) rtMax = std::max(rtMax, r);
    const int total = int(1.05f * rtMax * fs_) + kBlock;
    float zero[kBlock] = {}, in[kBlock], L[kBlock], R[kBlock];
    const float* er[kBanks] = {zero, zero};
    const float* late[kBanks] = {in, zero};
    Svf bpL, bpR, bpD;
    bpL.set(1000.0f, 1.41421356f, fs_);
    bpR.set(1000.0f, 1.41421356f, fs_);
    bpD.set(1000.0f, 1.41421356f, fs_);
    double eOut = 0.0, eIn = 0.0;
    for (int done = 0; done < total; done += kBlock) {
        std::fill(in, in + kBlock, 0.0f);
        std::fill(L, L + kBlock, 0.0f);
        std::fill(R, R + kBlock, 0.0f);
        if (done == 0) in[0] = 1.0f;
        processBlock(er, late, L, R, kBlock);
        for (int k = 0; k < kBlock; ++k) {
            const double l = bpL.bpNorm(L[k]), r = bpR.bpNorm(R[k]);
            eOut += l * l + r * r;
        }
    }
    for (int k = 0; k < 8192; ++k) {
        const double d = bpD.bpNorm(k == 0 ? 1.0f : 0.0f);
        eIn += d * d;
    }
    kMid_ = float(eOut / eIn);
    lateGain = savedLate;
    earlyGain = savedEarly;
    clear();
}

void HallReverb::clear() {
    for (BankState& bk : bank_) {
        bk.ring.clear();
        bk.ringDark.clear();
        bk.pre.clear();
        bk.lp.reset();
        bk.quiet = int(bk.ring.mask) + 1;  // the rings are all zero
        bk.lateQuiet = int(bk.pre.mask) + 1;
        bk.fading = false;
    }
    for (auto& a : ap_) a.d.clear();
    onsetL_.d.clear();
    onsetR_.d.clear();
    flushTank();
    chainQuiet_ = 1 << 30;
    for (int k = 0; k < kModLines; ++k) {
        modPhase_[k] = float(k) * 1.7f;
        modCur_[k] = modDepth_[k] * (1.0f + std::sin(modPhase_[k]));
    }
    inHp_.reset();
}

// ---- Geometry ------------------------------------------------------------------------------

void HallReverb::setListener(const Basis& b) {
    basis_ = b;
    maybeRebuild(Effects);
    if (speechActive_) maybeRebuild(Speech);
}

void HallReverb::setSpeechSource(bool active, m::vec3 pos, m::vec3 facing) {
    speechActive_ = active;
    if (!active) return;
    speechPos_ = pos;
    speechFace_ = m::length2(facing) > 0.5f ? m::normalize(facing) : m::vec3(0.0f);
    maybeRebuild(Speech);
}

void HallReverb::maybeRebuild(int b) {
    const BankState& bk = bank_[b];
    const m::vec3 src = b == Effects ? m::vec3(0.0f, layout::BOARD_TOP_Y, 0.0f) : speechPos_;
    const m::vec3 face = b == Effects ? m::vec3(0.0f) : speechFace_;
    bool need = !bk.built || m::length2(basis_.pos - bk.lisPos) > kRebuildMove * kRebuildMove ||
                m::dot(basis_.fwd, bk.lisFwd) < kRebuildTurn || m::dot(basis_.right, bk.lisRight) < kRebuildTurn;
    if (!need && b == Speech) {
        const bool nowFace = m::length2(face) > 0.5f, wasFace = m::length2(bk.srcFace) > 0.5f;
        need = m::length2(src - bk.srcPos) > kRebuildMove * kRebuildMove || nowFace != wasFace ||
               (nowFace && m::dot(face, bk.srcFace) < kRebuildFace);
    }
    if (need) rebuild(b, src, face);
}

void HallReverb::rebuild(int b, m::vec3 src, m::vec3 facing) {
    BankState& bk = bank_[b];
    const ReflectionSet rs = earlyReflections(src, basis_.pos, facing);
    TapSet ns;
    float eWall = 0.0f, firstWall = -1.0f, rhoSum = 0.0f;
    int darkCount = 0;
    for (int i = 0; i < rs.count && ns.n < kMaxTaps; ++i) {
        const Reflection& r = rs.r[size_t(i)];
        if (r.table) {
            if (b == Effects) continue;  // a source on the table: its half-space radiation already has it
        } else {
            eWall += r.gain * r.gain;
            if (r.order == 1 && firstWall < 0.0f) firstWall = r.delay;
        }
        const int d = int(std::lround(r.delay * fs_));
        if (d > maxDelay_ - kBlock) continue;
        // Pan: the equal-power law of computeSpatial, from the arrival direction.
        const float lateral = clampf(m::dot(r.dir, basis_.right), -1.0f, 1.0f);
        const float a = (lateral * 0.72f * 0.5f + 0.5f) * 0.5f * kPi;
        const float rho = r.gain > 0.0f ? r.hfGain / r.gain : 1.0f;
        const uint8_t dk = rho < kDarkBelow ? 1 : 0;
        if (dk) {
            rhoSum += rho;
            ++darkCount;
        }
        // Taps of the same delay and the same filter group are one tap (the symmetric images of a
        // listener on the hall's axis land on the same sample).
        int t = 0;
        while (t < ns.n && !(ns.delay[t] == d && ns.dark[t] == dk)) ++t;
        if (t == ns.n) {
            ++ns.n;
            ns.delay[t] = d;
            ns.dark[t] = dk;
            ns.gL[t] = ns.gR[t] = 0.0f;
            ns.reach = std::max(ns.reach, d);
        }
        ns.gL[t] += r.gain * std::cos(a);
        ns.gR[t] += r.gain * std::sin(a);
    }
    if (darkCount > 0) {
        // One-pole whose gain at 4 kHz is the mean ratio of the dark taps: |H| = 1 / sqrt(1 + (f / fc)^2).
        const float rho = clampf(rhoSum / float(darkCount), 0.05f, 0.999f);
        bk.lp.setCutoff(clampf(kHfHz * rho / std::sqrt(1.0f - rho * rho), 300.0f, 20000.0f), fs_);
    }
    // The late field begins at the first first-order wall reflection.
    const int pre = firstWall < 0.0f ? std::max(1, int(std::lround(0.024f * fs_)))
                                     : std::clamp(int(std::lround(firstWall * fs_)), 1, maxDelay_ - 2);
    if (!bk.built) {
        bk.cur = ns;
        bk.preCur = bk.prePrev = pre;
        bk.fading = false;
    } else {
        if (!bk.fading) {
            bk.prev = bk.cur;
            bk.prePrev = bk.preCur;
        }
        bk.cur = ns;
        bk.preCur = pre;
        bk.fading = true;
    }
    // Energy of the image taps against the voice gain squared: what a send carries is the direct
    // sound at the listener (distance gain, talker directivity towards the listener).
    float dirDirect = 1.0f;
    if (m::length2(facing) > 0.5f) {
        const m::vec3 v = basis_.pos - src;
        const float len = m::length(v);
        if (len > 1e-4f) dirDirect = talkerDirectivity(clampf(m::dot(facing, v / len), -1.0f, 1.0f));
    }
    const float dg = kRefDistance / std::max(rs.directDistance, kMinDistance);
    bk.erEnergy = eWall * dirDirect * dirDirect * dg * dg;
    bk.built = true;
    bk.srcPos = src;
    bk.srcFace = facing;
    bk.lisPos = basis_.pos;
    bk.lisFwd = basis_.fwd;
    bk.lisRight = basis_.right;
}

float HallReverb::lateSend(float q, int bank) const {
    if (!(q > 0.0f)) return 0.0f;
    // Diffuse energy against the direct energy at the reference distance (the voice gain squared):
    // 16 pi r^2 / (q R) at r = kRefDistance, left + right as the direct sound's own energy.
    const float R = hallAcoustics().roomConstant[kRoomMidBand];
    const float diffuse = 16.0f * kPi * kRefDistance * kRefDistance / (q * R);
    const float late = std::max(kMinLateShare * diffuse, diffuse - bank_[bank].erEnergy);
    return std::sqrt(late / kMid_);
}

// ---- Rendering -----------------------------------------------------------------------------

void HallReverb::process(const float* const er[kBanks], const float* const late[kBanks], float* outL, float* outR,
                         int n) {
    const float* e[kBanks] = {er[0], er[1]};
    const float* l[kBanks] = {late[0], late[1]};
    while (n > 0) {
        const int m = std::min(n, kBlock);
        processBlock(e, l, outL, outR, m);
        for (int b = 0; b < kBanks; ++b) {
            e[b] += m;
            l[b] += m;
        }
        outL += m;
        outR += m;
        n -= m;
    }
}

namespace {
// Appends n samples to a delay line (zeros when 'in' is null).
void pushBlock(DelayLine& d, const float* in, int n) {
    const uint32_t size = d.mask + 1u;
    uint32_t idx = d.w & d.mask;
    int k = 0;
    while (k < n) {
        const int seg = std::min(n - k, int(size - idx));
        if (in) std::memcpy(d.buf.data() + idx, in + k, size_t(seg) * sizeof(float));
        else std::memset(d.buf.data() + idx, 0, size_t(seg) * sizeof(float));
        // The guard (rings of the early reflections) mirrors the first kBlock samples.
        if (d.buf.size() > size && int(idx) < HallReverb::kBlock) {
            const int g = std::min(seg, HallReverb::kBlock - int(idx));
            std::memcpy(d.buf.data() + size + idx, d.buf.data() + idx, size_t(g) * sizeof(float));
        }
        k += seg;
        idx = 0;
    }
    d.w += uint32_t(n);
}

// Copies n samples of a delay line, from the absolute position 'start' on.
void readBlock(const DelayLine& d, uint32_t start, int n, float* dst) {
    const uint32_t size = d.mask + 1u;
    uint32_t idx = start & d.mask;
    int k = 0;
    while (k < n) {
        const int seg = std::min(n - k, int(size - idx));
        std::memcpy(dst + k, d.buf.data() + idx, size_t(seg) * sizeof(float));
        k += seg;
        idx = 0;
    }
}

// Any sample not zero (+0 or -0)? The bit patterns are or-ed: that vectorises, a float maximum does not.
bool anyNonZero(const float* x, int n) {
    uint32_t acc = 0;
    for (int k = 0; k < n; ++k) {
        uint32_t u;
        std::memcpy(&u, x + k, sizeof(u));
        acc |= u & 0x7fffffffu;
    }
    return acc != 0;
}

// y = f.lp(x) over n samples, in and out may be the same array. A one-pole low-pass is a scan
// (y[k] = b x[k] + a y[k-1]); four samples at a time it takes two shifted additions and the carry of
// the last block, in place of a dependent chain of three operations per sample.
void onePoleBlock(OnePole& f, const float* x, float* y, int n) {
    int k = 0;
#ifdef AUDIO_HAS_SSE
    const float a = f.a, b = 1.0f - a;
    const __m128 vb = _mm_set1_ps(b), va = _mm_set1_ps(a), va2 = _mm_set1_ps(a * a);
    const __m128 carry = _mm_setr_ps(a, a * a, a * a * a, a * a * a * a);
    float z = f.z;
    for (; k + 4 <= n; k += 4) {
        __m128 t = _mm_mul_ps(vb, _mm_loadu_ps(x + k));
        t = _mm_add_ps(t, _mm_mul_ps(va, _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(t), 4))));
        t = _mm_add_ps(t, _mm_mul_ps(va2, _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(t), 8))));
        t = _mm_add_ps(t, _mm_mul_ps(carry, _mm_set1_ps(z)));
        _mm_storeu_ps(y + k, t);
        z = _mm_cvtss_f32(_mm_shuffle_ps(t, t, 0xFF));
    }
    f.z = z;
#endif
    for (; k < n; ++k) y[k] = f.lp(x[k]);
}

// One Schroeder all-pass over n samples (in and out may be the same array). The delayed value of a
// sample is at least 'len' steps old, so a run of up to len samples has no dependency inside it.
void allpassBlock(Allpass& a, const float* in, float* out, int n) {
    DelayLine& d = a.d;
    const uint32_t size = d.mask + 1u;
    float* buf = d.buf.data();
    const float g = a.g;
    int k = 0;
    while (k < n) {
        const uint32_t ir = (d.w - uint32_t(a.len)) & d.mask, iw = d.w & d.mask;
        const int seg = std::min({n - k, a.len, int(size - ir), int(size - iw)});
        const float* r = buf + ir;
        float* wp = buf + iw;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC ivdep
#endif
        for (int j = 0; j < seg; ++j) {
            const float dl = r[j];
            const float v = in[k + j] + g * dl;
            wp[j] = v;
            out[k + j] = dl - g * v;
        }
        d.w += uint32_t(seg);
        k += seg;
    }
}

constexpr float kMixNorm = 0.25f;       // the orthonormal 16-point Walsh-Hadamard transform's scale
constexpr float kIdleEnergy = 2e-10f;   // mean of L^2 + R^2 under which the tank counts as silent: -100 dB
constexpr float kChainQuietSec = 0.5f;  // input silence after which the diffusers have decayed to nothing (24+ passes of the slowest)
constexpr float kIdleSec = 0.3f;        // time the tank stays below kIdleEnergy before it is switched off

// One step of the 16 lines' network after the delay lines: the absorption of every line, the
// Walsh-Hadamard mix (a 4 x 4 Kronecker product: butterflies across the four vectors, a 4 x 4
// transpose, the same again, the transpose back), the input added (v = H y + in x) and stored to
// 'dst'; sl / sr are the two outputs, the sums of the sign-flipped lines (left: all, right:
// alternating). The coefficient 'fb' carries the matrix's normalisation. The filter states live in
// registers across the steps of a run.
#ifdef AUDIO_HAS_SSE
class RowFilter {
public:
    RowFilter(const float* fb, const float* fa, const float* osp, const float* sp, const float* sk, const float* fz,
              const float* sz) {
        for (int i = 0; i < 16; ++i) {
            fb_[i] = fb[i] * kMixNorm;
            fa_[i] = fa[i];
            osp_[i] = osp[i];
            sp_[i] = sp[i];
            sk_[i] = sk[i];
        }
        for (int q = 0; q < 4; ++q) {
            fz_[q] = _mm_loadu_ps(fz + 4 * q);
            sz_[q] = _mm_loadu_ps(sz + 4 * q);
        }
    }
    void save(float* fz, float* sz) const {
        for (int q = 0; q < 4; ++q) {
            _mm_storeu_ps(fz + 4 * q, fz_[q]);
            _mm_storeu_ps(sz + 4 * q, sz_[q]);
        }
    }
    inline void step(const float* y, float x, float* dst, float& sl, float& sr) {
        __m128 r0 = absorb(0, _mm_loadu_ps(y)), r1 = absorb(1, _mm_loadu_ps(y + 4));
        __m128 r2 = absorb(2, _mm_loadu_ps(y + 8)), r3 = absorb(3, _mm_loadu_ps(y + 12));
        butterflies(r0, r1, r2, r3);
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
        butterflies(r0, r1, r2, r3);
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
        const __m128 vx = _mm_set1_ps(x);
        r0 = _mm_add_ps(r0, _mm_mul_ps(_mm_loadu_ps(kInSign), vx));
        r1 = _mm_add_ps(r1, _mm_mul_ps(_mm_loadu_ps(kInSign + 4), vx));
        r2 = _mm_add_ps(r2, _mm_mul_ps(_mm_loadu_ps(kInSign + 8), vx));
        r3 = _mm_add_ps(r3, _mm_mul_ps(_mm_loadu_ps(kInSign + 12), vx));
        _mm_storeu_ps(dst, r0);
        _mm_storeu_ps(dst + 4, r1);
        _mm_storeu_ps(dst + 8, r2);
        _mm_storeu_ps(dst + 12, r3);
        // Lane b of the column sum holds the lines 4a + b: the parity of the line is the parity of b.
        const __m128 s = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(kOutSignL), r0), _mm_mul_ps(_mm_loadu_ps(kOutSignL + 4), r1)),
                                    _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(kOutSignL + 8), r2), _mm_mul_ps(_mm_loadu_ps(kOutSignL + 12), r3)));
        const __m128 h = _mm_add_ps(s, _mm_movehl_ps(s, s));  // lanes 0, 1: s0 + s2 (even lines), s1 + s3 (odd)
        const float e = _mm_cvtss_f32(h), o = _mm_cvtss_f32(_mm_shuffle_ps(h, h, 1));
        sl = e + o;
        sr = e - o;
    }

private:
    // The absorption of lines 4q .. 4q + 3 for one step: low-pass, shelf, sum.
    inline __m128 absorb(int q, __m128 y) {
        const int o = 4 * q;
        fz_[q] = _mm_add_ps(_mm_mul_ps(_mm_load_ps(fb_ + o), y), _mm_mul_ps(_mm_load_ps(fa_ + o), fz_[q]));
        sz_[q] = _mm_add_ps(_mm_mul_ps(_mm_load_ps(osp_ + o), fz_[q]), _mm_mul_ps(_mm_load_ps(sp_ + o), sz_[q]));
        return _mm_add_ps(fz_[q], _mm_mul_ps(_mm_load_ps(sk_ + o), sz_[q]));
    }
    static inline void butterflies(__m128& r0, __m128& r1, __m128& r2, __m128& r3) {
        const __m128 p0 = _mm_add_ps(r0, r1), p1 = _mm_sub_ps(r0, r1), p2 = _mm_add_ps(r2, r3), p3 = _mm_sub_ps(r2, r3);
        r0 = _mm_add_ps(p0, p2);
        r1 = _mm_add_ps(p1, p3);
        r2 = _mm_sub_ps(p0, p2);
        r3 = _mm_sub_ps(p1, p3);
    }
    alignas(16) float fb_[16], fa_[16], osp_[16], sp_[16], sk_[16];
    __m128 fz_[4], sz_[4];
};
#else
class RowFilter {
public:
    RowFilter(const float* fb, const float* fa, const float* osp, const float* sp, const float* sk, const float* fz,
              const float* sz) {
        for (int i = 0; i < 16; ++i) {
            fb_[i] = fb[i] * kMixNorm;
            fa_[i] = fa[i];
            osp_[i] = osp[i];
            sp_[i] = sp[i];
            sk_[i] = sk[i];
            fz_[i] = fz[i];
            sz_[i] = sz[i];
        }
    }
    void save(float* fz, float* sz) const {
        for (int i = 0; i < 16; ++i) {
            fz[i] = fz_[i];
            sz[i] = sz_[i];
        }
    }
    void step(const float* y, float x, float* dst, float& sl, float& sr) {
        float v[16];
        for (int i = 0; i < 16; ++i) {
            fz_[i] = fb_[i] * y[i] + fa_[i] * fz_[i];
            sz_[i] = osp_[i] * fz_[i] + sp_[i] * sz_[i];
            v[i] = fz_[i] + sk_[i] * sz_[i];
        }
        fwht16(v);
        float e = 0.0f, o = 0.0f;
        for (int i = 0; i < 16; ++i) {
            dst[i] = v[i] / kMixNorm + kInSign[i] * x;
            const float s = kOutSignL[i] * dst[i];
            (i & 1 ? o : e) += s;
        }
        sl = e + o;
        sr = e - o;
    }

private:
    float fb_[16], fa_[16], osp_[16], sp_[16], sk_[16], fz_[16], sz_[16];
};
#endif
}  // namespace

void HallReverb::erBank(BankState& bk, const float* in, float* outL, float* outR, int n) {
    const bool nonZero = anyNonZero(in, n);
    const int size = int(bk.ring.mask) + 1;
    bk.quiet = nonZero ? 0 : std::min(bk.quiet + n, size);
    // A ring that has taken a whole length of silence holds nothing: nothing to read, nothing to push.
    if (!nonZero && bk.quiet >= size) return;
    const uint32_t w0 = bk.ring.w;
    float dk[kBlock];
    if (nonZero || bk.quiet <= 2048) {
        onePoleBlock(bk.lp, in, dk, n);  // the low-pass state runs out into the silence
        pushBlock(bk.ring, in, n);
        pushBlock(bk.ringDark, dk, n);
    } else {
        bk.lp.reset();
        pushBlock(bk.ring, nullptr, n);
        pushBlock(bk.ringDark, nullptr, n);
    }
    if (earlyGain == 0.0f) return;
    // Every tap reads silence once the send has been silent for longer than the longest of them.
    const int reach = bk.fading ? std::max(bk.cur.reach, bk.prev.reach) : bk.cur.reach;
    if (!nonZero && bk.quiet >= reach + n) return;
    const float* bright = bk.ring.buf.data();
    const float* dark = bk.ringDark.buf.data();
    const uint32_t mask = bk.ring.mask;
    if (!bk.fading) {
        addTaps(bk.cur.n, bk.cur.delay, bk.cur.gL, bk.cur.gR, bk.cur.dark, bright, dark, mask, w0, n, earlyGain, outL, outR);
        return;
    }
    // New set in, old set out, linearly over this block.
    float cL[kBlock] = {}, cR[kBlock] = {}, pL[kBlock] = {}, pR[kBlock] = {};
    addTaps(bk.cur.n, bk.cur.delay, bk.cur.gL, bk.cur.gR, bk.cur.dark, bright, dark, mask, w0, n, earlyGain, cL, cR);
    addTaps(bk.prev.n, bk.prev.delay, bk.prev.gL, bk.prev.gR, bk.prev.dark, bright, dark, mask, w0, n, earlyGain, pL, pR);
    const float inv = 1.0f / float(n);
    for (int k = 0; k < n; ++k) {
        const float w = float(k + 1) * inv;
        outL[k] += pL[k] + (cL[k] - pL[k]) * w;
        outR[k] += pR[k] + (cR[k] - pR[k]) * w;
    }
}

// Adds a bank's late send, delayed by its pre-delay (cross-faded from the old delay to the new one
// when the geometry moved), to 'acc'. False when the bank has nothing to contribute.
bool HallReverb::lateInput(BankState& bk, const float* in, int n, float* acc) {
    const bool nonZero = anyNonZero(in, n);
    const int size = int(bk.pre.mask) + 1;
    bk.lateQuiet = nonZero ? 0 : std::min(bk.lateQuiet + n, size);
    if (!nonZero && bk.lateQuiet >= size) return false;  // the ring holds nothing
    const uint32_t w0 = bk.pre.w;
    pushBlock(bk.pre, nonZero ? in : nullptr, n);
    const int reach = bk.fading ? std::max(bk.preCur, bk.prePrev) : bk.preCur;
    if (!nonZero && bk.lateQuiet >= reach + n) return false;
    // Sample k reads what was pushed preCur samples before it.
    readBlock(bk.pre, w0 - uint32_t(bk.preCur), n, tmp_);
    if (!bk.fading) {
        for (int k = 0; k < n; ++k) acc[k] += tmp_[k];
        return true;
    }
    float* old = rows_;  // the delay lines' scratch is free until the tank runs
    readBlock(bk.pre, w0 - uint32_t(bk.prePrev), n, old);
    const float inv = 1.0f / float(n);
    for (int k = 0; k < n; ++k) acc[k] += old[k] + (tmp_[k] - old[k]) * (float(k + 1) * inv);
    return true;
}

void HallReverb::flushTank() {
    if (ring_) std::fill(ring_, ring_ + size_t(lineStride_) * kLines, 0.0f);
    for (int i = 0; i < kLines; ++i) fz_[i] = sz_[i] = 0.0f;
    tankLow_ = 0;
    dormant_ = true;
}

// Rows <-> lines. The block's rows (16 floats per step, the lines being the vector lanes) come from
// and go to the lines' rings (one contiguous run of steps per line): 4 x 4 transposes.
void gatherRows(const float* ring, size_t stride, const uint32_t* off, int m, float* rows) {
    const int m4 = m & ~3;
#ifdef AUDIO_HAS_SSE
    for (int g = 0; g < 4; ++g) {
        const float* p0 = ring + size_t(4 * g) * stride + off[4 * g];
        const float* p1 = ring + size_t(4 * g + 1) * stride + off[4 * g + 1];
        const float* p2 = ring + size_t(4 * g + 2) * stride + off[4 * g + 2];
        const float* p3 = ring + size_t(4 * g + 3) * stride + off[4 * g + 3];
        for (int k = 0; k < m4; k += 4) {
            __m128 a = _mm_loadu_ps(p0 + k), b = _mm_loadu_ps(p1 + k), c = _mm_loadu_ps(p2 + k), d = _mm_loadu_ps(p3 + k);
            _MM_TRANSPOSE4_PS(a, b, c, d);
            float* r = rows + size_t(k) * 16 + 4 * g;
            _mm_storeu_ps(r, a);
            _mm_storeu_ps(r + 16, b);
            _mm_storeu_ps(r + 32, c);
            _mm_storeu_ps(r + 48, d);
        }
    }
#else
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < m4; ++k) rows[size_t(k) * 16 + i] = ring[size_t(i) * stride + off[i] + k];
#endif
    for (int k = m4; k < m; ++k)
        for (int i = 0; i < 16; ++i) rows[size_t(k) * 16 + i] = ring[size_t(i) * stride + off[i] + k];
}

void scatterRows(float* ring, size_t stride, uint32_t widx, uint32_t guard, int m, const float* rows) {
    const int m4 = m & ~3;
#ifdef AUDIO_HAS_SSE
    for (int g = 0; g < 4; ++g)
        for (int k = 0; k < m4; k += 4) {
            const float* r = rows + size_t(k) * 16 + 4 * g;
            __m128 a = _mm_loadu_ps(r), b = _mm_loadu_ps(r + 16), c = _mm_loadu_ps(r + 32), d = _mm_loadu_ps(r + 48);
            _MM_TRANSPOSE4_PS(a, b, c, d);
            float* q = ring + size_t(4 * g) * stride + widx + k;
            _mm_storeu_ps(q, a);
            _mm_storeu_ps(q + stride, b);
            _mm_storeu_ps(q + 2 * stride, c);
            _mm_storeu_ps(q + 3 * stride, d);
        }
#else
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < m4; ++k) ring[size_t(i) * stride + widx + k] = rows[size_t(k) * 16 + i];
#endif
    for (int k = m4; k < m; ++k)
        for (int i = 0; i < 16; ++i) ring[size_t(i) * stride + widx + k] = rows[size_t(k) * 16 + i];
    // The guard after a ring's end repeats its first samples (reads of a run never wrap).
    if (widx < guard) {
        const size_t len = std::min<size_t>(size_t(m), guard - widx);
        const size_t size = stride - guard;
        for (int i = 0; i < 16; ++i)
            std::memcpy(ring + size_t(i) * stride + size + widx, ring + size_t(i) * stride + widx, len * sizeof(float));
    }
}

// Advances the 16 lines by n steps with the diffused input xd_: the sums of the lines (left,
// right) go to sl_ / sr_. Within a run of at most chunk_ steps no line reads what the run writes, so
// a run takes three passes: gather the delayed values of all lines (rows, the lines being the lanes of
// a vector), absorb and mix a row at a time, scatter the new rows back. A line's ring is
// contiguous in time and has a guard of kBlock samples after its end that repeats its start, so a
// run is one contiguous read (and write, runs end at the wrap) per line, and the 16 streams stay
// in the L1 cache.
void HallReverb::tank(int n, const float* modStep) {
    const uint32_t mask = lineMask_, size = lineSize_;
    const size_t stride = lineStride_;
    RowFilter filter(fb_, fa_, osp_, sp_, sk_, fz_, sz_);
    for (int k0 = 0; k0 < n;) {
        const uint32_t w = lineW_;
        const uint32_t widx = w & mask;
        const int m = std::min({chunk_, n - k0, int(size - widx)});
        // Pass 1.
        uint32_t off[kLines];
        for (int i = 0; i < kLines; ++i) off[i] = (w - uint32_t(len_[i])) & mask;
        gatherRows(ring_, stride, off, m, rows_);
        // The modulated lines read between two samples instead.
        for (int j = 0; j < kModLines; ++j) {
            const int i = modLine_[j];
            const float* line = ring_ + size_t(i) * stride;
            float mc = modCur_[j];
            for (int k = 0; k < m; ++k) {
                mc += modStep[j];
                const float d = float(len_[i]) + mc;
                const int di = int(d);
                const float fr = d - float(di);
                const uint32_t t = w + uint32_t(k) - uint32_t(di);
                const float a = line[t & mask], b = line[(t - 1u) & mask];
                rows_[size_t(k) * kLines + i] = a + (b - a) * fr;
            }
            modCur_[j] = mc;
        }
        // Pass 2: absorption, mix, input, output sums (in place).
        for (int k = 0; k < m; ++k) {
            float* row = rows_ + size_t(k) * kLines;
            filter.step(row, xd_[k0 + k], row, sl_[k0 + k], sr_[k0 + k]);
        }
        // Pass 3.
        scatterRows(ring_, stride, widx, uint32_t(kBlock), m, rows_);
        lineW_ = w + uint32_t(m);
        k0 += m;
    }
    filter.save(fz_, sz_);
}

void HallReverb::processBlock(const float* const er[kBanks], const float* const late[kBanks], float* outL, float* outR,
                              int n) {
    for (int b = 0; b < kBanks; ++b) erBank(bank_[b], er[b], outL, outR, n);

    // Modulation targets for the end of this block (linear ramp per sample).
    float modStep[kModLines];
    for (int j = 0; j < kModLines; ++j) {
        modPhase_[j] += modInc_[j] * float(n);
        if (modPhase_[j] > kTau) modPhase_[j] -= kTau;
        float target = modDepth_[j] * (1.0f + std::sin(modPhase_[j]));
        modStep[j] = (target - modCur_[j]) / float(n);
    }

    // The late input of the two banks, pre-delayed.
    std::fill(xin_, xin_ + n, 0.0f);
    bool fed = false;
    for (int b = 0; b < kBanks; ++b) fed |= lateInput(bank_[b], late[b], n, xin_);
    for (BankState& bk : bank_) bk.fading = false;
    const bool live = fed && anyNonZero(xin_, n);
    chainQuiet_ = live ? 0 : std::min(chainQuiet_ + n, 1 << 30);
    const bool chainOn = chainQuiet_ < int(kChainQuietSec * fs_);
    if (!chainOn && dormant_) return;  // nothing in the network, nothing coming in
    dormant_ = false;

    // Input diffusion: sub-bass out, four Schroeder all-passes, a decorrelated onset per ear.
    if (chainOn) {
        onePoleBlock(inHp_, xin_, tmp_, n);
        for (int k = 0; k < n; ++k) xd_[k] = xin_[k] - tmp_[k];  // keeps the sub-bass rumble out of the network (not acoustics)
        for (auto& a : ap_) allpassBlock(a, xd_, xd_, n);
        allpassBlock(onsetL_, xd_, oL_, n);
        allpassBlock(onsetR_, xd_, oR_, n);
        for (int k = 0; k < n; ++k) {
            oL_[k] *= kOnset;
            oR_[k] *= kOnset;
            xd_[k] *= 0.25f;
        }
    } else {
        std::fill(xd_, xd_ + n, 0.0f);
        std::fill(oL_, oL_ + n, 0.0f);
        std::fill(oR_, oR_ + n, 0.0f);
    }

    tank(n, modStep);

    const float lg = lateGain;
    for (int k = 0; k < n; ++k) {
        outL[k] += (sl_[k] + oL_[k]) * lg;
        outR[k] += (sr_[k] + oR_[k]) * lg;
    }
    if (chainOn) {
        tankLow_ = 0;
        return;
    }
    // No input any more: once the tank has decayed below the idle level it is flushed and switched off.
    float energy = 0.0f;
    for (int k = 0; k < n; ++k) energy += sl_[k] * sl_[k] + sr_[k] * sr_[k];
    tankLow_ = energy < kIdleEnergy * float(n) ? tankLow_ + n : 0;
    if (tankLow_ >= int(kIdleSec * fs_)) flushTank();
}

}  // namespace audio
