// A Coach-mode game (levels 1-6) or the rules lesson (level 0), seen from the coach: what to say
// and when. The session owns the director and the coach's brain (Reviewer, Appraisal,
// OpeningAnnouncer, Lesson, the event lines) and follows the flows documented in the headers of
// coach/review.h, coach/appraisal.h, coach/openings.h, coach/lesson.h and coach/events.h:
//   - game start: greeting (and the level's introduction), opening names as they are identified;
//   - the human's turn: analysis A0 (and A3 at levels 3-4), background evaluations when idle;
//   - after every move: "Check!" / "Checkmate!"; after the human's move: A1/A2 when needed, the
//     review (verdict, explanation, demonstration, rewind, takeback offer, praise); after the
//     coach's move: its remarks and threat warnings;
//   - the takeback offer's answer, the player's takeback requests, draw answers;
//   - game end: closing words, the handshake (played by the scene), the appraisal (skippable);
//   - level 0: the lesson's chapters, WaitMove judging, illegal-move explanations, idle hints;
//   - a challenge (SessionConfig::challenge): its positions one after the other, the moves judged,
//     hints on request (coach/challenge_run.h), illegal-move explanations as in the lesson.
// Engine-free and GL-free: the world is reached through coach::Stage and coach::Analyst.
#pragma once
#include "appraisal.h"
#include "director.h"
#include "stage.h"
#include "../chess/chess.h"
#include <cstdint>
#include <string>
#include <vector>

namespace coach {

// The TTS speed of a session's lines (Options > Audio, Settings::ttsSpeed*): by default slower for
// new players, in the rules lesson and at levels 1-2; normal from level 3 on and in the challenges.
struct SpeechSpeeds {
    float lesson = 0.85f;     // level 0
    float beginner = 0.85f;   // levels 1-2
    float normal = 1.0f;      // levels 3-6, the challenges
    float forLevel(int level) const { return level == 0 ? lesson : level <= 2 ? beginner : normal; }
};

struct SessionConfig {
    int level = 1;                        // 0 = the rules lesson, 1..6 = the coach levels
    chess::Color human = chess::White;    // the lesson: always White
    DirectorConfig director;
    SpeechSpeeds speeds;                  // the director's speed, by level (Session::start)
    bool introduceLevel = false;          // first game at this level (or the level changed)
    bool firstAfterLesson = false;        // level 1: the first real game after the rules lesson (greeting recalls its questions)
    bool dangerMarkers = false;           // levels 1-2: the danger markers are shown in this game (GameRecord::markers)
    bool offersEnabled = true;            // takeback offers after blunders
    bool takeYourTime = true;             // "Take your time." after a minute without a move (levels 1-3)
    std::vector<GameRecord> history;      // earlier coach games, oldest first (suggestLevel)
    bool accuracyExplained = false;       // the appraisal has already explained what accuracy is
    int lessonChapter = 0;                // level 0: start at this chapter (0 = from the start)
    bool lessonChosen = false;            // ... which the player picked (the Coach page's chapter list, the command
                                          // line) rather than resumed: "Let's start with ...", not "Welcome back!"
    // A challenge (coach/challenge.h) instead of a game: its id ("" = none). The level is then
    // ignored, the human plays White, and nothing is recorded (history() is unchanged).
    std::string challenge;
    int challengePosition = 0;            // begin at this position, 0-based (tests, screenshots)
    std::string practiseTheme;            // a challenge started from a game's end card: the theme (exTypeName) it
                                          // practises, named when it begins ("" = none)
    // A retry of a game's critical position (levels 1-3, the end card's "Try that position again")
    // instead of a game: 'game' stands at that position, the human to move, and this is the engine's
    // better move there ("" = a game). The coach asks for it, with the explanation's scaffold; a move
    // that the review calls a mistake is taken back (the second miss shows the better move); the
    // session wants the handshake (handshakeWanted) when the retry is over. The coach never moves,
    // nothing is recorded (history() is unchanged).
    std::string retryBest;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // A new game (or lesson) on 'game' as it stands (the start position, or the lesson's first
    // set-up follows). Starts talking at once.
    void start(Stage& stage, Analyst& analyst, const chess::Game& game, const SessionConfig& config);
    void stop();                          // leaving: silence, cancel the coach's analyses

    // Every frame while the game runs (not while paused).
    void update(const chess::Game& game, float dt);
    void setPaused(bool paused);
    // Options > Audio changed the speeds: from the next line on (the lines already synthesised at
    // the old speed are synthesised again).
    void setSpeechSpeeds(const SpeechSpeeds& speeds);

    // ---- Events from the scene --------------------------------------------------------------------
    void onMove(const chess::Game& game);          // a move was completed (either side; lesson moves too)
    // The player touched a piece (idle timers, stale lines). While the takeback card is shown it
    // answers it: the player plays on (as onOfferAnswer(false), with "Let's play on").
    void onPlayerActive();
    // The player picked up the piece on 'from' (onPlayerActive, and the coach's open question about
    // its threat, levels 1-2: a piece that is neither one the threat is about nor one that can deal
    // with it shows the player the answer). The scene calls this instead of onPlayerActive.
    void onPieceTouched(const chess::Game& game, chess::Square from);
    // The coach has asked whether the player sees its threat and not yet shown it (levels 1-2): the
    // scene paints no danger markers meanwhile, they would answer the question.
    bool threatQuestionOpen() const;
    // A placement refused as illegal (legal-move hints on): the rules lesson explains why.
    void onIllegalAttempt(const chess::Game& game, chess::Square from, chess::Square to);
    void onOfferAnswer(const chess::Game& game, bool accept);   // the takeback card's answer
    // The pause menu's "Take back": back to the player's last move (the coach's reply included).
    bool canTakeBack(const chess::Game& game) const;
    void onTakeBackRequested(const chess::Game& game);
    void onDrawAnswer(bool accepted);              // the coach answered the player's draw offer
    // The game is over (Game::isOver(): mate, stalemate, a draw, a resignation recorded on the Game).
    void onGameOver(const chess::Game& game, bool humanResigned);
    void skip();                                   // Space
    // Every frame while the engine searches the coach's move: seconds since that search began
    // (0 when none runs). Once the move is held by the engine alone (coachMayMove() true) for 6 s,
    // the coach says a short filler (once per move).
    void onCoachThinking(float seconds);

    // ---- What the scene asks -----------------------------------------------------------------------
    // The coach may play its move now (computed or not): no review, offer, demonstration or rewind is
    // running or pending for the human's last move.
    bool coachMayMove() const;
    // The player may pick up a piece: levels 1-6, the human's turn (no takeback card shown); the
    // lesson, while an exercise waits for the move. A lesson move made otherwise is taken back.
    bool playerMayMove(const chess::Game& game) const;
    bool handshakeWanted() const;                  // the closing words are said: play the handshake
    void onHandshakeDone(const chess::Game& game); // then the appraisal (level 0: the lesson ends)
    bool finished() const;                         // everything is said: show the end-of-game menu
    bool offerOpen() const;                        // show the takeback card (Stage::showTakebackOffer too)
    // Levels 4-6 of a game: how many more of the coach's takeback offers the player may accept this
    // game (Reviewer::takebacksLeft), shown at the bottom of the screen and on the card. -1 at levels
    // 1-3, in the lesson and in a challenge (no count to show).
    int takebacksLeft() const;
    bool offerUsesTakeback() const;                // the open card, accepted, uses one of them

    // ---- Results to persist (Settings [coach]) ------------------------------------------------------
    const std::vector<GameRecord>& history() const;   // with the game just finished appended
    bool accuracyExplained() const;
    int suggestedLevel() const;                    // 0 = no suggestion
    // Levels 1-3 of a game: the challenge set (id) that practises the theme the appraisal named as
    // the one improvement; "" when it named none or no set practises it. Not persisted: the end card.
    const std::string& suggestedChallenge() const;
    // The theme (exTypeName) behind suggestedChallenge(), "" when there is none.
    const std::string& practiseTheme() const;
    int lessonChapter() const;                     // the chapter to resume at
    bool lessonCompleted() const;
    // Levels 1-3 of a game: the position the end card offers again (the one before the critical
    // blunder); valid() is false when there is none. Known once the appraisal has begun.
    const RetryPoint& retryPoint() const;
    // A retry (SessionConfig::retryBest) ended with the player's own move (the better move or a move
    // that is no mistake), not with the coach showing the better move.
    bool retryFound() const;

    // ---- Challenges ---------------------------------------------------------------------------------
    bool challengeMode() const;                    // a challenge runs (its id was found)
    int challengePosition() const;                 // the position played, 0-based
    int challengePositions() const;
    bool challengeCompleted() const;               // every position solved
    bool hintAvailable(const chess::Game& game) const;   // H would give a hint now
    void onHintRequested(const chess::Game& game);       // H
    bool offerIsHint() const;                      // the offer card is the hint offer (Yes = a hint)

    Director& director();
    const Director& director() const;

private:
    struct Impl;
    Impl* d_;
};

}  // namespace coach
