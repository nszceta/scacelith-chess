// The [coach] and [tts] sections of Scacelith.ini (see settings.h). Engine-free and GL-free: in the
// core library, so that tests/coach_scene_tests.cpp checks their round trip.
#include "settings.h"
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace game {

// A game of the history: "level:result:accuracy:theme:blunders:markers" ("-" for an unknown accuracy,
// no theme, unknown blunders; markers 0 / 1). Earlier versions wrote the first three fields only:
// the others then read as unknown / none / no markers.
std::string encodeCoachHistory(const std::vector<coach::GameRecord>& games) {
    std::string out;
    size_t first = games.size() > size_t(Settings::kCoachHistoryMax) ? games.size() - size_t(Settings::kCoachHistoryMax) : 0;
    for (size_t i = first; i < games.size(); ++i) {
        const coach::GameRecord& g = games[i];
        char buf[64];
        if (g.accuracy >= 0.0 && std::isfinite(g.accuracy))
            std::snprintf(buf, sizeof(buf), "%d:%d:%.1f", g.level, g.result, g.accuracy);
        else
            std::snprintf(buf, sizeof(buf), "%d:%d:-", g.level, g.result);
        if (!out.empty()) out += ' ';
        out += buf;
        out += ':';
        out += g.theme.empty() ? std::string("-") : g.theme;
        out += g.blunders >= 0 ? ':' + std::to_string(g.blunders) : std::string(":-");
        out += g.markers ? ":1" : ":0";
    }
    return out;
}

std::vector<coach::GameRecord> decodeCoachHistory(const std::string& text) {
    std::vector<coach::GameRecord> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find(' ', i);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(i, end - i);
        i = end + 1;
        if (entry.empty()) continue;
        std::vector<std::string> field;
        for (size_t from = 0;;) {
            const size_t colon = entry.find(':', from);
            field.push_back(entry.substr(from, colon == std::string::npos ? std::string::npos : colon - from));
            if (colon == std::string::npos) break;
            from = colon + 1;
        }
        if (field.size() < 3) continue;  // malformed: skipped
        coach::GameRecord g;
        g.level = std::clamp(std::atoi(field[0].c_str()), 0, 6);
        g.result = std::clamp(std::atoi(field[1].c_str()), -1, 1);
        const std::string& acc = field[2];
        const double a = std::atof(acc.c_str());  // "nan" would pass the clamp: unknown, as "-"
        g.accuracy = acc.empty() || acc == "-" || std::isnan(a) ? -1.0 : std::clamp(a, 0.0, 100.0);
        if (field.size() > 3 && field[3] != "-" &&
            std::all_of(field[3].begin(), field[3].end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; }))
            g.theme = field[3];
        if (field.size() > 4 && !field[4].empty() && field[4] != "-") g.blunders = std::clamp(std::atoi(field[4].c_str()), 0, 99);
        g.markers = field.size() > 5 && field[5] == "1";
        out.push_back(g);
    }
    if (out.size() > size_t(Settings::kCoachHistoryMax)) out.erase(out.begin(), out.end() - Settings::kCoachHistoryMax);
    return out;
}

std::string encodeChallengeIds(const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) {
        if (id.empty()) continue;
        if (!out.empty()) out += ',';
        out += id;
    }
    return out;
}

std::vector<std::string> decodeChallengeIds(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t end = text.find(',', i);
        if (end == std::string::npos) end = text.size();
        std::string id = text.substr(i, end - i);
        i = end + 1;
        size_t a = id.find_first_not_of(" \t"), b = id.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        id = id.substr(a, b - a + 1);
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

// The chapter saved as an index by earlier versions (coach.lesson_chapter): the lesson then had
// these ten chapters, in this order. "" = from the start.
static std::string legacyLessonChapterId(int index) {
    static const char* const kIds[] = {"welcome", "pieces", "safety", "check", "mate",
                                       "castling", "en_passant", "promotion", "draws", "etiquette"};
    return index > 0 && index < int(sizeof kIds / sizeof *kIds) ? kIds[index] : "";
}

// A chapter id saved by an earlier version, as the lesson names it now: "pieces" was split in
// three, and a player who left inside it resumes at the first of them. Other ids are kept (an id
// the lesson no longer knows starts it over, see GameScene's lessonChapterIndex).
static std::string currentLessonChapterId(const std::string& id) {
    return id == "pieces" ? "pieces_lines" : id;
}

void readCoachSettings(const IniFile& ini, Settings& s) {
    s.coachLevel = std::max(0, ini.getInt("coach.level", s.coachLevel));  // the UI clamps to its list
    s.coachColour = std::clamp(ini.getInt("coach.colour", s.coachColour), 0, 2);
    s.coachNextColour = std::clamp(ini.getInt("coach.next_colour", s.coachNextColour), 0, 1);
    s.coachRulesDone = ini.getBool("coach.rules_done", s.coachRulesDone);
    if (ini.has("coach.history")) s.coachHistory = decodeCoachHistory(ini.getString("coach.history"));
    s.coachAccuracyExplained = ini.getBool("coach.accuracy_explained", s.coachAccuracyExplained);
    s.coachTakeYourTime = ini.getBool("coach.take_your_time", s.coachTakeYourTime);
    if (ini.has("coach.danger_markers_mode")) {
        std::string mode = ini.getString("coach.danger_markers_mode");
        for (char& c : mode) c = char(std::tolower((unsigned char)c));
        s.coachDangerMarkerMode = mode == "on"    ? Settings::DangerMarkersOn
                                  : mode == "off" ? Settings::DangerMarkersOff
                                                  : Settings::DangerMarkersAuto;
    } else if (ini.has("coach.danger_markers")) {   // the on / off switch of earlier versions
        s.coachDangerMarkerMode = ini.getBool("coach.danger_markers", true) ? Settings::DangerMarkersAuto : Settings::DangerMarkersOff;
    }
    if (ini.has("coach.lesson_chapter_id")) s.coachLessonChapter = currentLessonChapterId(ini.getString("coach.lesson_chapter_id"));
    else if (ini.has("coach.lesson_chapter"))
        s.coachLessonChapter = currentLessonChapterId(legacyLessonChapterId(ini.getInt("coach.lesson_chapter", 0)));
    s.coachTab = std::clamp(ini.getInt("coach.tab", s.coachTab), 0, 1);
    s.coachChallenge = ini.getString("coach.challenge", s.coachChallenge);
    if (ini.has("coach.challenges_done")) s.coachChallengesDone = decodeChallengeIds(ini.getString("coach.challenges_done"));
    s.ttsThreads = std::clamp(ini.getInt("tts.threads", s.ttsThreads), 0, 16);
    s.ttsVoice = std::max(-1, ini.getInt("tts.voice", s.ttsVoice));
    s.ttsSteps = std::clamp(ini.getInt("tts.steps", s.ttsSteps), Settings::kTtsStepsMin, Settings::kTtsStepsMax);
    s.ttsArch = ini.getString("tts.arch", s.ttsArch);
    if (s.ttsArch.empty()) s.ttsArch = "auto";
    // The speeds, in the slider's range (a hand-edited "nan" keeps the default).
    auto speed = [&](const char* key, float def) {
        const float v = ini.getFloat(key, def);
        return std::isfinite(v) ? std::clamp(v, Settings::kTtsSpeedMin, Settings::kTtsSpeedMax) : def;
    };
    s.ttsSpeedLesson = speed("tts.speed_lesson", s.ttsSpeedLesson);
    s.ttsSpeedBeginner = speed("tts.speed_beginner", s.ttsSpeedBeginner);
    s.ttsSpeed = speed("tts.speed", s.ttsSpeed);
    s.coachVoice = ini.getBool("coach.voice", s.coachVoice);  // the voice model download (W12)
    s.coachVoiceOffered = ini.getBool("coach.voice_offered", s.coachVoiceOffered);
    s.coachVoiceUpdateOffered = ini.getBool("coach.voice_update_offered", s.coachVoiceUpdateOffered);
    s.analysisComments = ini.getBool("analysis.comments", s.analysisComments);
    s.analysisVoice = ini.getBool("analysis.voice", s.analysisVoice);
    s.analysisArrows = ini.getBool("analysis.arrows", s.analysisArrows);
}

void writeCoachSettings(IniFile& ini, const Settings& s) {
    ini.setInt("coach.level", s.coachLevel);
    ini.setInt("coach.colour", s.coachColour);
    ini.setInt("coach.next_colour", s.coachNextColour);
    ini.setBool("coach.rules_done", s.coachRulesDone);
    ini.set("coach.history", encodeCoachHistory(s.coachHistory));
    ini.setBool("coach.accuracy_explained", s.coachAccuracyExplained);
    ini.setBool("coach.take_your_time", s.coachTakeYourTime);
    ini.set("coach.danger_markers_mode", s.coachDangerMarkerMode == Settings::DangerMarkersOn    ? "on"
                                         : s.coachDangerMarkerMode == Settings::DangerMarkersOff ? "off"
                                                                                                 : "auto");
    ini.set("coach.lesson_chapter_id", s.coachLessonChapter);
    ini.setInt("coach.tab", s.coachTab);
    ini.set("coach.challenge", s.coachChallenge);
    ini.set("coach.challenges_done", encodeChallengeIds(s.coachChallengesDone));
    ini.setInt("tts.threads", s.ttsThreads);
    ini.setInt("tts.voice", s.ttsVoice);
    ini.setInt("tts.steps", s.ttsSteps);
    ini.set("tts.arch", s.ttsArch);
    ini.setFloat("tts.speed_lesson", s.ttsSpeedLesson);
    ini.setFloat("tts.speed_beginner", s.ttsSpeedBeginner);
    ini.setFloat("tts.speed", s.ttsSpeed);
    ini.setBool("coach.voice", s.coachVoice);  // the voice model download (W12)
    ini.setBool("coach.voice_offered", s.coachVoiceOffered);
    ini.setBool("coach.voice_update_offered", s.coachVoiceUpdateOffered);
    ini.setBool("analysis.comments", s.analysisComments);
    ini.setBool("analysis.voice", s.analysisVoice);
    ini.setBool("analysis.arrows", s.analysisArrows);
}

}  // namespace game
