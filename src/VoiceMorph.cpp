// Voice Morph - an MPC insert effect plugin by Germanikkus
//
// SPDX-License-Identifier: MIT
//
// A voice pitch effect in eight styles: Natural and Hard Tune correction,
// Robot monotone, Chipmunk and Monster octave voices, Harmony, Warble and
// Underwater. Put it on a track with a vocal (mic input or a sample).
//
// Signal path:
//   in (L+R summed to mono) -> input history (ring buffer)
//      |
//      +-> lowpass + decimate to ~22 kHz -> YIN pitch detector (every hop of
//      |   ~5.8 ms) -> "how many semitones to move" for the chosen Style,
//      |   Key and Scale
//      |
//      +-> lead shifter (pitch-synchronous overlap-add, "PSOLA"): two-period
//          Hann grains are cut from the input at the detected period and laid
//          down at the corrected period. Grain playback speed sets Formant.
//          A second shifter adds the Harmony voice (Harmony style) or a
//          detuned chorus voice (Underwater style).
//      -> Underwater only: swept resonant lowpass
//   out = delayed dry * (1 - Mix) + wet * Mix
//      -> Width (adds a delayed copy to one side and subtracts it from the other)
//      -> Stutter (tempo-synced repeats of 1/32 .. 1/128 slices)
//      -> Echo (tempo-synced filtered ping-pong)
//      -> EQ (stereo): Low Cut, Lows shelf, Highs shelf, High Cut -> times Output
//
// The plugin delays the sound by one longest voice period (~15 ms) so every
// grain it needs has already arrived. It reports that latency to the host.
//
// Parameters:
//   Output   final level (parameter 0, harmless when the MPC nudges it)
//   Style    Natural / Hard Tune / Robot / Chipmunk / Monster / Harmony / Warble / Underwater
//   Key      root note of the scale (and the Robot note)
//   Scale    which notes the voice is pulled to
//   Formant  voice size: negative = bigger/darker, positive = smaller/brighter
//   Character one knob whose job follows the Style: tuning speed (Natural,
//            Hard Tune), note (Robot), extra size (Chipmunk, Monster), harmony
//            interval, vibrato depth (Warble), depth (Underwater)
//   Speed, Harmony, Wobble, Murk: replaced by Character in 1.2; kept only so
//            saved projects keep their parameter numbering
//   Sens     pitch detector sensitivity (raise if notes drop out, lower if
//            breaths and noise get "tuned")
//   Mix      dry/wet
//   Low Cut  12 dB/oct highpass on the final sound (20 Hz = off)
//   Lows     low shelf at 200 Hz, -12..+12 dB
//   Highs    high shelf at 4 kHz, -12..+12 dB
//   High Cut 12 dB/oct lowpass on the final sound (20 kHz = off)
//   Echo     echo amount (level and repeats together), 0 = off
//   Echo Time note length of the echo, synced to the MPC's tempo
//   Stutter  chance that each 1/16 step turns into a repeat
//   Width    stereo spread of the voice (mono-compatible)

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/biquad.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <initializer_list>
#include <vector>

using namespace mpc::dsp;

namespace {

enum ParamIndex
{
    kOutput = 0,
    kStyle,
    kKey,
    kScale,
    kSpeed,
    kFormant,
    kHarmony,
    kWobble,
    kMurk,
    kSens,
    kMix,
    kLowCut,
    kLows,
    kHighs,
    kHighCut,
    kCharacter,
    kEcho,
    kEchoTime,
    kStutter,
    kWidth,
    kNumParams
};

enum Style { kNatural = 0, kHardTune, kRobot, kChipmunk, kMonster, kHarmonyStyle, kWarble, kUnderwater };

constexpr float kMinHz = 70.0f;     // lowest voice the detector looks for
constexpr float kMaxHz = 1000.0f;   // highest
constexpr int   kHannSize = 2048;
constexpr float kLowShelfHz = 200.0f;
constexpr float kHighShelfHz = 4000.0f;
constexpr float kMaxEchoSec = 3.2f;   // 1/2 note at 40 BPM is 3 s

// Echo Time choices (see the Echo Time parameter), in quarter notes.
const float kEchoTimeBeats[]       = {0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 2.0f / 3.0f, 1.0f, 1.5f, 2.0f};

// Character knob, Harmony style: intervals from lowest to highest, as indices
// into the Harmony tables in analyse().
const char* const kHarmNames[] = {"3rd Up", "5th Up", "Oct Up", "3rd Down", "Oct Down"};
const int kHarmOrder[] = {4, 3, 0, 1, 2};   // Oct Down, 3rd Down, 3rd Up, 5th Up, Oct Up

// Character knob, Robot style: which note the robot sings.
const char* const kRobotNames[] = {"5th Down", "Root", "5th Up"};
const int kRobotOffsets[] = {-7, 0, 7};

// Notes in each scale as a 12-bit mask, bit i = i semitones above the key.
constexpr int scaleMask(std::initializer_list<int> notes)
{
    int m = 0;
    for (int n : notes) m |= 1 << n;
    return m;
}
const int kScaleMasks[] = {
    0xFFF,                                  // Chromatic
    scaleMask({0, 2, 4, 5, 7, 9, 11}),      // Major
    scaleMask({0, 2, 3, 5, 7, 8, 10}),      // Minor
    scaleMask({0, 2, 4, 7, 9}),             // Pentatonic major
    scaleMask({0, 3, 5, 7, 10}),            // Pentatonic minor
    scaleMask({0, 3, 5, 6, 7, 10}),         // Blues
};

inline bool inScale(int note, int key, int mask)
{
    int deg = (note - key) % 12;
    if (deg < 0) deg += 12;
    return (mask >> deg) & 1;
}

// Nearest scale note to the sung pitch `m` (MIDI, fractional). Sticks with the
// previous note unless another is clearly closer, so a voice hovering between
// two notes does not flicker.
int quantize(float m, int key, int mask, int prev)
{
    const int base = (int)std::floor(m);
    int best = base;
    float bestDist = 1.0e9f;
    for (int n = base - 7; n <= base + 7; ++n)
    {
        if (!inScale(n, key, mask)) continue;
        const float d = std::fabs((float)n - m);
        if (d < bestDist) { bestDist = d; best = n; }
    }
    if (prev >= 0 && inScale(prev, key, mask) && std::fabs((float)prev - m) < bestDist + 0.2f) return prev;
    return best;
}

// Move `steps` notes up or down inside the scale.
int stepScale(int note, int steps, int key, int mask)
{
    while (steps > 0) { do { ++note; } while (!inScale(note, key, mask)); --steps; }
    while (steps < 0) { do { --note; } while (!inScale(note, key, mask)); ++steps; }
    return note;
}

// Hann window lookup, built once (thread-safe static init) and shared by all instances.
struct HannTable
{
    float v[kHannSize + 1];
    HannTable() { for (int i = 0; i <= kHannSize; ++i) v[i] = 0.5f - 0.5f * std::cos(kTwoPi * (float)i / (float)kHannSize); }
};
const HannTable& hann() { static const HannTable t; return t; }

// ---------------------------------------------------------------------------
// Pitch-synchronous grain shifter. Reads a shared mono history buffer.
// ---------------------------------------------------------------------------
class Shifter
{
public:
    void reset()
    {
        for (Grain& g : grains_) g.on = false;
        inputMark_ = -1.0e18;
        nextSpawn_ = -1.0e18;
    }

    // n = number of samples written to the history so far (newest is n - 1).
    // period = current input period in samples, ratio = pitch factor,
    // formant = grain playback speed, latency = how far behind n we read.
    float process(const float* hist, int64_t mask, int64_t n, int latency, float period,
                  float ratio, float formant)
    {
        const double now = (double)n;
        if (now >= nextSpawn_)
        {
            // Grain centres step through the input in whole periods, trailing `latency`
            // behind the newest sample, so neighbouring grains line up in phase.
            const double anchor = now - (double)latency;
            if (inputMark_ > anchor || inputMark_ < anchor - 4.0 * period) inputMark_ = anchor;
            while (inputMark_ + period <= anchor) inputMark_ += period;
            spawn(inputMark_ - period, formant, 2.0f * period / formant);
            const double step = (double)(period / ratio);
            nextSpawn_ = (nextSpawn_ < now - step) ? now + step : nextSpawn_ + step;
        }

        const float* win = hann().v;
        float sum = 0.0f, wsum = 0.0f;
        for (Grain& g : grains_)
        {
            if (!g.on) continue;
            const float ph = g.age * g.invLen * (float)kHannSize;
            const int   hi = (int)ph;
            const float w  = win[hi] + (ph - (float)hi) * (win[hi + 1] - win[hi]);
            const double fl = std::floor(g.pos);
            const int64_t i0 = (int64_t)fl;
            const float fr = (float)(g.pos - fl);
            const float a = hist[i0 & mask], b = hist[(i0 + 1) & mask];
            sum += w * (a + fr * (b - a));
            wsum += w;
            g.pos += g.inc;
            g.age += 1.0f;
            if (g.age >= g.len) g.on = false;
        }
        // Where grains pile up (pitch raised), divide out the extra window weight.
        return wsum > 1.0f ? sum / wsum : sum;
    }

private:
    struct Grain { double pos = 0.0; float inc = 1.0f, len = 1.0f, invLen = 1.0f, age = 0.0f; bool on = false; };
    static constexpr int kMaxGrains = 12;

    void spawn(double start, float inc, float len)
    {
        for (Grain& g : grains_)
        {
            if (g.on) continue;
            g.pos = start; g.inc = inc; g.len = len; g.invLen = 1.0f / len; g.age = 0.0f; g.on = true;
            return;
        }
    }

    Grain grains_[kMaxGrains];
    double inputMark_ = -1.0e18, nextSpawn_ = -1.0e18;
};

} // namespace

class VoiceMorph : public mpc::Plugin
{
public:
    VoiceMorph() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("out",     "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(choiceParam("style",   "Style", {"Natural", "Hard Tune", "Robot", "Chipmunk", "Monster",
                                                  "Harmony", "Warble", "Underwater"}, kHardTune));
        addParam(choiceParam("key",     "Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
        addParam(choiceParam("scale",   "Scale", {"Chromatic", "Major", "Minor", "Pent Maj", "Pent Min", "Blues"}, 0));
        addParam(floatParam ("speed",   "Old Speed", 0.0f, 300.0f, 60.0f, "ms", 0, 0.5f));    // unused since 1.2
        addParam(floatParam ("formant", "Formant", -6.0f, 6.0f, 0.0f, "st", 1));
        addParam(choiceParam("harm",    "Old Harmony", {"3rd Up", "5th Up", "Oct Up", "3rd Down", "Oct Down"}, 0));   // unused
        addParam(floatParam ("wobble",  "Old Wobble", 0.0f, 100.0f, 50.0f, "%", 0));   // unused
        addParam(floatParam ("murk",    "Old Murk", 0.0f, 100.0f, 50.0f, "%", 0));     // unused
        addParam(floatParam ("sens",    "Sens", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("mix",     "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(logParam   ("lowcut",  "Low Cut", 20.0f, 1000.0f, 20.0f, "Hz", 0));
        addParam(floatParam ("lows",    "Lows", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(floatParam ("highs",   "Highs", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(logParam   ("highcut", "High Cut", 2000.0f, 20000.0f, 20000.0f, "Hz", 0));
        addParam(floatParam ("char",    "Character", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("echo",    "Echo", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("etime",   "Echo Time", {"1/16", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2"}, 5));
        addParam(floatParam ("stutter", "Stutter", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("width",   "Width", 0.0f, 100.0f, 0.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("VoiceMorph: parameter count mismatch");

        //                       Out Style          Key Scale Spd Frm Hrm Wob Mrk Sens Mix  LoCut Lows Highs HiCut  Char Echo ETime Stut Width
        addProgram("Hard Tune",  {0, kHardTune,     0,  1,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 0,   0,   5,    0,   0});
        addProgram("Natural",    {0, kNatural,      0,  0,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 58,  0,   5,    0,   0});
        addProgram("Robot",      {0, kRobot,        0,  0,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 50,  0,   5,    0,   0});
        addProgram("Chipmunk",   {0, kChipmunk,     0,  0,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 50,  0,   5,    0,   0});
        addProgram("Monster",    {0, kMonster,      0,  0,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 50,  0,   5,    0,   0});
        addProgram("Harmony",    {0, kHarmonyStyle, 0,  1,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 50,  0,   5,    0,   35});
        addProgram("Warble",     {0, kWarble,       0,  0,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 60,  0,   5,    0,   0});
        addProgram("Underwater", {0, kUnderwater,   0,  0,    60, 0,  0,  50, 50, 50,  100, 20,   0,   0,    20000, 60,  25,  6,    0,   30});
        addProgram("Vocal Throw",{0, kHardTune,     0,  1,    60, 0,  0,  50, 50, 50,  100, 120,  0,   0,    20000, 0,   40,  6,    0,   20});
        addProgram("Glitch Bot", {0, kRobot,        0,  0,    60, 0,  0,  50, 50, 50,  100, 80,   0,   0,    20000, 50,  20,  3,    35,  0});
        hann();
    }

    // The Character knob's value text names what it does in the current Style.
    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index != kCharacter) return false;
        const float c = plain * 0.01f;
        char buf[48];
        switch (getInt(kStyle))
        {
            case kNatural:  std::snprintf(buf, sizeof buf, "Speed %d ms", (int)std::lround(naturalSpeedMs(c))); break;
            case kHardTune:
            {
                const float ms = hardTuneSpeedMs(c);
                if (ms <= 0.0f) std::snprintf(buf, sizeof buf, "Instant");
                else std::snprintf(buf, sizeof buf, "Speed %d ms", (int)std::lround(ms));
                break;
            }
            case kRobot:    std::snprintf(buf, sizeof buf, "%s", kRobotNames[robotChoice(c)]); break;
            case kChipmunk:
            case kMonster:  std::snprintf(buf, sizeof buf, "Size %+.1f st", extraSize(c)); break;
            case kHarmonyStyle: std::snprintf(buf, sizeof buf, "%s", kHarmNames[harmChoice(c)]); break;
            case kWarble:   std::snprintf(buf, sizeof buf, "Wobble %d%%", (int)std::lround(plain)); break;
            default:        std::snprintf(buf, sizeof buf, "Murk %d%%", (int)std::lround(plain)); break;
        }
        out = buf;
        return true;
    }

    void parameterChanged(int index, float /*plainValue*/) override
    {
        if (index == kStyle) requestDisplayUpdate();   // Character's value text changes meaning
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        decim_ = clamp((int)std::lround(sampleRate / 22050.0), 1, 4);
        srd_ = sr_ / (float)decim_;
        hop_ = 128 * decim_;
        maxLag_ = (int)std::ceil(srd_ / kMinHz) + 2;
        minLag_ = std::max(2, (int)std::floor(srd_ / kMaxHz));
        maxPeriod_ = sr_ / kMinHz;
        unvoicedPeriod_ = 0.008f * sr_;
        latency_ = (int)std::ceil(maxPeriod_) + 4;

        histSize_ = pow2AtLeast(latency_ + (int)(8.0f * maxPeriod_) + 64);
        hist_.assign((size_t)histSize_, 0.0f);
        drySize_ = pow2AtLeast(latency_ + 4);
        dryL_.assign((size_t)drySize_, 0.0f);
        dryR_.assign((size_t)drySize_, 0.0f);
        decSize_ = pow2AtLeast(4 * maxLag_);
        dec_.assign((size_t)decSize_, 0.0f);
        frame_.assign((size_t)(2 * maxLag_), 0.0f);
        echoSize_ = pow2AtLeast((int)(kMaxEchoSec * sr_) + 8);
        echoL_.assign((size_t)echoSize_, 0.0f);
        echoR_.assign((size_t)echoSize_, 0.0f);
        stutSize_ = pow2AtLeast((int)(1.2f * sr_));   // one 1/16 step at 20 BPM is 0.75 s
        stutL_.assign((size_t)stutSize_, 0.0f);
        stutR_.assign((size_t)stutSize_, 0.0f);
        widthSize_ = pow2AtLeast((int)(0.02f * sr_));
        widthBuf_.assign((size_t)widthSize_, 0.0f);
        widthDelay_ = (int)(0.012f * sr_);
        stutFade_ = std::max(8, (int)(0.0025f * sr_));
        diff_.assign((size_t)(maxLag_ + 1), 0.0f);

        decimLp_.setCoeffs(BiquadCoeffs::lowpass(sampleRate, 1200.0f, 0.7071f));
        echoHp_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 250.0f, 0.7071f));
        echoLp_.setCoeffs(BiquadCoeffs::lowpass(sampleRate, 3500.0f, 0.7071f));
        widthHp_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 200.0f, 0.7071f));
        echoAmt_.setTime(30.0f, sampleRate);
        width_.setTime(30.0f, sampleRate);
        waterLp_.setSampleRate(sampleRate);
        out_.setTime(20.0f, sampleRate);
        mix_.setTime(20.0f, sampleRate);
        updateEq(true);

        if (info().latencySamples != latency_) setLatency(latency_);
        reset();
    }

    void reset() override
    {
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        std::fill(dryL_.begin(), dryL_.end(), 0.0f);
        std::fill(dryR_.begin(), dryR_.end(), 0.0f);
        std::fill(dec_.begin(), dec_.end(), 0.0f);
        std::fill(echoL_.begin(), echoL_.end(), 0.0f);
        std::fill(echoR_.begin(), echoR_.end(), 0.0f);
        std::fill(stutL_.begin(), stutL_.end(), 0.0f);
        std::fill(stutR_.begin(), stutR_.end(), 0.0f);
        std::fill(widthBuf_.begin(), widthBuf_.end(), 0.0f);
        echoHp_.reset(); echoLp_.reset(); widthHp_.reset();
        echoDelay_ = -1.0f;
        freeStep_ = 0.0; lastStep_ = -1; stutOn_ = false; stutLen_ = 1; stutStart_ = 0; needFadeIn_ = false; fadeInPos_ = 0;
        echoAmt_.snap(get(kEcho) * 0.01f);
        width_.snap(get(kWidth) * 0.01f);
        decimLp_.reset();
        waterLp_.reset();
        lowCut_.reset(); lowShelf_.reset(); highShelf_.reset(); highCut_.reset();
        lead_.reset();
        second_.reset();
        n_ = 0; decN_ = 0; hopCount_ = 0;
        period_ = unvoicedPeriod_;
        voiced_ = false; unvoicedHops_ = 0;
        leadRatio_ = secondRatio_ = 1.0f; naturalCorr_ = 0.0f;
        target_ = -1; robotNote_ = -1;
        lfoPhase_ = driftPhase_ = waterPhase_ = 0.0;
        out_.snap(dbToGain(get(kOutput)));
        mix_.snap(get(kMix) * 0.01f);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int style = getInt(kStyle);
        const float character = get(kCharacter) * 0.01f;
        const bool useSecond = style == kHarmonyStyle || style == kUnderwater;
        float formantSemis = get(kFormant);
        if (style == kChipmunk) formantSemis += 5.0f + extraSize(character);
        if (style == kMonster)  formantSemis -= 5.0f + extraSize(character);
        const float formant = semitonesToRatio(clamp(formantSemis, -9.0f, 9.0f));
        // Level match: octave shifts lose ~4 dB, two voices / the resonant filter gain ~2 dB.
        float wetGain = 1.0f;
        if (style == kChipmunk) wetGain = 1.5f;
        if (style == kMonster)  wetGain = 1.3f;
        if (style == kHarmonyStyle || style == kUnderwater) wetGain = 0.8f;
        out_.setTarget(dbToGain(get(kOutput)));
        mix_.setTarget(get(kMix) * 0.01f);
        echoAmt_.setTarget(get(kEcho) * 0.01f);
        width_.setTarget(get(kWidth) * 0.01f);
        updateEq(false);

        // Tempo: the MPC's when it sends one, else 120 BPM.
        const mpc::Transport& tr = ctx.transport;
        const double bpm = (tr.tempoValid && tr.bpm > 20.0) ? std::min(tr.bpm, 400.0) : 120.0;
        const float echoTarget = std::min((float)(60.0 / bpm) * kEchoTimeBeats[clamp(getInt(kEchoTime), 0, 7)] * sr_,
                                          (float)(echoSize_ - 4));
        if (echoDelay_ < 0.0f) echoDelay_ = echoTarget;
        const double stepLen = 60.0 / bpm / 4.0 * (double)sr_;   // one 1/16 step in samples
        // Stutter steps follow the song position while the MPC plays, else run freely.
        double stepPos = (tr.playing && tr.ppqValid) ? tr.ppqPos * 4.0 : freeStep_;
        const float stutter = get(kStutter) * 0.01f;
        // Underwater: slow sweep of a resonant lowpass, once per block.
        if (style == kUnderwater)
        {
            const float murk = character;
            waterPhase_ += (double)ctx.numFrames * 0.21 / (double)sr_;
            waterPhase_ -= std::floor(waterPhase_);
            const float sweep = 0.5f + 0.5f * fastSin01((float)waterPhase_);
            const float base = 1600.0f - 1250.0f * murk;              // 1600 Hz shallow .. 350 Hz deep
            waterLp_.set(base * (0.7f + 0.6f * sweep), 0.35f + 0.35f * murk);
        }

        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        const int64_t histMask = histSize_ - 1, dryMask = drySize_ - 1, decMask = decSize_ - 1;
        const int64_t echoMask = echoSize_ - 1, stutMask = stutSize_ - 1, widthMask = widthSize_ - 1;

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float l = inL[i], r = inR[i];
            const float mono = 0.5f * (l + r);
            hist_[(size_t)(n_ & histMask)] = mono;
            dryL_[(size_t)(n_ & dryMask)] = l;
            dryR_[(size_t)(n_ & dryMask)] = r;
            ++n_;

            // Feed the pitch detector at the decimated rate.
            const float lp = decimLp_.process(mono);
            if ((n_ % decim_) == 0) dec_[(size_t)(decN_++ & decMask)] = lp;
            if (++hopCount_ >= hop_) { hopCount_ = 0; analyse(style); }

            float wet = lead_.process(hist_.data(), histMask, n_, latency_, period_, leadRatio_, formant);
            if (useSecond)
            {
                const float s = second_.process(hist_.data(), histMask, n_, latency_, period_, secondRatio_, formant);
                wet += (style == kUnderwater ? 0.6f : 0.7f) * s;
            }
            if (style == kUnderwater) wet = waterLp_.process(wet, SVF::Mode::Lowpass);
            wet *= wetGain;

            const int64_t d = n_ - 1 - latency_;
            const float dl = dryL_[(size_t)(d & dryMask)], dr = dryR_[(size_t)(d & dryMask)];
            const float mix = mix_.process(), g = out_.process();
            float yl = dl + (wet - dl) * mix, yr = dr + (wet - dr) * mix;

            const int64_t t = n_ - 1;   // index of this frame

            // Width: a delayed, bass-free copy added to the left and subtracted from the right.
            const float wd = width_.process();
            const float mid = 0.5f * (yl + yr);
            widthBuf_[(size_t)(t & widthMask)] = mid;
            const float side = 0.5f * wd * widthHp_.process(widthBuf_[(size_t)((t - widthDelay_) & widthMask)]);
            yl += side; yr -= side;

            // Stutter.
            stutL_[(size_t)(t & stutMask)] = yl;
            stutR_[(size_t)(t & stutMask)] = yr;
            const int64_t step = (int64_t)std::floor(stepPos);
            if (step != lastStep_)
            {
                lastStep_ = step;
                const bool was = stutOn_;
                stutOn_ = stutter > 0.0f && nextRandom() < stutter;
                if (stutOn_)
                {
                    // Busier settings chop finer: 1/32, 1/64, 1/128.
                    const float pick = nextRandom() * (0.4f + stutter);
                    const int div = pick < 0.5f ? 2 : (pick < 1.0f ? 4 : 8);
                    stutLen_ = std::max(stutFade_ * 2, (int)(stepLen / div));
                    stutStart_ = t;
                }
                if (was) { needFadeIn_ = true; fadeInPos_ = 0; }   // the last repeat faded out
            }
            stepPos += 1.0 / stepLen;
            if (stutOn_)
            {
                const int64_t since = t - stutStart_;
                const int rep = (int)(since / stutLen_), pos = (int)(since % stutLen_);
                if (pos == 0 && rep > 0) { needFadeIn_ = true; fadeInPos_ = 0; }
                const int64_t at = stutStart_ + pos;
                float env = std::min(1.0f, (float)(stutLen_ - 1 - pos) / (float)stutFade_);   // fade out each repeat
                yl = stutL_[(size_t)(at & stutMask)];
                yr = stutR_[(size_t)(at & stutMask)];
                if (needFadeIn_) env = std::min(env, (float)fadeInPos_ / (float)stutFade_);
                yl *= env; yr *= env;
            }
            else if (needFadeIn_)
            {
                const float env = (float)fadeInPos_ / (float)stutFade_;
                yl *= env; yr *= env;
            }
            if (needFadeIn_ && ++fadeInPos_ > stutFade_) needFadeIn_ = false;

            // Echo: filtered ping-pong; the knob raises level and repeats together.
            echoDelay_ += 0.0005f * (echoTarget - echoDelay_);
            const float ea = echoAmt_.process();
            const float readPos = (float)t - echoDelay_;
            const float fl = std::floor(readPos), fr = readPos - fl;
            const int64_t e0 = (int64_t)fl;
            float tl = echoL_[(size_t)(e0 & echoMask)] + fr * (echoL_[(size_t)((e0 + 1) & echoMask)] - echoL_[(size_t)(e0 & echoMask)]);
            float trr = echoR_[(size_t)(e0 & echoMask)] + fr * (echoR_[(size_t)((e0 + 1) & echoMask)] - echoR_[(size_t)(e0 & echoMask)]);
            echoHp_.process(tl, trr);
            echoLp_.process(tl, trr);
            const float fb = 0.2f + 0.45f * ea;
            echoL_[(size_t)(t & echoMask)] = ea > 0.0f ? 0.5f * (yl + yr) + fb * trr : 0.0f;
            echoR_[(size_t)(t & echoMask)] = ea > 0.0f ? fb * tl : 0.0f;
            yl += 0.6f * ea * tl;
            yr += 0.6f * ea * trr;

            if (lowCutOn_)    lowCut_.process(yl, yr);
            if (lowShelfOn_)  lowShelf_.process(yl, yr);
            if (highShelfOn_) highShelf_.process(yl, yr);
            if (highCutOn_)   highCut_.process(yl, yr);
            outL[i] = yl * g;
            outR[i] = yr * g;
        }
        freeStep_ = stepPos;
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name           = "Voice Morph";
        info.vendor         = "Germanikkus";   // must match plugin.json "manufacturer"
        info.uniqueId       = MPC_FOURCC('G', 'C', 'g', '8');   // must match plugin.json "uid"
        info.version        = 1200;
        info.isInstrument   = false;
        info.numInputs      = 2;
        info.numOutputs     = 2;
        info.latencySamples = (int)std::ceil(44100.0f / kMinHz) + 4;   // updated in prepare() for other rates
        info.tailSamples    = 44100 * 6;   // echo repeats
        return info;
    }

    // Glides the EQ toward the knob settings once per block (no zipper noise) and
    // recomputes only the filters that moved. A filter at its "off" setting is skipped.
    void updateEq(bool snap)
    {
        const float k = snap ? 1.0f : 0.25f;
        const float topHz = std::min(20000.0f, 0.45f * sr_);
        const float target[4] = {std::log2(get(kLowCut)), get(kLows), get(kHighs), std::log2(std::min(get(kHighCut), topHz))};
        bool moved[4];
        for (int j = 0; j < 4; ++j)
        {
            const float before = eq_[j];
            eq_[j] += k * (target[j] - eq_[j]);
            if (std::fabs(target[j] - eq_[j]) < 0.002f) eq_[j] = target[j];
            moved[j] = snap || eq_[j] != before;
        }
        if (moved[0]) lowCut_.setCoeffs(BiquadCoeffs::highpass(sr_, std::exp2(eq_[0]), 0.7071f));
        if (moved[1]) lowShelf_.setCoeffs(BiquadCoeffs::lowShelf(sr_, kLowShelfHz, 0.7071f, eq_[1]));
        if (moved[2]) highShelf_.setCoeffs(BiquadCoeffs::highShelf(sr_, std::min(kHighShelfHz, 0.4f * sr_), 0.7071f, eq_[2]));
        if (moved[3]) highCut_.setCoeffs(BiquadCoeffs::lowpass(sr_, std::exp2(eq_[3]), 0.7071f));

        const bool on[4] = {eq_[0] > std::log2(20.5f), eq_[1] != 0.0f, eq_[2] != 0.0f, eq_[3] < std::log2(0.99f * topHz)};
        // A filter switching back on starts from silence rather than stale memory.
        if (on[0] && !lowCutOn_) lowCut_.reset();
        if (on[1] && !lowShelfOn_) lowShelf_.reset();
        if (on[2] && !highShelfOn_) highShelf_.reset();
        if (on[3] && !highCutOn_) highCut_.reset();
        lowCutOn_ = on[0]; lowShelfOn_ = on[1]; highShelfOn_ = on[2]; highCutOn_ = on[3];
    }

    // What the Character knob (0..1) means in each Style.
    static float naturalSpeedMs(float c) { return 240.0f * c * c; }   // 50% = 60 ms
    static float hardTuneSpeedMs(float c) { const float e = std::max(0.0f, 2.0f * c - 1.0f); return 60.0f * e * e; }   // up to 50% = instant
    static int   robotChoice(float c) { return c < 0.25f ? 0 : (c < 0.75f ? 1 : 2); }
    static float extraSize(float c) { return (c - 0.5f) * 8.0f; }   // -4..+4 st on top of the style's own
    static int   harmChoice(float c) { return kHarmOrder[clamp((int)(c * 5.0f), 0, 4)]; }

    float nextRandom()   // 0..1, xorshift
    {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return (float)(rng_ >> 8) * (1.0f / 16777216.0f);
    }

    static int pow2AtLeast(int v) { int s = 16; while (s < v) s <<= 1; return s; }

    static float toRatio(float semis) { return clamp(semitonesToRatio(semis), 0.5f, 2.05f); }

    // Runs every hop: detect pitch, then decide how far each voice moves.
    void analyse(int style)
    {
        float hz = detectPitch();
        if (hz > 0.0f) { voiced_ = true; unvoicedHops_ = 0; period_ = sr_ / hz; lastHz_ = hz; }
        else if (++unvoicedHops_ > 2) { voiced_ = false; period_ = unvoicedPeriod_; }
        else if (voiced_) hz = lastHz_;   // bridge a short dropout

        const float hopSec = (float)hop_ / sr_;
        lfoPhase_ += (double)hopSec * 5.5;     // vibrato / chorus movement
        lfoPhase_ -= std::floor(lfoPhase_);
        driftPhase_ += (double)hopSec * 0.7;   // slow underwater pitch drift
        driftPhase_ -= std::floor(driftPhase_);
        const float lfo = fastSin01((float)lfoPhase_);
        const float character = get(kCharacter) * 0.01f;
        const float drift = character * 0.35f * fastSin01((float)driftPhase_);

        // Octave moves and the Underwater drift apply to breaths and consonants too.
        float fixed = 0.0f;
        if (style == kChipmunk) fixed = 12.0f;
        if (style == kMonster)  fixed = -12.0f;

        if (!voiced_)
        {
            naturalCorr_ *= 0.8f;
            target_ = robotNote_ = -1;
            const float lead = fixed + (style == kUnderwater ? drift : 0.0f);
            leadRatio_ = toRatio(lead);
            secondRatio_ = style == kUnderwater ? toRatio(-drift + 0.12f) : leadRatio_;
            return;
        }

        const float m = 69.0f + 12.0f * std::log2(hz / 440.0f);
        const int key = getInt(kKey);
        const int mask = kScaleMasks[clamp(getInt(kScale), 0, 5)];
        target_ = quantize(m, key, mask, target_);
        const float hard = (float)target_ - m;
        float lead = hard, second = 0.0f;

        switch (style)
        {
            case kNatural:
            case kHardTune:
            {
                const float speedMs = style == kNatural ? naturalSpeedMs(character) : hardTuneSpeedMs(character);
                const float c = speedMs <= 0.5f ? 1.0f : 1.0f - std::exp(-hopSec * 1000.0f / speedMs);
                naturalCorr_ += c * (hard - naturalCorr_);
                lead = naturalCorr_;
                break;
            }
            case kRobot:
                robotNote_ = quantize(m, key, 1, robotNote_);   // nearest root note, any octave
                lead = (float)(robotNote_ + kRobotOffsets[robotChoice(character)]) - m;
                break;
            case kHarmonyStyle:
            {
                static const int steps[] = {2, 4, 0, -2, 0};
                static const int semis[] = {4, 7, 12, -4, -12};
                const int h = harmChoice(character);
                const int other = (h == 2 || h == 4 || mask == 0xFFF) ? target_ + semis[h]
                                                                       : stepScale(target_, steps[h], key, mask);
                second = (float)other - m;
                break;
            }
            case kWarble:
                lead = hard + character * 1.2f * lfo;
                break;
            case kUnderwater:
                lead = drift;
                second = -drift + 0.12f + 0.08f * lfo;
                break;
            default:   // Chipmunk, Monster
                lead = hard + fixed;
                break;
        }
        leadRatio_ = toRatio(lead);
        secondRatio_ = toRatio(second);
    }

    // YIN on the newest decimated frame. Returns Hz, or 0 when not voiced.
    float detectPitch()
    {
        const int frameLen = 2 * maxLag_;
        if (decN_ < frameLen) return 0.0f;
        const int64_t decMask = decSize_ - 1;
        float energy = 0.0f;
        for (int j = 0; j < frameLen; ++j)
        {
            const float x = dec_[(size_t)((decN_ - frameLen + j) & decMask)];
            frame_[(size_t)j] = x;
            energy += x * x;
        }
        if (energy / (float)frameLen < 1.0e-5f) return 0.0f;   // quieter than about -50 dBFS

        const int W = maxLag_;
        const float* x = frame_.data();
        const float threshold = 0.06f + 0.24f * get(kSens) * 0.01f;
        float running = 0.0f;
        int found = -1;
        diff_[0] = 1.0f;
        for (int tau = 1; tau < maxLag_; ++tau)
        {
            float d = 0.0f;
            for (int j = 0; j < W; ++j) { const float t = x[j] - x[j + tau]; d += t * t; }
            running += d;
            diff_[(size_t)tau] = running > 0.0f ? d * (float)tau / running : 1.0f;
            // First dip under the threshold, taken at its lowest point.
            if (tau - 1 > minLag_ && diff_[(size_t)tau - 1] < threshold && diff_[(size_t)tau - 1] <= diff_[(size_t)tau])
            {
                found = tau - 1;
                break;
            }
        }
        if (found < 2) return 0.0f;

        // Parabolic interpolation around the dip for sub-sample accuracy.
        const float a = diff_[(size_t)found - 1], b = diff_[(size_t)found], c = diff_[(size_t)found + 1];
        const float den = a - 2.0f * b + c;
        float tau = (float)found;
        if (std::fabs(den) > 1.0e-9f) tau += clamp(0.5f * (a - c) / den, -0.5f, 0.5f);
        return srd_ / tau;
    }

    float sr_ = 44100.0f, srd_ = 22050.0f;
    int decim_ = 2, hop_ = 256, maxLag_ = 317, minLag_ = 22, latency_ = 634;
    float maxPeriod_ = 630.0f, unvoicedPeriod_ = 352.0f;

    std::vector<float> hist_, dryL_, dryR_, dec_, frame_, diff_;
    int histSize_ = 0, drySize_ = 0, decSize_ = 0;
    int64_t n_ = 0, decN_ = 0;
    int hopCount_ = 0;

    Biquad decimLp_;
    SVF waterLp_;
    StereoBiquad lowCut_, lowShelf_, highShelf_, highCut_;
    StereoBiquad echoHp_, echoLp_;
    Biquad widthHp_;
    Smoother echoAmt_, width_;
    std::vector<float> echoL_, echoR_, stutL_, stutR_, widthBuf_;
    int echoSize_ = 0, stutSize_ = 0, widthSize_ = 0, widthDelay_ = 529, stutFade_ = 110;
    float echoDelay_ = -1.0f;
    double freeStep_ = 0.0;
    int64_t lastStep_ = -1, stutStart_ = 0;
    int stutLen_ = 1, fadeInPos_ = 0;
    bool stutOn_ = false, needFadeIn_ = false;
    uint32_t rng_ = 0x9E3779B9u;
    float eq_[4] = {4.32f, 0.0f, 0.0f, 14.29f};   // current Low Cut (log2 Hz), Lows dB, Highs dB, High Cut (log2 Hz)
    bool lowCutOn_ = false, lowShelfOn_ = false, highShelfOn_ = false, highCutOn_ = false;
    Shifter lead_, second_;
    Smoother out_, mix_;

    float period_ = 352.0f, lastHz_ = 0.0f;
    bool voiced_ = false;
    int unvoicedHops_ = 0;
    float leadRatio_ = 1.0f, secondRatio_ = 1.0f, naturalCorr_ = 0.0f;
    int target_ = -1, robotNote_ = -1;
    double lfoPhase_ = 0.0, driftPhase_ = 0.0, waterPhase_ = 0.0;
};

MPC_PLUGIN_ENTRY(VoiceMorph)
