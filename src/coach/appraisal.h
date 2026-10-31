// The coach's end-of-game appraisal: one verdict per ply collected during the game (the human's
// from the review, the coach's from the neighbouring evaluations, background analyses for the
// gaps), the statistics (lichess game accuracy, phase accuracy, class counts, best streak, critical
// moment, turning point, best moment, recurring theme, takebacks, material), and the appraisal
// Script: opener, highlight, numbers, one improvement, encouragement and level suggestion. Every
// beat is skippable. Lines are catalog keys of assets/coach/speech/<lang>/appraisal.lang.
//
// Engine-free: coach::Session hands in the analyses; nothing is searched at the end of the game.
#pragma once
#include "../ai/analysis.h"
#include "../chess/chess.h"
#include "game_record.h"
#include "review.h"
#include "script.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace coach {

// ---- Lichess accuracy (AccuracyPercent.scala) -----------------------------------------------------
// Game accuracy of each side from the evaluations after every ply (White's view, centipawns, mates
// as +-1000; clamped). The position before the first ply counts as 'startCp' (lichess: +15).
// 'firstMover' made the first ply of the series. -1 for a side without a move.
struct SideAccuracy {
    double white = -1.0, black = -1.0;
    double of(chess::Color c) const { return c == chess::White ? white : black; }
};
// 'moveOverride' (optional, one per move of the series, negative = none): the accuracy a move is
// scored with instead of the one its evaluations give (a taken-back move counts as first played).
SideAccuracy gameAccuracy(const std::vector<int>& cpsWhiteAfterEachPly, chess::Color firstMover = chess::White,
                          int startCp = 15, const std::vector<double>* moveOverride = nullptr);
// The volatility weights of that accuracy: one per move of a series of W% values (White's view,
// the position before the first move first, one value per position): the standard deviation of
// the W% in the window of positions the move belongs to (a tenth of the game, 2 to 8 positions),
// clamped to 0.5..12. Fewer than two values: none. The analysis mode's summary (analysis/review.h)
// weighs its moves' accuracy with them.
std::vector<double> accuracyWeights(const std::vector<double>& winPercents);

// The accuracy players of a rating typically reach (the server's anti-cheat priors: 600 -> 65,
// 1000 -> 71, 1500 -> 79, 2000 -> 86, 2500 -> 91, linear in between), and the rating of a level.
double typicalAccuracy(int rating);
int levelRating(int level);   // 750, 1050, 1350, 1650, 1950, 2250

// Suggested level after the games so far (oldest first, the last one just finished): up after 2 wins
// in the last 3 games at this level with accuracy >= the next level's typical accuracy; down after
// 3 losses in a row at this level with accuracy below its typical accuracy; else the same level.
// Never down right after a win. Level 1 does not move up until one of its games was played without
// a first-try blunder or without the danger markers (a win with the scaffold on proves little).
// Returns 1..6.
int suggestLevel(int level, const std::vector<GameRecord>& games);
// The level's mastery goal, met by the games so far: level 1, the last two games at the level had
// no first-try blunder (no piece lost for nothing); level 2, the last two games at the level were
// won without one. False for the other levels (no goal) and when the games are too few or their
// blunders unknown.
bool levelMastered(int level, const std::vector<GameRecord>& games);
// The theme the appraisals named in two of the last three games (any level), as GameRecord::theme
// ("" = none); the most recent one when there are two. The Coach page recommends its challenge set.
std::string recurringTheme(const std::vector<GameRecord>& games);
// The ExType whose exTypeName() is 'name'; ExType::None for "" and for names this version does not know.
ExType themeFromName(const std::string& name);

// ---- Statistics ------------------------------------------------------------------------------------
enum class BestMoment : uint8_t { None, Brilliant, Great, OnlyMove, WonMaterial, Streak, GoodCapture, Phase };

struct SideStats {
    double accuracy = -1.0;                        // lichess game accuracy, -1 unknown
    std::array<double, 3> phaseAccuracy{{-1.0, -1.0, -1.0}};   // opening, middlegame, endgame (>= 6 plies of the side)
    std::array<int, 9> classes{};                  // count per MoveClass (index = int(MoveClass))
    int judged = 0, unjudged = 0;                  // judged: cls other than Unjudged
    int topMoves = 0;                              // Best + Excellent among judged non-book non-forced plies
    int captured = 0;                              // points captured (1-3-3-5-9)
    int piecesCaptured = 0;                        // coach men (pawns included) captured
    int checks = 0;
    int count(MoveClass c) const { return classes[size_t(c)]; }
};

struct AppraisalStats {
    int plies = 0, fullMoves = 0;
    chess::GameStatus status = chess::GameStatus::Ongoing;
    chess::GameEndReason reason = chess::GameEndReason::None;
    int result = 0;                                // the human's view: +1 won, 0 draw, -1 lost (or unfinished)
    SideStats human, coach;
    int humanMoves = 0;
    bool numbers = false;                          // accuracy may be voiced: >= 10 human moves, <= 20 % unjudged
    int bestStreak = 0;                            // longest run of Best / Excellent (book / forced skipped)
    int criticalPly = -1;                          // the human ply with the largest Δ >= 10 (earliest on ties)
    int turningPly = -1;                           // first human ply from W% >= 50 to < 40 (or a comeback <= 50 to > 60)
    BestMoment bestMoment = BestMoment::None;
    int bestPly = -1;                              // the best moment's ply (-1: streak / phase)
    int bestPhase = -1;                            // BestMoment::Phase: 0..2
    ExType theme = ExType::None;                   // recurring theme, else the critical moment's type
    bool themeRecurring = false;
    int offers = 0, takebacks = 0, fixed = 0;      // takeback offers, taken, replayed with Δ < 5
    // Moves taken back count as first played: a ply whose move was taken back and replayed scores as
    // the worse of the two (accuracy, firstTryBlunders); 'fixedBlunders' are the blunders taken back
    // whose replacement is no mistake.
    int firstTryBlunders = 0;                      // the human's blunders, taken-back ones included
    int fixedBlunders = 0;
    int coachHungPly = -1;                         // a coach move that hung a piece of 3+ the human did not take
    chess::PieceType coachHungType = chess::NoPiece;
    double humanWinAtEnd = -1.0;                   // the human's W% at the last evaluated position (resignation)
};

// The critical position of a game, for the end card's "Try that position again" (levels 1-3): the
// position before the human's critical blunder and the engine's better move there. ply -1: none (no
// blunder was critical, or the review had no better move to name).
struct RetryPoint {
    int ply = -1;
    std::string fen, bestUci;
    bool valid() const { return ply >= 0; }
};

// ---- The appraisal script ----------------------------------------------------------------------------
struct AppraisalContext {
    bool humanResigned = false;
    bool explainAccuracy = false;   // first appraisal with numbers: say what accuracy means (level 3)
    std::string opening;            // opening id for Arg::ofOpening ("" = none): said from level 3
    int outOfBookMove = 0;          // the move number the game left the book (level 5), 0 = unknown
    int suggestedLevel = 0;         // suggestLevel() result; 0 or the current level = no suggestion
    // GameRecord::theme of the last two finished games before this one, newest first (empty
    // entries skipped): a theme named again wins the "again" wording ("The same tip as last time").
    std::vector<std::string> recentThemes;
};

class Appraisal {
public:
    void reset(int level, chess::Color human);
    int level() const { return level_; }
    chess::Color human() const { return human_; }

    // During the game. The human's reviewed move: its verdict (and the takeback record of a retry,
    // which replaces the verdict of the taken-back move at the same ply).
    void add(const Review& r);
    // The evaluation of the position after 'plies' moves (A0 of the human's turn, or a background
    // analysis): its best line, side to move's view, as the analysis reports it.
    void addEval(size_t plies, const ai::Analysis& a);
    // After Game::undo (a takeback, or anything else that shortens the game): forget what lies past
    // the current move list. A human verdict that goes with it is kept as a first try (see stats).
    void truncate(size_t plies);
    // Positions after 1..moves().size() plies whose evaluation is still unknown, for background
    // analyses when the engine is free (the coach's moves are judged from these evaluations).
    std::vector<size_t> missingEvals(const chess::Game& g) const;
    // A background evaluation request (priority -1, MultiPV 1, depth 14, 400 ms).
    ai::AnalysisRequest evalRequest(const chess::Game& g, size_t plies) const;

    // At the end of the game (whoever won; also callable during the game for a panel).
    AppraisalStats stats(const chess::Game& g) const;
    // The coach's verdict of every ply (the human's from the review, the coach's from the
    // neighbouring evaluations); Unjudged where nothing is known.
    std::vector<PlyVerdict> verdicts(const chess::Game& g) const;
    // Levels 1-3: the position to offer again (see RetryPoint); empty at other levels.
    RetryPoint retryPoint(const chess::Game& g) const;
    // *improvement (when given): the theme the "one improvement" sentence names aloud ("One thing
    // to practise: forks"), after the sentence cap; ExType::None when it names none (keep doing,
    // a critical move said without its theme, the coach's hung piece, stalemate, a dropped sentence).
    Script script(const chess::Game& g, const AppraisalContext& ctx, ExType* improvement = nullptr) const;

private:
    int level_ = 1;
    chess::Color human_ = chess::White;
    std::vector<PlyVerdict> humanVerdicts_;   // index = ply; ply -1 = none
    std::vector<PlyVerdict> firstTries_;       // index = ply; the worst human move taken back there (ply -1 = none)
    // retryPoint() from the stats and verdicts of 'g' already computed.
    RetryPoint retryPointOf(const chess::Game& g, const AppraisalStats& st, const std::vector<PlyVerdict>& vs) const;
    std::vector<int> eval_;                    // index = plies played; White's view cp
    std::vector<uint8_t> evalKnown_;           // 0 unknown, 1 from a move's line, 2 from a root analysis
    int offers_ = 0, takebacks_ = 0, fixed_ = 0;
    void setEval(size_t plies, int cpWhite, uint8_t source);
};

// The challenge set (assets/coach/challenges/challenges.txt) that practises a theme: "fork" for
// Fork and MissedFork, "hanging" for Hanging, MissedCapture and Exchange, "escape" for King safety,
// "opening" for Opening, "kq" for Stalemate, ...; "" for a theme without one (positional, bad
// trade, material, none).
const char* practiceChallenge(ExType theme);

}  // namespace coach
