// The hall's acoustics from its geometry and materials (room.h).
//
// Geometry: game/layout.h (14 x 22 x 9 m shoebox, three arched windows in the -X wall, the table),
// scene/hall.h (wainscot to DADO_TOP, tapestries, doors), scene/hall_decor.cpp (the six tapestries,
// the double door), scene/hall_windows.cpp (six velvet curtain panels), scene/hall_floor.cpp (marble),
// scene/hall_shell.cpp (limestone walls), scene/hall_ceiling.cpp (coffered ceiling, painted panels
// and stone ribs).
//
// Absorption coefficients are the usual published random-incidence values for the octave bands
// 125 Hz .. 4 kHz, as reprinted in M. Vorlaender, "Auralization" (Springer 2008), Appendix A,
// H. Kuttruff, "Room Acoustics" (5th ed.), Table 1 and T. Cox / P. D'Antonio, "Acoustic Absorbers and
// Diffusers", Appendix: the table row each material follows is named at its line. Where the tables
// stop at 4 kHz, the 8 kHz value is the 4 kHz one (the coefficients of such surfaces rise slowly, so
// this is the cautious end; the air takes the 8 kHz band anyway). Bands in all arrays below:
// 125, 250, 500, 1000, 2000, 4000, 8000 Hz.
//
// Not modelled (each is below the tabulated coefficients' own scatter): the extra volume of the
// coffers (0.22 m deep over the ceiling = +1.5% of V) and the extra surface of the relief
// (ribs, pilasters, cornice: scattering, not absorption), the framed paintings, the chandelier's
// crystal, the consoles under the paintings.
#include "room.h"
#include "../game/layout.h"
#include "../scene/hall.h"
#include <algorithm>
#include <cmath>

namespace audio {
namespace {

using Bands = std::array<float, kRoomBands>;

struct Material {
    const char* name;
    Bands alpha;
};

// ---- Materials --------------------------------------------------------------------------------
// Polished marble (floor FloorMarble / FloorMarbleInlay): Vorlaender "marble, glazed tile".
constexpr Material kMarble{"marble", {0.01f, 0.01f, 0.01f, 0.01f, 0.02f, 0.02f, 0.02f}};
// Limestone / rough plaster on masonry (WallStone walls and pilasters): Kuttruff Table 1 "plaster on
// brick/stone, rough" family, Vorlaender "plaster on stone".
constexpr Material kStone{"limestone", {0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.05f, 0.05f}};
// Painted plaster/stucco coffers and plafond (CeilingPainted + stone ribs): Cox & D'Antonio
// "plaster, smooth finish on tile or brick", the coffers' thin panels adding a little at low frequency.
constexpr Material kCeiling{"painted plaster ceiling", {0.03f, 0.03f, 0.03f, 0.03f, 0.04f, 0.05f, 0.05f}};
// Thin wood panelling over a masonry wall with an air space (the wainscot WallPanelWood): Cox &
// D'Antonio / Kuttruff "wood panelling, 5 mm, air space behind".
constexpr Material kPanelling{"wood panelling", {0.28f, 0.22f, 0.17f, 0.09f, 0.10f, 0.11f, 0.11f}};
// Heavy wooden door (two leaves, WallPanelWood): Vorlaender "wooden door".
constexpr Material kDoor{"wooden door", {0.14f, 0.10f, 0.06f, 0.08f, 0.10f, 0.10f, 0.10f}};
// Window glass, large panes in frames: Vorlaender "window glass, typical".
constexpr Material kGlass{"window glass", {0.35f, 0.25f, 0.18f, 0.12f, 0.07f, 0.04f, 0.04f}};
// Tapestries (woven heavy wool, ~475 g/m2 class, hung 4.5 cm off the wall: hall_decor.cpp, n = 0.045 m
// plus folds of 1-2 cm, 6 hangings, face area only, the hems and fringe add no tabulated area). Two
// published rows bracket the hanging: Kuttruff Table 1 / Vorlaender App. A "heavy velour, 475 g/m2,
// hung straight in contact with the wall" {.03 .04 .11 .17 .24 .35} (bands 125..4k) and the
// Fasold / Vorlaender "heavy curtain, ~0.5 kg/m2, hung 10 cm from the wall" {.07 .31 .49 .75 .70 .60}.
// A porous sheet absorbs at its velocity maximum, a quarter wavelength from the wall; the gap moves
// that peak (c/4d = 1.9 kHz at 4.5 cm, 0.85 kHz at 10 cm), so the 4.5 cm row is interpolated linearly
// in gap, w = 4.5/10 = 0.45: alpha = contact + 0.45 (gap10 - contact). 8 kHz = 4 kHz. A closely woven
// tapestry is denser than velour, so the result is an upper-middle estimate. Values here are
// transcribed from the commonly reprinted tables (not re-checked against the books).
constexpr Material kTapestry{"tapestry", {0.048f, 0.162f, 0.281f, 0.431f, 0.447f, 0.463f, 0.463f}};
// Window curtains (heavy velvet 475 g/m2, gathered and tied back, hall_windows.cpp: hung from a rod
// 0.20 m off the wall, hanging 0.2..0.25 m off it, K = 7 folds of up to 7.5 cm): Kuttruff Table 1 /
// Vorlaender App. A "velour, 475 g/m2, draped to half area" {.14 .35 .55 .72 .70 .65}, a row for fabric
// hung clear of the wall, which the 0.2 m gap matches (the larger gap would only add low-frequency
// absorption). "Half area" means the cloth is twice the covered wall area. The coefficient therefore
// applies to the footprint on the wall, not to half the flat cloth: the scene's drape is 0.24 m wide
// at the tie-back, 0.43 m at the floor, 0.65 m at the rod, 3.74 m2 per panel (1.28 x 7.67 = 9.82 m2
// of cloth, a ratio of 2.6, slightly denser than the row's 2: conservative for the real drape).
constexpr Material kCurtain{"velvet curtain", {0.14f, 0.35f, 0.55f, 0.72f, 0.70f, 0.65f, 0.65f}};
// Solid wood furniture (table top and legs): Vorlaender "wood, polished, solid" (parquet-on-concrete
// row: wood on a stiff support).
constexpr Material kWood{"wood", {0.15f, 0.11f, 0.10f, 0.07f, 0.06f, 0.07f, 0.07f}};
// Unoccupied upholstered seat, per m2 of floor covered: Beranek, "Concert Halls and Opera Houses",
// Table (upholstered seats, unoccupied), as reprinted in Kuttruff Table 1.
constexpr Material kUpholstery{"upholstered chair", {0.49f, 0.66f, 0.80f, 0.88f, 0.82f, 0.70f, 0.70f}};
// Hard plastic / painted metal shells (the two robots): Vorlaender "painted metal, hard plastic".
constexpr Material kShell{"robot shell", {0.05f, 0.04f, 0.03f, 0.03f, 0.03f, 0.03f, 0.03f}};

// Air attenuation (energy, 1/m) at 20 C, 50% RH, 101.325 kPa: m = alpha_dB/m * ln(10) / 10 with
// alpha from ISO 9613-1 at the nominal band centres (0.44, 1.31, 2.73, 4.67, 9.89, 29.7, 105.3 dB/km).
constexpr Bands kAirM = {0.000101f, 0.000302f, 0.000628f, 0.001074f, 0.002277f, 0.006831f, 0.024244f};

// ---- Boundaries -------------------------------------------------------------------------------
enum Boundary { kMinX, kMaxX, kMinZ, kMaxZ, kFloor, kCeil, kBoundaries };
struct Plane {
    int axis;    // 0 x, 1 y, 2 z
    float pos;
};
constexpr Plane kPlanes[kBoundaries] = {{0, layout::HALL_MIN_X}, {0, layout::HALL_MAX_X}, {2, layout::HALL_MIN_Z},
                                        {2, layout::HALL_MAX_Z}, {1, 0.0f},               {1, layout::HALL_HEIGHT}};

struct Patch {
    Boundary boundary;
    double area;  // m2
    const Material* material;
};
// A small absorber standing in the room (its footprint is not part of the boundary surface):
// 'area' m2 of 'material' per object.
struct Absorber {
    const char* name;
    int count;
    double area;
    const Material* material;
};

constexpr double kPi = 3.14159265358979323846;

struct HallData {
    RoomAcoustics room;
    std::array<Bands, kBoundaries> boundaryAlpha{};   // area-weighted over the patches of one boundary
};

HallData computeHall() {
    using namespace layout;
    const double X = HALL_MAX_X - HALL_MIN_X, Z = HALL_MAX_Z - HALL_MIN_Z, H = HALL_HEIGHT;
    const double wallLong = Z * H, wallEnd = X * H;   // -X/+X walls, -Z/+Z walls

    // Windows (-X wall): rectangle from the sill to the springing line plus the round arch.
    const double winRect = WINDOW_WIDTH * (hall::WINDOW_SPRING_Y - WINDOW_SILL_Y);
    const double winArch = 0.5 * kPi * (0.5 * WINDOW_WIDTH) * (0.5 * WINDOW_WIDTH);
    const double windows = WINDOW_COUNT * (winRect + winArch);
    // Six curtain panels beside the windows: hall_windows.cpp, each drape's footprint on the wall
    // integrated from eA/eB (3.74 m2, of which 0.31 m2 below DADO_TOP covers the wainscot). The
    // row's "draped to half area" coefficient is per m2 of this footprint (see kCurtain).
    const double curtainFoot = 6.0 * 3.744, curtainOnDado = 6.0 * 0.311;
    const double curtains = curtainFoot - curtainOnDado;   // above the wainscot, replaces limestone
    // Wainscot up to DADO_TOP on every wall (the door opening takes its part of the -Z wall).
    const double dadoLong = Z * hall::DADO_TOP, dadoEnd = X * hall::DADO_TOP;
    const double dadoMinZ = (X - hall::DOOR_WIDTH) * hall::DADO_TOP;
    // Tapestries (hall_decor.cpp buildTapestries): three on +X, two narrow ones on -Z, one on +Z.
    const double tapMaxX = 3.0 * hall::TAPESTRY_W * hall::TAPESTRY_H;
    const double tapMinZ = 2.0 * hall::TAPESTRY_NARROW_W * hall::TAPESTRY_H;
    const double tapMaxZ = 1.0 * hall::TAPESTRY_W * hall::TAPESTRY_H;
    const double door = hall::DOOR_WIDTH * hall::DOOR_HEIGHT;

    Patch patches[16];
    int n = 0;
    auto add = [&](Boundary b, double area, const Material& m) { patches[n++] = {b, area, &m}; };
    // -X (window wall): glass, curtains, wainscot, limestone for the rest.
    add(kMinX, windows, kGlass);
    add(kMinX, curtainFoot, kCurtain);
    add(kMinX, dadoLong - curtainOnDado, kPanelling);
    add(kMinX, wallLong - windows - curtains - dadoLong, kStone);
    // +X: three tapestries, wainscot, limestone.
    add(kMaxX, tapMaxX, kTapestry);
    add(kMaxX, dadoLong, kPanelling);
    add(kMaxX, wallLong - tapMaxX - dadoLong, kStone);
    // -Z: the door, two tapestries, wainscot, limestone.
    add(kMinZ, door, kDoor);
    add(kMinZ, tapMinZ, kTapestry);
    add(kMinZ, dadoMinZ, kPanelling);
    add(kMinZ, wallEnd - door - tapMinZ - dadoMinZ, kStone);
    // +Z: one tapestry, wainscot, limestone.
    add(kMaxZ, tapMaxZ, kTapestry);
    add(kMaxZ, dadoEnd, kPanelling);
    add(kMaxZ, wallEnd - tapMaxZ - dadoEnd, kStone);
    add(kFloor, X * Z, kMarble);
    add(kCeil, X * Z, kCeiling);

    // Equivalent absorption of what stands in the hall: table (top, underside shadowed: 1.5 m2 of
    // wood), the two chairs (0.5 x 0.5 m seat footprint, upholstered, unoccupied; furniture.cpp
    // buildChair also has an oval 0.205 x 0.25 m velvet back cushion, both faces exposed:
    // 2 x pi x 0.205 x 0.25 = 0.32 m2 per chair, same row, a thin pad so slightly generous) and the
    // two robots (about 1.5 m2 of hard shell each). The hall has no rug or carpet (hall_floor.cpp is
    // marble and inlay only) and no other fabric (checked: only Tapestry, Curtain, ChairVelvet).
    const Absorber objects[] = {{"table", 1, 1.5, &kWood}, {"chair", 2, 0.25, &kUpholstery},
                                {"chair back", 2, 0.32, &kUpholstery}, {"robot", 2, 1.5, &kShell}};

    HallData d;
    RoomAcoustics& r = d.room;
    r.volume = float(X * Z * H);
    double surface = 0.0;
    double sa[kRoomBands] = {};
    double bArea[kBoundaries] = {};
    double bSa[kBoundaries][kRoomBands] = {};
    for (int i = 0; i < n; ++i) {
        surface += patches[i].area;
        bArea[patches[i].boundary] += patches[i].area;
        for (int b = 0; b < kRoomBands; ++b) {
            sa[b] += patches[i].area * patches[i].material->alpha[size_t(b)];
            bSa[patches[i].boundary][b] += patches[i].area * patches[i].material->alpha[size_t(b)];
        }
    }
    for (int b = 0; b < kBoundaries; ++b)
        for (int k = 0; k < kRoomBands; ++k) d.boundaryAlpha[size_t(b)][size_t(k)] = float(bSa[b][k] / bArea[b]);
    for (const Absorber& o : objects)
        for (int b = 0; b < kRoomBands; ++b) sa[b] += o.count * o.area * o.material->alpha[size_t(b)];
    r.surface = float(surface);
    r.meanFreePath = float(4.0 * double(r.volume) / surface);
    for (int b = 0; b < kRoomBands; ++b) {
        const double V = r.volume, m = kAirM[size_t(b)];
        const double a = sa[b] / surface;
        const double air = 4.0 * m * V;
        r.airM[size_t(b)] = kAirM[size_t(b)];
        r.meanAlpha[size_t(b)] = float(a);
        r.rt60[size_t(b)] = float(0.161 * V / (-surface * std::log(1.0 - a) + air));
        r.roomConstant[size_t(b)] = float((surface * a + air) / (1.0 - a));
    }
    return d;
}

const HallData& hallData() {
    static const HallData d = computeHall();
    return d;
}

// ---- Early reflections ------------------------------------------------------------------------
constexpr float kTableHalfX = layout::TABLE_WIDTH * 0.5f, kTableHalfZ = layout::TABLE_DEPTH * 0.5f;
constexpr int kBand1k = 3, kBand4k = 5;

// True when the segment a-b passes through the table top (the rectangle at TABLE_TOP_Y).
bool crossesTable(m::vec3 a, m::vec3 b) {
    const float da = a.y - layout::TABLE_TOP_Y, db = b.y - layout::TABLE_TOP_Y;
    if (!(da * db < 0.0f)) return false;
    const float t = da / (da - db);
    const float x = a.x + (b.x - a.x) * t, z = a.z + (b.z - a.z) * t;
    return std::fabs(x) <= kTableHalfX && std::fabs(z) <= kTableHalfZ;
}

bool finite3(m::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Tracer {
    m::vec3 src, lis, face;
    bool directional;
    float direct;
    const HallData& hall;
    Reflection out[64];
    int count = 0;

    Tracer(m::vec3 s, m::vec3 l, m::vec3 f, const HallData& h)
        : src(s), lis(l), face(m::normalize(f)), directional(m::length2(f) > 1e-12f), direct(m::length(l - s)), hall(h) {}

    float dirGain(m::vec3 to) const {   // talker directivity towards the unit vector 'to'
        return talkerDirectivity(m::dot(face, to));
    }
    // 'img' = the final image, 'firstLeg' = the direction the sound leaves the source in, 'beta1k/4k'
    // the product of the surfaces' reflection factors.
    void emit(m::vec3 img, m::vec3 firstLeg, float beta1k, float beta4k, int order, bool table = false) {
        const float path = m::length(img - lis);
        if (!(path > 1e-6f) || !(path > direct)) return;
        float dirRatio = 1.0f;
        if (directional) dirRatio = dirGain(m::normalize(firstLeg)) / dirGain(m::normalize(lis - src));
        const float spread = direct / path, extra = path - direct;
        Reflection& r = out[count++];
        r.delay = extra / kSpeedOfSound;
        r.gain = spread * beta1k * std::exp(-0.5f * hall.room.airM[kBand1k] * extra) * dirRatio;
        r.hfGain = spread * beta4k * std::exp(-0.5f * hall.room.airM[kBand4k] * extra) * dirRatio;
        r.dir = m::normalize(img - lis);
        r.order = order;
        r.table = table;
    }

    // One wall sequence of 'n' boundaries: the image, the real path back through the reflection
    // points (to test the table against), the factors.
    void wallPath(const int* seq, int n) {
        m::vec3 img[3];
        img[0] = src;
        for (int k = 1; k <= n; ++k) {
            img[k] = img[k - 1];
            const Plane& p = kPlanes[seq[k - 1]];
            img[k][p.axis] = 2.0f * p.pos - img[k][p.axis];
        }
        m::vec3 pts[3];
        m::vec3 cur = lis;
        bool usesFloor = false;
        for (int k = n; k >= 1; --k) {
            const Plane& p = kPlanes[seq[k - 1]];
            const m::vec3 d = img[k] - cur;
            if (!(std::fabs(d[p.axis]) > 1e-9f)) return;
            pts[k] = cur + d * ((p.pos - cur[p.axis]) / d[p.axis]);
            cur = pts[k];
            usesFloor |= seq[k - 1] == kFloor;
        }
        if (usesFloor) {   // the table shades a floor bounce on either side of the floor point
            m::vec3 prev = lis;
            for (int k = n; k >= 1; --k) {
                if (crossesTable(prev, pts[k])) return;
                prev = pts[k];
            }
            if (crossesTable(prev, src)) return;
        }
        float b1 = 1.0f, b4 = 1.0f;
        for (int k = 0; k < n; ++k) {
            b1 *= std::sqrt(std::max(0.0f, 1.0f - hall.boundaryAlpha[size_t(seq[k])][kBand1k]));
            b4 *= std::sqrt(std::max(0.0f, 1.0f - hall.boundaryAlpha[size_t(seq[k])][kBand4k]));
        }
        emit(img[n], pts[1] - src, b1, b4, n);
    }

    // The table top as a mirror: the source above it, the specular point on the rectangle.
    void tablePath() {
        const float ty = layout::TABLE_TOP_Y;
        if (!(src.y > ty && lis.y > ty)) return;
        const m::vec3 img(src.x, 2.0f * ty - src.y, src.z);
        const float t = (ty - img.y) / (lis.y - img.y);
        const m::vec3 p = img + (lis - img) * t;
        if (std::fabs(p.x) > kTableHalfX || std::fabs(p.z) > kTableHalfZ) return;
        const float a1 = 1.0f - kWood.alpha[kBand1k], a4 = 1.0f - kWood.alpha[kBand4k];
        emit(img, p - src, std::sqrt(a1), std::sqrt(a4), 1, true);
    }
};

}  // namespace

const RoomAcoustics& hallAcoustics() { return hallData().room; }

float diffuseToDirect(float r, float q, int band) {
    band = std::clamp(band, 0, kRoomBands - 1);
    return 16.0f * m::PI * r * r / (q * hallAcoustics().roomConstant[size_t(band)]);
}

float criticalDistance(float q, int band) {
    band = std::clamp(band, 0, kRoomBands - 1);
    return std::sqrt(q * hallAcoustics().roomConstant[size_t(band)] / (16.0f * m::PI));
}

ReflectionSet earlyReflections(m::vec3 source, m::vec3 listener, m::vec3 facing) {
    ReflectionSet set;
    if (!finite3(source) || !finite3(listener) || !finite3(facing)) return set;
    Tracer t(source, listener, facing, hallData());
    set.directDistance = t.direct;
    for (int a = 0; a < kBoundaries; ++a) {
        const int one[1] = {a};
        t.wallPath(one, 1);
    }
    // Second order: two parallel walls in either order (two images), two perpendicular walls once
    // (the same image whichever is hit first).
    for (int a = 0; a < kBoundaries; ++a)
        for (int b = 0; b < kBoundaries; ++b) {
            if (a == b) continue;
            if (kPlanes[a].axis != kPlanes[b].axis && a > b) continue;
            const int two[2] = {a, b};
            t.wallPath(two, 2);
        }
    t.tablePath();

    Reflection* r = t.out;
    int n = t.count;
    if (n > kMaxReflections) {   // the strongest stay
        std::nth_element(r, r + kMaxReflections, r + n, [](const Reflection& x, const Reflection& y) { return x.gain > y.gain; });
        n = kMaxReflections;
    }
    std::sort(r, r + n, [](const Reflection& x, const Reflection& y) { return x.delay < y.delay; });
    set.count = n;
    for (int i = 0; i < n; ++i) set.r[size_t(i)] = r[i];
    return set;
}

}  // namespace audio
