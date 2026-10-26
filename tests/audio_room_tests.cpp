// The hall's precomputed acoustics (src/audio/room.cpp): reverberation times and room constant from
// the hall's geometry and the published absorption coefficients, and the image-source early
// reflections between a talker and a listener.
#include "test.h"
#include "audio/room.h"
#include "game/layout.h"
#include "scene/hall.h"
#include <cmath>

namespace {

using namespace audio;

// White's seat (the default ListenerPose) and the coach's mouth across the table.
const m::vec3 kSeat(0.0f, 1.23f, 0.62f);
const m::vec3 kMouth(0.0f, 1.17f, -0.60f);

bool relNear(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::fabs(b); }

// Reflections of the first order that are not the table top.
int countFirstOrderAt(const ReflectionSet& s, double delay, double tol = 1e-6) {
    int n = 0;
    for (int i = 0; i < s.count; ++i)
        if (s.r[size_t(i)].order == 1 && !s.r[size_t(i)].table && std::fabs(s.r[size_t(i)].delay - delay) < tol) ++n;
    return n;
}

// The delay of the first-order image of 'src' across the plane axis = pos, by hand.
double imageDelay(m::vec3 src, m::vec3 lis, int axis, double pos) {
    double s[3] = {src.x, src.y, src.z}, l[3] = {lis.x, lis.y, lis.z};
    double direct = 0.0, path = 0.0;
    for (int k = 0; k < 3; ++k) {
        direct += (s[k] - l[k]) * (s[k] - l[k]);
        const double img = k == axis ? 2.0 * pos - s[k] : s[k];
        path += (img - l[k]) * (img - l[k]);
    }
    return (std::sqrt(path) - std::sqrt(direct)) / double(kSpeedOfSound);
}

}  // namespace

TEST(room_rt60_mid_band_matches_hand_calculation) {
    const RoomAcoustics& r = hallAcoustics();
    using namespace layout;
    const double X = HALL_MAX_X - HALL_MIN_X, Z = HALL_MAX_Z - HALL_MIN_Z, H = HALL_HEIGHT;
    const double V = X * Z * H;
    const double S = 2.0 * (X * Z + X * H + Z * H);   // 1264 m2
    CHECK(relNear(r.volume, V, 1e-6));
    CHECK(relNear(r.surface, S, 1e-6));
    CHECK(relNear(r.meanFreePath, 4.0 * V / S, 1e-5));

    // 1 kHz, by hand: the areas of the hall's surfaces and their coefficients.
    const double pi = 3.14159265358979;
    const double windows = 3.0 * (2.6 * (hall::WINDOW_SPRING_Y - 0.9) + 0.5 * pi * 1.3 * 1.3);   // 3 x 16.17
    // Curtains: footprint of the gathered drapes on the wall (3.74 m2 each, 0.31 m2 of it over the
    // wainscot; integrated from hall_windows.cpp), draped-to-half-area row 0.72 at 1 kHz.
    const double curtains = 6.0 * 3.744, curtainOnDado = 6.0 * 0.311;
    const double dado = Z * hall::DADO_TOP * 2.0 + X * hall::DADO_TOP + (X - 2.2) * hall::DADO_TOP - curtainOnDado;
    const double tapestries = 3 * 3.2 * 5.0 + 2 * 2.8 * 5.0 + 1 * 3.2 * 5.0;                       // 92 m2
    const double door = 2.2 * 4.4;
    const double stone = 2.0 * Z * H + 2.0 * X * H - windows - curtains - dado - tapestries - door;
    // Tapestries hang 4.5 cm off the wall: the contact row (0.17 at 1 kHz) and the 10 cm-gap row
    // (0.75) interpolated in gap.
    const double tapAlpha = 0.17 + (4.5 / 10.0) * (0.75 - 0.17);
    double sa = (X * Z) * 0.01 + (X * Z) * 0.03 + stone * 0.04 + dado * 0.09 + windows * 0.12 + curtains * 0.72 +
                tapestries * tapAlpha + door * 0.08;
    sa += 1.5 * 0.07 + 2 * (0.25 + 0.32) * 0.88 + 2 * 1.5 * 0.03;   // table, chair seats and backs, robots
    const double a = sa / S;
    const double air = 4.0 * 0.001074 * V;
    const double rt = 0.161 * V / (-S * std::log(1.0 - a) + air);   // 3.92 s
    CHECK(relNear(r.rt60[kRoomMidBand], rt, 0.05));
    CHECK(relNear(r.meanAlpha[kRoomMidBand], a, 0.05));
    CHECK(relNear(r.roomConstant[kRoomMidBand], (S * a + air) / (1.0 - a), 0.05));
    // A bare stone hall of this size with tapestries and curtains: a few seconds, not tens.
    CHECK(r.rt60[kRoomMidBand] > 3.0f && r.rt60[kRoomMidBand] < 7.0f);
    for (int b = 0; b < kRoomBands; ++b) {
        CHECK(r.meanAlpha[size_t(b)] > 0.0f && r.meanAlpha[size_t(b)] < 0.5f);
        CHECK(r.rt60[size_t(b)] > 0.5f && r.rt60[size_t(b)] < 12.0f);
        CHECK(r.roomConstant[size_t(b)] > 0.0f);
    }
}

TEST(room_rt60_falls_towards_high_frequencies) {
    const RoomAcoustics& r = hallAcoustics();
    // Air absorption grows steeply, and the fabrics take the highs: 1 k > 2 k > 4 k > 8 k.
    for (int b = kRoomMidBand; b + 1 < kRoomBands; ++b) CHECK(r.rt60[size_t(b + 1)] < r.rt60[size_t(b)]);
    CHECK(r.rt60[6] < 0.5f * r.rt60[kRoomMidBand]);   // the 8 kHz band is mostly air
    for (int b = 0; b + 1 < kRoomBands; ++b) CHECK(r.airM[size_t(b + 1)] > r.airM[size_t(b)]);
    CHECK(r.roomConstant[6] > r.roomConstant[kRoomMidBand]);   // a drier room: a stronger direct sound
}

TEST(room_critical_distance_follows_the_room_constant) {
    const RoomAcoustics& r = hallAcoustics();
    const double pi = 3.14159265358979;
    const double R = r.roomConstant[kRoomMidBand];
    CHECK(relNear(criticalDistance(1.0f), std::sqrt(R / (16.0 * pi)), 1e-4));
    CHECK(relNear(criticalDistance(kTalkerQ), std::sqrt(kTalkerQ * R / (16.0 * pi)), 1e-4));
    CHECK(relNear(criticalDistance(2.0f), std::sqrt(2.0) * criticalDistance(1.0f), 1e-4));
    // At the critical distance the diffuse and the direct energies are equal; closer, the direct wins.
    const float rc = criticalDistance(kTalkerQ);
    CHECK(relNear(diffuseToDirect(rc, kTalkerQ), 1.0, 1e-3));
    CHECK(diffuseToDirect(0.5f * rc, kTalkerQ) < 0.3f);
    CHECK(diffuseToDirect(2.0f * rc, kTalkerQ) > 3.9f);
    for (int b = 0; b < kRoomBands; ++b)
        CHECK(relNear(criticalDistance(1.0f, b), std::sqrt(r.roomConstant[size_t(b)] / (16.0 * pi)), 1e-4));
}

TEST(room_early_reflections_coach_to_white_seat) {
    const m::vec3 facing = m::normalize(kSeat - kMouth);   // the coach faces the player
    const ReflectionSet s = earlyReflections(kMouth, kSeat, facing);
    CHECK(relNear(s.directDistance, std::sqrt(1.22 * 1.22 + 0.06 * 0.06), 1e-5));
    REQUIRE(s.count > 8 && s.count <= kMaxReflections);

    for (int i = 0; i < s.count; ++i) {
        const Reflection& r = s.r[size_t(i)];
        CHECK(r.delay > 0.0f);
        CHECK(r.gain > 0.0f && r.gain < 1.0f);
        CHECK(r.hfGain > 0.0f && r.hfGain < 1.0f);
        CHECK(std::fabs(m::length(r.dir) - 1.0f) < 1e-4f);
        CHECK(r.order == 1 || r.order == 2);
        if (i > 0) CHECK(s.r[size_t(i - 1)].delay <= r.delay);
    }

    // The side walls at x = +-7 m, by hand: image at x = +-14, path sqrt(14^2 + 1.22^2 + 0.06^2).
    const double side = (std::sqrt(14.0 * 14.0 + 1.22 * 1.22 + 0.06 * 0.06) - s.directDistance) / double(kSpeedOfSound);
    CHECK(relNear(side, imageDelay(kMouth, kSeat, 0, 7.0), 1e-6));
    CHECK(relNear(side, imageDelay(kMouth, kSeat, 0, -7.0), 1e-6));
    CHECK_EQ(countFirstOrderAt(s, side, 2e-6), 2);   // one image per side wall: 37.4 ms
    for (int i = 0; i < s.count; ++i)
        if (s.r[size_t(i)].order == 1 && !s.r[size_t(i)].table && std::fabs(s.r[size_t(i)].delay - side) < 2e-6)
            CHECK(std::fabs(s.r[size_t(i)].dir.x) > 0.99f);

    // The end walls and the ceiling are there too.
    CHECK_EQ(countFirstOrderAt(s, imageDelay(kMouth, kSeat, 2, layout::HALL_MAX_Z), 2e-6), 1);
    CHECK_EQ(countFirstOrderAt(s, imageDelay(kMouth, kSeat, 2, layout::HALL_MIN_Z), 2e-6), 1);
    CHECK_EQ(countFirstOrderAt(s, imageDelay(kMouth, kSeat, 1, layout::HALL_HEIGHT), 2e-6), 1);

    // The floor bounce passes under the table top: blocked (4.3 ms after the direct sound).
    const double floorDelay = imageDelay(kMouth, kSeat, 1, 0.0);
    CHECK(floorDelay > 0.004 && floorDelay < 0.0045);
    CHECK_EQ(countFirstOrderAt(s, floorDelay, 2e-5), 0);
    for (int i = 0; i < s.count; ++i)
        if (s.r[size_t(i)].order == 1) CHECK(s.r[size_t(i)].table || s.r[size_t(i)].dir.y > -0.5f);

    // The table-top bounce: image of the mouth below the top (y = 0.35), 0.8 ms behind the direct sound.
    int tables = 0;
    for (int i = 0; i < s.count; ++i) {
        const Reflection& r = s.r[size_t(i)];
        if (!r.table) continue;
        ++tables;
        const double path = std::sqrt(1.22 * 1.22 + (1.23 - (2.0 * 0.76 - 1.17)) * (1.23 - (2.0 * 0.76 - 1.17)));
        CHECK(r.delay < 0.002f);
        CHECK(relNear(r.delay, (path - s.directDistance) / double(kSpeedOfSound), 1e-3));
        CHECK_EQ(r.order, 1);
        CHECK(r.dir.y < -0.5f);
        CHECK(relNear(r.gain, 0.91 * s.directDistance / path, 0.03));   // wood: sqrt(1 - 0.07), the coach faces the seat
        CHECK(i == 0);                                                // the earliest of all
    }
    CHECK_EQ(tables, 1);
}

TEST(room_early_reflection_gain_of_a_wall_by_hand) {
    // Omnidirectional source: the +X wall's first-order image carries spreading, the wall's
    // reflection factor at 1 kHz (three tapestries 4.5 cm off the wall, wainscot, limestone) and the
    // air over the extra path.
    const ReflectionSet s = earlyReflections(kMouth, kSeat);
    const double X = 14.0, Z = 22.0, H = 9.0;
    const double tap = 3 * 3.2 * 5.0, dado = Z * hall::DADO_TOP;
    const double tapAlpha = 0.17 + (4.5 / 10.0) * (0.75 - 0.17);
    const double a = (tap * tapAlpha + dado * 0.09 + (Z * H - tap - dado) * 0.04) / (Z * H);
    const double path = std::sqrt((X - 0.0) * (X - 0.0) + 1.22 * 1.22 + 0.06 * 0.06);
    const double extra = path - s.directDistance;
    const double expected = s.directDistance / path * std::sqrt(1.0 - a) * std::exp(-0.5 * 0.001074 * extra);
    int found = 0;
    for (int i = 0; i < s.count; ++i) {
        const Reflection& r = s.r[size_t(i)];
        if (r.order == 1 && !r.table && r.dir.x > 0.99f) {
            ++found;
            CHECK(relNear(r.gain, expected, 0.01));
            CHECK(r.hfGain < r.gain);   // 4 kHz: more absorbing tapestries and more air
        }
    }
    CHECK_EQ(found, 1);
}

TEST(room_early_reflections_follow_the_listener) {
    const m::vec3 facing = m::normalize(kSeat - kMouth);
    const ReflectionSet a = earlyReflections(kMouth, kSeat, facing);
    const m::vec3 moved(3.0f, 1.23f, -2.5f);   // clear of the table, on the coach's side
    const ReflectionSet b = earlyReflections(kMouth, moved, m::normalize(moved - kMouth));
    CHECK(std::fabs(a.directDistance - b.directDistance) > 1.0f);
    REQUIRE(a.count > 6 && b.count > 6);
    int differing = 0;
    for (int i = 0; i < 6; ++i)
        if (std::fabs(a.r[size_t(i)].delay - b.r[size_t(i)].delay) > 1e-4f) ++differing;
    CHECK(differing >= 4);
    for (int i = 1; i < b.count; ++i) CHECK(b.r[size_t(i - 1)].delay <= b.r[size_t(i)].delay);
    // Away from the table the floor bounce is free, and the x walls are no longer symmetric (the
    // nearer one comes sooner).
    CHECK_EQ(countFirstOrderAt(b, imageDelay(kMouth, moved, 1, 0.0), 2e-5), 1);
    CHECK_EQ(countFirstOrderAt(b, imageDelay(kMouth, moved, 0, layout::HALL_MAX_X), 2e-6), 1);
    CHECK_EQ(countFirstOrderAt(b, imageDelay(kMouth, moved, 0, layout::HALL_MIN_X), 2e-6), 1);
    CHECK(imageDelay(kMouth, moved, 0, layout::HALL_MAX_X) < imageDelay(kMouth, moved, 0, layout::HALL_MIN_X));
    // Listener or source below the table top: no table image.
    const ReflectionSet under = earlyReflections(kMouth, m::vec3(0.0f, 0.5f, 0.62f), facing);
    for (int i = 0; i < under.count; ++i) CHECK(!under.r[size_t(i)].table);
}
