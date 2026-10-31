// What the coach remembers of one finished Coach game (levels 1-6). Kept apart from appraisal.h
// so that game::Settings can store the history without pulling in the analysis headers; the
// functions that read it (suggestLevel, levelMastered, ...) are in coach/appraisal.h.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace coach {

struct GameRecord {
    int level = 1;
    int result = 0;          // +1 the human won, 0 draw, -1 lost
    double accuracy = -1.0;  // the human's game accuracy, -1 unknown
    // The theme the appraisal named aloud as the one improvement (exTypeName: "fork", "hanging",
    // ...), "" when it named none. The next greetings, the 'again' wording of the appraisal and the
    // Coach page's recommended challenge read it.
    std::string theme;
    // The human's blunders at their first try (a taken-back move counts as it was first played),
    // -1 unknown (a game saved by an earlier version).
    int blunders = -1;
    // The danger markers were shown on the board in that game (an earlier version's games: no).
    bool markers = false;
};

// The danger markers' Auto setting (Settings::coachDangerMarkerMode) for a game at 'level', given
// the finished games: shown at level 1 for the player's first three games at that level, at level
// 2 for the first one, never from level 3 on. The scaffold is withdrawn as the player learns to
// scan the board unaided.
inline bool dangerMarkersAuto(int level, const std::vector<GameRecord>& games) {
    if (level != 1 && level != 2) return false;
    const auto played = std::count_if(games.begin(), games.end(), [&](const GameRecord& g) { return g.level == level; });
    return played < (level == 1 ? 3 : 1);
}

}  // namespace coach
