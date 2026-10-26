// The hall's acoustics, precomputed from its geometry (game/layout.h, scene/hall.h) and published
// absorption coefficients of its surfaces: per-band reverberation times (Eyring, with air
// absorption), the room constant (diffuse level against direct sound), and image-source early
// reflections for a source and listener. Pure functions of constants and positions: no audio-thread
// state, no allocation. The hall figures are computed once; the reflections again only when the
// listener (or a reflection bank's source) moves.
#pragma once
#include "../math/math.h"
#include <array>

namespace audio {

constexpr int kRoomBands = 7;   // octave bands
constexpr float kRoomBandHz[kRoomBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};
constexpr int kRoomMidBand = 3;  // 1 kHz
constexpr float kSpeedOfSound = 343.0f;  // m/s, 20 C

// Talker directivity (speech): on-axis 1, behind 0.55, as a function of the cosine between the
// talker's facing and the direction of the sound. Its directivity factor Q is the on-axis power
// over the mean power on the sphere: 1 / (0.775^2 + 0.225^2 / 3) = 1.62.
inline float talkerDirectivity(float cosAngle) { return 0.775f + 0.225f * cosAngle; }
constexpr float kTalkerQ = 1.0f / (0.775f * 0.775f + 0.225f * 0.225f / 3.0f);

struct RoomAcoustics {
    float volume = 0.0f;                              // m^3
    float surface = 0.0f;                             // m^2, every absorbing surface
    std::array<float, kRoomBands> meanAlpha{};        // area-weighted absorption coefficient
    std::array<float, kRoomBands> airM{};             // air attenuation m (1/m, energy), 20 C 50% RH
    std::array<float, kRoomBands> rt60{};             // s: Eyring, with air absorption (4 m V)
    std::array<float, kRoomBands> roomConstant{};     // R = A / (1 - mean alpha), A = S a' + 4 m V (m^2)
    float meanFreePath = 0.0f;                        // 4 V / S (m)
};
// The hall, computed on first use (thread-safe static).
const RoomAcoustics& hallAcoustics();

// Diffuse-field energy over the direct energy at distance r (m) from a source of directivity
// factor q, in band b: 16 pi r^2 / (q R_b).
float diffuseToDirect(float r, float q, int band = kRoomMidBand);
// The distance where both are equal: sqrt(q R_b / (16 pi)).
float criticalDistance(float q, int band = kRoomMidBand);

struct Reflection {
    float delay = 0.0f;     // s after the direct sound ((path - direct) / c)
    float gain = 0.0f;      // amplitude relative to the direct sound at 1 kHz: spreading (direct / path),
                            // the surfaces' reflection factors, the source's directivity towards it
    float hfGain = 0.0f;    // the same at 4 kHz (absorption and air tilt the reflections dark)
    m::vec3 dir;            // arrival direction at the listener (unit, world, pointing to the image)
    int order = 0;          // 1 or 2 (the table top counts as a surface)
    bool table = false;     // the image is the table top's (not one of the hall's six boundaries)
};
constexpr int kMaxReflections = 32;
struct ReflectionSet {
    int count = 0;
    std::array<Reflection, kMaxReflections> r{};  // sorted by delay
    float directDistance = 0.0f;                  // m
};
// Image sources up to the second order of the hall's six boundaries, plus the table top for a
// source above it. A path is dropped when the table blocks it (a floor bounce under the table)
// or when its reflection point falls outside a finite surface (the table top). 'facing' zero =
// omnidirectional; else each image is weighted by talkerDirectivity towards it (the direct
// sound by the same towards the listener, so 'gain' stays relative to what is heard direct).
ReflectionSet earlyReflections(m::vec3 source, m::vec3 listener, m::vec3 facing = m::vec3(0.0f));

}  // namespace audio
