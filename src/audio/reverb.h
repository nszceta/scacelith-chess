// Stone hall reverb, derived from the hall's geometry and materials (room.h): nothing in it is tuned
// by ear.
//
//  * Early reflections: the image sources of earlyReflections() for the current listener, as taps on
//    a mono send, panned from their arrival direction in the listener's basis (the equal-power law of
//    computeSpatial). Two banks, each with its own send and tap set: Effects (the table's sounds, a
//    source at the board centre; the table-top image is left out, a source lying on the table
//    already radiates into the half space the send level accounts for) and Speech (the current
//    talker's mouth and facing, the table-top bounce included). A bank's taps are rebuilt, at block
//    rate, when the listener moves or turns or the talker moves or turns; the old and the new set
//    cross-fade over one block (no clicks). The taps are split into two sub-banks by their
//    4 kHz / 1 kHz gain ratio: flat, and through one shared one-pole low-pass (no per-tap filters).
//    A send carries the direct sound as heard at the listener (gain, distance, talker directivity), so
//    a tap's gain is the Reflection's own gain.
//  * Late field: a 16-line feedback delay network (Hadamard matrix, 22-76 ms lines, slow delay
//    modulation against metallic ringing, input diffusion and a decorrelated diffuse onset). Each
//    line's absorption (a one-pole low-pass and a one-pole low shelf, fitted per line in prepare())
//    gives the hall's reverberation time in every octave band 125 Hz .. 8 kHz (room.h rt60). The
//    field starts at the delay of the first first-order wall reflection (no diffuse energy before
//    it), per bank; the delay follows the geometry with the same cross-fade.
//  * Cost: the network runs in runs of up to 256 steps (no line is read within the run that writes
//    it, its shortest line being 22 ms): a run gathers the delayed values of all lines (one
//    contiguous read per line ring), then absorbs and mixes a step at a time with the 16 lines as
//    vector lanes (4 x 4 butterflies and transposes), then scatters the new steps back. The early
//    reflections of a bank are summed over all taps eight samples at a time, and not at all while
//    its send has been silent for longer than its longest tap. The input diffusion is block-wise
//    (one-pole scan, all-passes in runs). Silence costs next to nothing: a late input silent for
//    longer than the diffusers need to decay (0.5 s) switches them off, and the network itself is
//    flushed and skipped once its output has stayed below -100 dB for 0.3 s, until an input returns.
//  * Level: lateSend(q, bank) is the send amplitude (relative to the voice's gain, which is the
//    direct sound at 0.5 m) that makes the late field's energy what the room constant gives for a
//    source of directivity factor q, less the energy of the early reflections (not counted twice).
//    The reverb's own energy gain at 1 kHz is measured once per sample rate in prepare().
// Mono sends in, stereo out. Allocation only in prepare().
#pragma once
#include "dsp.h"
#include "room.h"
#include "spatial.h"

namespace audio {

class HallReverb {
public:
    static constexpr int kLines = 16;
    static constexpr int kBanks = 2;
    static constexpr int kBlock = 256;
    enum Bank { Effects = 0, Speech = 1 };

    void prepare(float sampleRate);
    void clear();

    // Block-rate geometry. The listener's basis; the talker of the Speech bank (inactive: its taps
    // keep their last set, nothing is sent to them). Both rebuild a bank's taps when it has drifted.
    void setListener(const Basis& b);
    void setSpeechSource(bool active, m::vec3 pos, m::vec3 facing);

    // Late-field send amplitude, relative to the voice gain, for a source of directivity factor q
    // (0: none) feeding 'bank'.
    float lateSend(float q, int bank) const;
    // Energy gain (left + right) at 1 kHz of the late field for an impulse on a late input.
    float lateEnergyGain() const { return kMid_; }
    // Seconds after the direct sound at which a bank's late field begins.
    float lateDelay(int bank) const { return float(bank_[bank].preCur) / fs_; }
    // The bank's early reflection taps (tests): delays in seconds and linear gains per ear.
    int tapCount(int bank) const { return bank_[bank].cur.n; }
    float tapDelay(int bank, int i) const { return float(bank_[bank].cur.delay[i]) / fs_; }

    // er[b]: early-reflection send of bank b; late[b]: late-field send of bank b (n samples, any n).
    // Adds the wet stereo signal into outL / outR.
    void process(const float* const er[kBanks], const float* const late[kBanks], float* outL, float* outR, int n);

    float earlyGain = 1.0f, lateGain = 1.0f;

private:
    static constexpr int kMaxTaps = kMaxReflections;
    struct TapSet {
        int n = 0;
        int reach = 0;  // the longest tap delay (samples)
        int delay[kMaxTaps] = {};
        float gL[kMaxTaps] = {}, gR[kMaxTaps] = {};
        uint8_t dark[kMaxTaps] = {};  // 1: read through the bank's shared low-pass
    };
    struct BankState {
        dsp::DelayLine ring, ringDark, pre;
        dsp::OnePole lp;
        TapSet cur, prev;
        bool fading = false;   // prev -> cur (and prePrev -> preCur) cross-fade in the next block
        int preCur = 1, prePrev = 1;
        int quiet = 0;         // samples since the early send was last non-zero
        int lateQuiet = 0;     // samples since the late send was last non-zero
        // Geometry the taps were built for.
        bool built = false;
        m::vec3 srcPos, srcFace, lisPos, lisFwd, lisRight;
        float erEnergy = 0.0f;  // energy of the hall's image taps relative to the voice gain squared
    };

    void rebuild(int b, m::vec3 src, m::vec3 facing);
    void maybeRebuild(int b);
    void erBank(BankState& bk, const float* in, float* outL, float* outR, int n);
    bool lateInput(BankState& bk, const float* in, int n, float* acc);
    void processBlock(const float* const er[kBanks], const float* const late[kBanks], float* outL, float* outR, int n);
    void tank(int n, const float* modStep);
    void flushTank();
    void fitAbsorption();
    void measureLateGain();

    float fs_ = 48000.0f;
    int maxDelay_ = 1;  // samples: longest early reflection / pre-delay
    BankState bank_[kBanks];
    Basis basis_;
    bool speechActive_ = false;
    m::vec3 speechPos_, speechFace_;
    dsp::OnePole inHp_;
    dsp::Allpass ap_[4];
    dsp::Allpass onsetL_, onsetR_;
    // The 16 lines of the feedback network, one ring per line (line i is read len_[i] steps back).
    // 'mem_' holds the rings and the block scratch, 64-byte aligned by hand (over-aligned members are
    // unreliable on the Windows stack).
    std::vector<float> mem_;
    float* ring_ = nullptr;      // kLines rings of lineSize_ samples (and a guard), lineStride_ floats apart
    float* rows_ = nullptr;      // kBlock rows: the lines' delayed outputs of a block
    float *xd_ = nullptr, *oL_ = nullptr, *oR_ = nullptr, *sl_ = nullptr, *sr_ = nullptr;  // kBlock floats each: diffused input, onset L / R, sums L / R
    float *xin_ = nullptr, *tmp_ = nullptr;  // kBlock floats each: late input of the tank, a delayed send
    uint32_t lineSize_ = 0, lineMask_ = 0, lineW_ = 0, lineStride_ = 0;
    int len_[kLines] = {};
    int chunk_ = kBlock;  // longest run of steps the lines can be advanced at once (shortest line)
    // Line absorption: fz = fb x + fa fz (low-pass), then y = fz + sk * sz, sz = (1 - sp) fz + sp sz
    // (low shelf: gain 1 + sk below the corner, 1 above).
    float fa_[kLines] = {}, fb_[kLines] = {}, fz_[kLines] = {};
    float sp_[kLines] = {}, osp_[kLines] = {}, sk_[kLines] = {}, sz_[kLines] = {};
    // Modulated lines: delay = len + depth * (1 + sin(phase)), updated per block, ramped per sample.
    static constexpr int kModLines = 4;
    int modLine_[kModLines] = {};
    float modDepth_[kModLines] = {}, modPhase_[kModLines] = {}, modInc_[kModLines] = {}, modCur_[kModLines] = {};
    float kMid_ = 1.0f;
    // Idle handling. The input chain (high-pass, diffusers) is skipped once its input has been zero
    // long enough for it to have decayed to nothing; the tank is then flushed and skipped once its
    // output has stayed below -100 dB, until an input returns.
    int chainQuiet_ = 0;   // samples since the diffusion chain's input was last non-zero
    int tankLow_ = 0;      // samples the tank has been below the idle level with no input
    bool dormant_ = true;  // the lines and filter states are zero and not advanced
    // The block scratch lives in mem_ (allocated in prepare()).
};

}  // namespace audio
