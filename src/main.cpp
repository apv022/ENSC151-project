/*
 * FocusBird - a Flappy Bird-style reaction game for an Arduino Grove kit.
 *
 * The bird stays at a fixed x position while gates scroll toward it. Each gate
 * carries a prompt (YELL or COVER THE LIGHT) that must be answered with the
 * matching sensor while the gate is near. Inputs come from the Arduino over a
 * serial port (BTN / SOUND / LIGHT lines) or from the keyboard; both are turned
 * into the same Action values so the game rules never care where input came from.
 *
 * File layout: constants -> data types -> game rules -> drawing -> input -> main().
 */

#include "serial_port.h"

#include "raylib.h"

#include <algorithm> // std::max, std::min, std::remove_if
#include <cmath> // std::fmod for the scrolling ground
#include <iostream> // Console messages for usage and serial status
#include <random> // Random gate heights and prompt types
#include <string>
#include <utility> // std::move
#include <vector>

namespace {

// ---------------------------- Window and game constants -----------------------

constexpr int ScreenWidth = 960; // Window width in pixels.
constexpr int ScreenHeight = 640; // Window height in pixels.
constexpr float GroundHeight = 72.0f; // Bottom strip reserved for the ground.
constexpr float PlayfieldBottom = ScreenHeight - GroundHeight; // Lowest y a gate column reaches.
constexpr float PlayerX = 185.0f; // Fixed horizontal bird position; gates scroll toward it.
constexpr float BirdRadius = 17.0f; // Bird collision radius and drawn body size.
constexpr float Gravity = 920.0f; // Downward acceleration in pixels per second squared.
constexpr float FlapVelocity = -350.0f; // Upward velocity applied by a flap (negative y is up).
constexpr float ObstacleWidth = 72.0f; // Width of each gate column.
constexpr float ObstacleSpacing = 540.0f; // Distance between gates; larger than the prompt lead so windows never overlap.
constexpr float PromptLeadDistance = 420.0f; // A gate's prompt becomes active within this distance of the bird.
constexpr int StartingLives = 3; // Mistakes/collisions allowed before game over.
constexpr float SerialReconnectInterval = 2.0f; // Seconds between attempts to reopen a lost Arduino port.
constexpr float RestartDelay = 1.0f; // Ignore start input briefly on the results screen so a flap doesn't skip it.

// ----------------------------- Gameplay data types -----------------------------

/** Which top-level screen is shown; drives input handling and drawing. */
enum class ScreenState {
    Title, // Menu: choose difficulty, theme, and bird color.
    Playing, // A run is in progress.
    GameOver // Results card after the last life is lost.
};

/** One player input, already translated from either the keyboard or the Arduino. */
enum class Action {
    Start, // Enter: start a run from a menu.
    Button, // Grove button or Space/Up: start from a menu, flap during play; never answers a prompt.
    Sound, // Sound sensor event, or Y on the keyboard.
    Light // Covered-light-sensor event, or C on the keyboard.
};

/** The response a gate asks for. */
enum class PromptType {
    Yell, // Answered by Action::Sound.
    Cover // Answered by Action::Light.
};

/** Progress of a gate's prompt. */
enum class PromptResult {
    Waiting, // Not yet answered; may still be active.
    Correct, // Matching action arrived inside the prompt window.
    Failed // Wrong action, expired, or the gate was hit.
};

/** Tuning values that differ between Easy and Challenge. */
struct DifficultySettings {
    const char* label; // Name shown on the title screen.
    float obstacleSpeed; // Gate travel speed in pixels per second.
    float gapHalfHeight; // Half of the safe opening height in pixels.
};

constexpr DifficultySettings Easy{ "Easy", 220.0f, 112.0f }; // Slower gates, wider openings.
constexpr DifficultySettings Challenge{ "Challenge", 285.0f, 94.0f }; // Faster gates, narrower openings.

/** A bird color choice: the label shown on the menu and the color drawn. */
struct NamedColor {
    const char* name; // Label shown on the title screen.
    Color color; // Bird body color.
};

// Bird colors cycled with the B key; the index is stored in Game::birdColorIndex.
const NamedColor BirdColors[] = {
    { "GOLD", GOLD }, { "BLUE", Color{ 30, 75, 180, 255 } }, { "PINK", PINK },
    { "PURPLE", VIOLET }, { "GREEN", LIME },
};
constexpr int BirdColorCount = sizeof(BirdColors) / sizeof(BirdColors[0]); // Number of entries above.

// Background themes cycled with the T key; the index matches DrawBackground's switch cases.
const char* const ThemeNames[] = { "MEADOW", "SUNSET", "NIGHT" };
constexpr int ThemeCount = sizeof(ThemeNames) / sizeof(ThemeNames[0]); // Number of entries above.

/** One gate: a top and bottom column with a gap, plus its prompt. */
struct Obstacle {
    float x; // Left edge in screen coordinates.
    float gapCenter; // Vertical center of the opening.
    PromptType prompt; // Action this gate asks for.
    PromptResult result = PromptResult::Waiting; // Answer state; controls color and scoring.
    float promptStartedAt = -1.0f; // Game time the prompt became active; negative until then.
    bool passed = false; // Score is awarded at most once.
    bool collisionHandled = false; // A life is lost at most once per gate.
};

/** All game state in one place, so a new run is just a fresh Game (see StartGame). */
struct Game {
    // Player choices: kept across runs.
    DifficultySettings difficulty = Easy; // Selected with 1/2 on the title screen.
    int themeIndex = 0; // Index into ThemeNames.
    int birdColorIndex = 0; // Index into BirdColors.

    // Per-run state: reset by StartGame.
    ScreenState screen = ScreenState::Title; // Current screen.
    float birdY = ScreenHeight * 0.48f; // Vertical bird center.
    float birdVelocity = 0.0f; // Positive values move downward.
    float elapsedSeconds = 0.0f; // Run duration for the summary; also the clock for reaction times.
    float gameOverSeconds = 0.0f; // Time spent on the results screen (for RestartDelay).
    float boundaryDamageCooldown = 0.0f; // Prevents frame-by-frame life loss on the floor/ceiling.
    float groundOffset = 0.0f; // Scrolling ground animation offset.
    int score = 0; // Gates passed.
    int lives = StartingLives; // Lives left; the run ends at zero.
    int correctResponses = 0; // Prompts answered correctly.
    int incorrectResponses = 0; // Wrong sensor used during a prompt.
    int missedPrompts = 0; // Prompts that expired unanswered.
    float reactionTimeTotal = 0.0f; // Sum of correct response times (count = correctResponses).
    float fastestReactionTime = 0.0f; // Fastest correct response.
    std::string feedbackText; // Short message shown over gameplay.
    Color feedbackColor = MAROON; // Green for praise, maroon for mistakes.
    float feedbackTimer = 0.0f; // Seconds the message stays visible.
    std::vector<Obstacle> obstacles; // Gates on screen, oldest (leftmost) first.
    std::mt19937 random{ std::random_device{}() }; // Random source for gate heights and prompts.
};

// ------------------------------- Game rules -----------------------------------

/**
 * @brief Compute the rectangles of a gate's solid top and bottom columns.
 * Shared by collision and drawing so they can never disagree.
 * @param obstacle Gate to measure.
 * @param gapHalfHeight Half the opening height for the current difficulty.
 * @param top Output: column from the ceiling down to the gap.
 * @param bottom Output: column from the gap down to the ground.
 */
void GateColumns(const Obstacle& obstacle, float gapHalfHeight, Rectangle& top, Rectangle& bottom) {
    const float gapTop = obstacle.gapCenter - gapHalfHeight; // Upper edge of the opening.
    const float gapBottom = obstacle.gapCenter + gapHalfHeight; // Lower edge of the opening.
    top = { obstacle.x, 0.0f, ObstacleWidth, gapTop };
    bottom = { obstacle.x, gapBottom, ObstacleWidth, PlayfieldBottom - gapBottom };
}

/**
 * @brief Check whether a gate's prompt can be answered right now.
 * @param obstacle Gate to check.
 * @return true while the prompt is unanswered, the gate is within PromptLeadDistance
 *         of the bird, and the gate has not yet passed the bird.
 */
bool IsPromptActive(const Obstacle& obstacle) {
    return obstacle.result == PromptResult::Waiting
        && obstacle.x <= PlayerX + PromptLeadDistance // Close enough to the bird...
        && obstacle.x + ObstacleWidth >= PlayerX - BirdRadius; // ...and not yet behind it.
}

/**
 * @brief Find the single gate whose prompt is currently answerable.
 * Searching oldest-first means only the next gate can be answered.
 * @param game Current game state.
 * @return Pointer to that gate, or nullptr when no prompt is open.
 */
Obstacle* FindActivePrompt(Game& game) {
    for (Obstacle& obstacle : game.obstacles) {
        if (IsPromptActive(obstacle)) {
            return &obstacle;
        }
    }
    return nullptr;
}

/**
 * @brief Mean time of correct responses this run.
 * @param game Current game state.
 * @return Seconds, or zero before the first correct response.
 */
float AverageReactionTime(const Game& game) {
    return game.correctResponses > 0 ? game.reactionTimeTotal / game.correctResponses : 0.0f;
}

/**
 * @brief Show a short message over gameplay for a few seconds.
 * @param game Game state that stores the message.
 * @param text Message to display.
 * @param color Text color (green for praise, maroon for mistakes).
 * @param seconds How long the message stays visible.
 */
void ShowFeedback(Game& game, const char* text, Color color, float seconds) {
    game.feedbackText = text;
    game.feedbackColor = color;
    game.feedbackTimer = seconds;
}

/**
 * @brief Begin a new run, resetting all statistics while keeping the player's
 *        difficulty, theme, and bird color choices.
 * @param game Game state to reset.
 */
void StartGame(Game& game) {
    Game fresh; // Default member values describe a brand-new run.
    fresh.difficulty = game.difficulty; // Carry over the menu choices...
    fresh.themeIndex = game.themeIndex;
    fresh.birdColorIndex = game.birdColorIndex;
    fresh.random = game.random; // ...and keep the existing random sequence going.
    fresh.screen = ScreenState::Playing;
    game = std::move(fresh);
}

/**
 * @brief Spend one life and show why; losing the last life ends the run.
 * @param game Game state to update.
 * @param message Child-friendly reason shown to the player.
 */
void LoseLife(Game& game, const char* message) {
    if (game.screen != ScreenState::Playing) {
        return; // Statistics are frozen once the run has ended.
    }
    game.lives -= 1;
    ShowFeedback(game, message, MAROON, 1.35f);
    if (game.lives <= 0) {
        game.screen = ScreenState::GameOver;
        game.gameOverSeconds = 0.0f; // Start the RestartDelay countdown.
    }
}

/**
 * @brief Apply one keyboard or Arduino action to the game.
 * On menus, Start/Button begin a run. During play, Button flaps and Sound/Light
 * answer the currently open prompt (if any).
 * @param game Game state to update.
 * @param action The input to apply.
 * @param serial Used for buzzer feedback; writes are ignored when it is closed.
 */
void HandleAction(Game& game, Action action, SerialPort& serial) {
    // Menus: only starting a run is possible.
    if (game.screen != ScreenState::Playing) {
        const bool wantsStart = action == Action::Start || action == Action::Button;
        const bool canStart = game.screen == ScreenState::Title || game.gameOverSeconds >= RestartDelay;
        if (wantsStart && canStart) {
            StartGame(game);
        }
        return;
    }

    if (action == Action::Start) {
        return; // Enter does nothing during play.
    }
    if (action == Action::Button) {
        game.birdVelocity = FlapVelocity; // The button is flight control, never a prompt answer.
        return;
    }

    // Sound or Light: only counts if a prompt is open right now.
    Obstacle* active = FindActivePrompt(game);
    if (active == nullptr) {
        return; // Early/late sensor events cannot count for a future prompt.
    }
    const bool matches = (active->prompt == PromptType::Yell && action == Action::Sound)
        || (active->prompt == PromptType::Cover && action == Action::Light);
    if (matches) {
        active->result = PromptResult::Correct; // Turns the gate green and closes the prompt.
        const float reactionTime = std::max(0.0f, game.elapsedSeconds - active->promptStartedAt);
        if (game.correctResponses == 0 || reactionTime < game.fastestReactionTime) {
            game.fastestReactionTime = reactionTime; // First answer, or a new best.
        }
        game.correctResponses += 1;
        game.reactionTimeTotal += reactionTime;
        ShowFeedback(game, "GREAT JOB!", DARKGREEN, 1.0f);
        serial.WriteLine("BUZZER 1350 100"); // High tone: success.
    } else {
        active->result = PromptResult::Failed; // The first wrong response closes this prompt.
        game.incorrectResponses += 1;
        LoseLife(game, "NOT THAT ONE - TRY THE NEXT!");
        serial.WriteLine("BUZZER 380 150"); // Low tone: wrong action.
    }
}

/**
 * @brief Add a new gate just off the right edge with a random gap height and prompt.
 * @param game Game state that receives the gate and supplies the random source.
 */
void SpawnObstacle(Game& game) {
    const float margin = game.difficulty.gapHalfHeight + 58.0f; // Keep the opening away from ceiling and ground.
    std::uniform_real_distribution<float> gapDistribution(margin, PlayfieldBottom - margin);
    std::uniform_int_distribution<int> promptDistribution(0, 1); // 0 = Yell, 1 = Cover.
    Obstacle obstacle{};
    obstacle.x = ScreenWidth + 24.0f; // Start just off-screen so it slides in.
    obstacle.gapCenter = gapDistribution(game.random);
    obstacle.prompt = promptDistribution(game.random) == 0 ? PromptType::Yell : PromptType::Cover;
    game.obstacles.push_back(obstacle);
}

/**
 * @brief Keep the bird inside the playfield and penalize touching the floor or
 *        ceiling at most once per second.
 * @param game Game state to update.
 * @param clampedY Bird y position at the boundary that was touched.
 * @param message Reason shown if a life is lost.
 */
void HitBoundary(Game& game, float clampedY, const char* message) {
    game.birdY = clampedY; // Don't let the bird leave the screen.
    game.birdVelocity = 0.0f;
    if (game.boundaryDamageCooldown <= 0.0f) {
        LoseLife(game, message);
        game.boundaryDamageCooldown = 1.0f;
    }
}

/**
 * @brief Advance physics, prompts, gates, scoring, and collisions by one frame.
 * @param game Game state to update.
 * @param frameTime Seconds since the last frame (already clamped by the caller).
 * @param serial Used for the prompt-start buzzer cue.
 */
void UpdateGame(Game& game, float frameTime, SerialPort& serial) {
    if (game.screen == ScreenState::GameOver) {
        game.gameOverSeconds += frameTime; // Counts toward RestartDelay.
    }
    if (game.screen != ScreenState::Playing) {
        return; // Nothing moves on menus.
    }

    // Timers and bird physics (scaled by frameTime so speed is the same at any frame rate).
    game.elapsedSeconds += frameTime;
    game.feedbackTimer = std::max(0.0f, game.feedbackTimer - frameTime);
    game.boundaryDamageCooldown = std::max(0.0f, game.boundaryDamageCooldown - frameTime);
    game.birdVelocity += Gravity * frameTime;
    game.birdY += game.birdVelocity * frameTime;
    game.groundOffset = std::fmod(game.groundOffset + game.difficulty.obstacleSpeed * frameTime, 48.0f);

    // Floor and ceiling.
    if (game.birdY > PlayfieldBottom - BirdRadius) {
        HitBoundary(game, PlayfieldBottom - BirdRadius, "WATCH THE GROUND!");
    } else if (game.birdY < BirdRadius) {
        HitBoundary(game, BirdRadius, "STAY IN THE SKY!");
    }

    // Add a new gate once the newest one has moved far enough left.
    if (game.obstacles.empty() || game.obstacles.back().x < ScreenWidth - ObstacleSpacing) {
        SpawnObstacle(game);
    }

    // The bird is treated as a square for collision tests.
    const Rectangle birdBounds{ PlayerX - BirdRadius, game.birdY - BirdRadius,
                                BirdRadius * 2.0f, BirdRadius * 2.0f };
    for (Obstacle& obstacle : game.obstacles) {
        if (game.screen != ScreenState::Playing) {
            return; // The last life was just lost.
        }
        obstacle.x -= game.difficulty.obstacleSpeed * frameTime; // Scroll left toward the bird.

        // Prompt opens: start the reaction timer and beep once.
        if (IsPromptActive(obstacle) && obstacle.promptStartedAt < 0.0f) {
            obstacle.promptStartedAt = game.elapsedSeconds;
            serial.WriteLine("BUZZER 900 70"); // Prompt cue.
        }

        // Gate passed the bird: score it, and count the prompt as missed if unanswered.
        if (!obstacle.passed && obstacle.x + ObstacleWidth < PlayerX - BirdRadius) {
            obstacle.passed = true;
            game.score += 1;
            if (obstacle.result == PromptResult::Waiting) {
                obstacle.result = PromptResult::Failed;
                game.missedPrompts += 1;
                LoseLife(game, "PROMPT MISSED!");
            }
        }

        // Collision with either column.
        Rectangle top;
        Rectangle bottom;
        GateColumns(obstacle, game.difficulty.gapHalfHeight, top, bottom);
        const bool hitColumn = CheckCollisionRecs(birdBounds, top) || CheckCollisionRecs(birdBounds, bottom);
        if (hitColumn && !obstacle.collisionHandled) {
            obstacle.collisionHandled = true; // Contact costs a life even on a green (answered) gate.
            if (obstacle.result == PromptResult::Waiting) {
                obstacle.result = PromptResult::Failed; // A hit closes an unanswered prompt.
            }
            LoseLife(game, "BUMP! TRY AGAIN!");
        }
    }

    // Drop gates that have scrolled fully off the left edge.
    game.obstacles.erase(
        std::remove_if(game.obstacles.begin(), game.obstacles.end(),
                       [](const Obstacle& obstacle) { return obstacle.x + ObstacleWidth < -20.0f; }),
        game.obstacles.end());
}

// --------------------------------- Drawing ------------------------------------

/**
 * @brief Draw text horizontally centered on the window.
 * @param text Text to draw.
 * @param y Top of the text in pixels.
 * @param fontSize Raylib default-font size.
 * @param color Text color.
 */
void DrawCentered(const char* text, int y, int fontSize, Color color) {
    DrawText(text, ScreenWidth / 2 - MeasureText(text, fontSize) / 2, y, fontSize, color);
}

/**
 * @brief Draw one gate; answered gates turn green, and a gate whose prompt is open shows a badge.
 * @param obstacle Gate to draw.
 * @param gapHalfHeight Half the opening height for the current difficulty.
 */
void DrawObstacle(const Obstacle& obstacle, float gapHalfHeight) {
    Rectangle top;
    Rectangle bottom;
    GateColumns(obstacle, gapHalfHeight, top, bottom);
    const Color columnColor = obstacle.result == PromptResult::Correct ? GREEN : Color{ 63, 151, 103, 255 };
    DrawRectangleRec(top, columnColor);
    DrawRectangleRec(bottom, columnColor);
    DrawRectangleLinesEx(top, 3.0f, DARKGREEN); // Outlines give a cartoon look.
    DrawRectangleLinesEx(bottom, 3.0f, DARKGREEN);

    // Badge in the gap ("!" = yell, "C" = cover), shown only while the prompt can
    // be answered, so players never yell or cover before it counts.
    if (IsPromptActive(obstacle)) {
        const int centerX = static_cast<int>(obstacle.x + ObstacleWidth / 2.0f);
        DrawCircle(centerX, static_cast<int>(obstacle.gapCenter), 19.0f, GOLD);
        DrawText(obstacle.prompt == PromptType::Yell ? "!" : "C", centerX - 7,
                 static_cast<int>(obstacle.gapCenter - 11.0f), 22, DARKBROWN);
    }
}

/**
 * @brief Draw the bird from simple shapes (no image assets needed).
 * @param centerY Vertical center of the bird.
 * @param bodyColor Selected body color.
 */
void DrawBird(float centerY, Color bodyColor) {
    const int x = static_cast<int>(PlayerX);
    const int y = static_cast<int>(centerY);
    DrawCircle(x, y, BirdRadius, bodyColor); // Body.
    DrawEllipse(x - 3, y + 4, 8.0f, 5.5f, ColorBrightness(bodyColor, -0.25f)); // Wing, a darker shade of the body.
    DrawTriangle({ PlayerX + 13.0f, centerY - 2.0f }, { PlayerX + 26.0f, centerY + 2.0f },
                 { PlayerX + 13.0f, centerY + 6.0f }, ORANGE); // Beak.
    DrawCircle(x + 6, y - 5, 3.0f, WHITE); // Eye.
    DrawCircle(x + 7, y - 5, 1.5f, BLACK); // Pupil.
}

/**
 * @brief Draw the selected scene (sky, scenery) and the scrolling ground.
 * @param game Supplies the theme and the ground scroll offset.
 */
void DrawBackground(const Game& game) {
    Color groundColor; // Each theme picks its own ground palette below.
    Color groundEdgeColor;
    Color groundMarkColor;

    switch (game.themeIndex) {
        case 1: // Sunset: warm sky, low sun, layered hills.
            ClearBackground(Color{ 247, 137, 111, 255 });
            DrawCircle(755, 245, 70.0f, Fade(Color{ 255, 224, 139, 255 }, 0.95f));
            DrawTriangle({ 0.0f, 475.0f }, { 250.0f, 290.0f }, { 510.0f, 475.0f }, Color{ 202, 101, 105, 255 });
            DrawTriangle({ 360.0f, 490.0f }, { 690.0f, 330.0f }, { 960.0f, 490.0f }, Color{ 176, 86, 103, 255 });
            DrawTriangle({ 0.0f, 520.0f }, { 450.0f, 400.0f }, { 800.0f, 520.0f }, Color{ 128, 77, 103, 255 });
            groundColor = Color{ 124, 77, 84, 255 };
            groundEdgeColor = Color{ 220, 126, 91, 255 };
            groundMarkColor = Color{ 167, 99, 86, 255 };
            break;
        case 2: // Night: stars, crescent moon, dark hills.
            ClearBackground(Color{ 20, 32, 68, 255 });
            for (int i = 0; i < 18; ++i) { // Pseudo-random but fixed star positions.
                DrawCircle(35 + (i * 137) % (ScreenWidth - 70), 38 + (i * 83) % 310,
                           (i % 3 == 0) ? 2.5f : 1.5f, Fade(WHITE, 0.85f));
            }
            DrawCircle(780, 125, 43.0f, Color{ 255, 238, 183, 255 }); // Moon...
            DrawCircle(799, 108, 40.0f, Color{ 20, 32, 68, 255 }); // ...cut into a crescent by a sky-colored circle.
            DrawTriangle({ 0.0f, 510.0f }, { 255.0f, 340.0f }, { 520.0f, 510.0f }, Color{ 41, 63, 101, 255 });
            DrawTriangle({ 350.0f, 520.0f }, { 700.0f, 365.0f }, { 960.0f, 520.0f }, Color{ 34, 54, 91, 255 });
            groundColor = Color{ 27, 40, 67, 255 };
            groundEdgeColor = Color{ 86, 134, 128, 255 };
            groundMarkColor = Color{ 54, 77, 98, 255 };
            break;
        default: // Meadow (index 0): blue sky, sun, clouds made of overlapping circles.
            ClearBackground(SKYBLUE);
            DrawCircle(790, 115, 44.0f, Fade(YELLOW, 0.85f));
            DrawCircle(155, 130, 25.0f, Fade(WHITE, 0.85f));
            DrawCircle(185, 125, 34.0f, Fade(WHITE, 0.85f));
            DrawCircle(218, 134, 23.0f, Fade(WHITE, 0.85f));
            DrawCircle(535, 200, 24.0f, Fade(WHITE, 0.65f));
            DrawCircle(565, 194, 33.0f, Fade(WHITE, 0.65f));
            DrawCircle(598, 205, 22.0f, Fade(WHITE, 0.65f));
            groundColor = Color{ 218, 190, 112, 255 };
            groundEdgeColor = Color{ 108, 190, 95, 255 };
            groundMarkColor = Color{ 189, 160, 91, 255 };
            break;
    }

    // Ground strip, a grass/edge band on top, and soil marks that scroll to show movement.
    const int groundTop = static_cast<int>(PlayfieldBottom);
    DrawRectangle(0, groundTop, ScreenWidth, static_cast<int>(GroundHeight), groundColor);
    DrawRectangle(0, groundTop, ScreenWidth, 10, groundEdgeColor);
    for (int x = -48; x < ScreenWidth + 48; x += 48) {
        const int tileX = x - static_cast<int>(game.groundOffset);
        DrawLine(tileX, ScreenHeight - 35, tileX + 24, ScreenHeight - 35, groundMarkColor);
    }
}

/**
 * @brief Draw the Arduino connection status text.
 * @param connected The serial port is open.
 * @param requested A port was given with --serial (so we are searching when not connected).
 * @param x Right edge of the text when not centered.
 * @param y Top of the text.
 * @param centered Center horizontally instead of right-aligning at x.
 */
void DrawConnectionStatus(bool connected, bool requested, int x, int y, bool centered) {
    const char* text = connected ? "GROVE KIT CONNECTED"
                                 : (requested ? "SEARCHING FOR GROVE KIT" : "KEYBOARD MODE");
    const Color color = connected ? LIME : (requested ? GOLD : RAYWHITE);
    if (centered) {
        DrawCentered(text, y, 16, color);
    } else {
        DrawText(text, x - MeasureText(text, 16), y, 16, color); // Right-aligned at x.
    }
}

/**
 * @brief Draw the score bar, the active prompt, and the latest feedback message.
 * @param game Current game state.
 * @param connected The serial port is open.
 * @param requested A port was given with --serial.
 */
void DrawHud(Game& game, bool connected, bool requested) {
    // Top bar with the run's statistics.
    DrawRectangle(0, 0, ScreenWidth, 64, Fade(DARKBLUE, 0.76f));
    DrawText(TextFormat("SCORE  %03i", game.score), 24, 18, 28, WHITE);
    DrawText(TextFormat("LIVES  %i", game.lives), 245, 20, 23, GOLD);
    DrawText(TextFormat("GOOD  %i", game.correctResponses), 388, 22, 20, LIME);
    DrawText(game.correctResponses > 0 ? TextFormat("AVG RT %.2fs", AverageReactionTime(game)) : "AVG RT --",
             525, 24, 17, WHITE);
    DrawConnectionStatus(connected, requested, ScreenWidth - 20, 24, false);

    // Big instruction panel while a prompt is open.
    if (const Obstacle* active = FindActivePrompt(game)) {
        DrawRectangle(ScreenWidth / 2 - 230, 82, 460, 58, Fade(WHITE, 0.9f));
        DrawCentered(active->prompt == PromptType::Yell ? "YELL!" : "COVER THE LIGHT!", 96, 30, MAROON);
    }
    // Temporary praise or mistake message.
    if (game.feedbackTimer > 0.0f) {
        DrawCentered(game.feedbackText.c_str(), 164, 24, game.feedbackColor);
    }
}

/**
 * @brief Draw the title menu with the current difficulty, theme, and bird color.
 * @param game Supplies the player's current choices.
 * @param connected The serial port is open.
 * @param requested A port was given with --serial.
 */
void DrawTitle(const Game& game, bool connected, bool requested) {
    const Color birdColor = BirdColors[game.birdColorIndex].color;
    DrawBackground(game); // Preview the chosen theme behind the menu.
    DrawRectangle(150, 80, 660, 490, Fade(DARKBLUE, 0.82f)); // Menu card.
    DrawCentered("FOCUSBIRD", 137, 58, GOLD);
    DrawCentered("Fly through the gates. Follow the prompts!", 216, 22, RAYWHITE);
    DrawCentered(TextFormat("Selected: %s", game.difficulty.label), 267, 22, WHITE);
    DrawCentered("1  EASY       2  CHALLENGE", 308, 21, LIME);
    DrawCentered("ENTER OR GROVE BUTTON  START GAME", 348, 21, GOLD);
    DrawCentered(TextFormat("T  BACKGROUND: %s", ThemeNames[game.themeIndex]), 386, 19, SKYBLUE);
    DrawCentered(TextFormat("B  BIRD COLOR: %s", BirdColors[game.birdColorIndex].name), 416, 19, birdColor);
    DrawCircle(670, 425, 10.0f, birdColor); // Small swatch previewing the bird color.
    DrawCircleLines(670, 425, 10.0f, RAYWHITE);
    DrawText("SPACE / UP = flap only", 270, 449, 18, RAYWHITE);
    DrawText("Y = yell input       C = cover input", 270, 476, 18, RAYWHITE);
    DrawCentered("Grove button starts; sensor events answer matching prompts.", 505, 16, LIGHTGRAY);
    DrawConnectionStatus(connected, requested, 0, 532, true);
}

/**
 * @brief Draw one gameplay frame: scene, gates, bird, then HUD on top.
 * @param game Current game state.
 * @param connected The serial port is open.
 * @param requested A port was given with --serial.
 */
void DrawPlaying(Game& game, bool connected, bool requested) {
    DrawBackground(game);
    for (const Obstacle& obstacle : game.obstacles) {
        DrawObstacle(obstacle, game.difficulty.gapHalfHeight);
    }
    DrawBird(game.birdY, BirdColors[game.birdColorIndex].color); // After gates so the bird stays visible.
    DrawHud(game, connected, requested);
}

/**
 * @brief Draw the results card; the replay hint appears once restarting is allowed.
 * @param game Supplies the finished run's statistics.
 */
void DrawGameOver(const Game& game) {
    DrawBackground(game);
    DrawRectangle(190, 105, 580, 438, Fade(DARKBLUE, 0.9f)); // Results card.
    DrawCentered("NICE FLYING!", 137, 46, GOLD); // Friendly heading rather than "game over".
    DrawText(TextFormat("Gates passed: %i", game.score), 340, 220, 25, WHITE);
    DrawText(TextFormat("Correct prompts: %i", game.correctResponses), 340, 264, 22, LIME);
    DrawText(TextFormat("Wrong responses: %i", game.incorrectResponses), 340, 304, 22, ORANGE);
    DrawText(TextFormat("Missed prompts: %i", game.missedPrompts), 340, 344, 22, LIGHTGRAY);
    DrawCentered(game.correctResponses > 0
                     ? TextFormat("Reaction time: avg %.2fs  |  fastest %.2fs",
                                  AverageReactionTime(game), game.fastestReactionTime)
                     : "Reaction time: no correct responses",
                 390, 20, SKYBLUE);
    DrawText(TextFormat("Play time: %.1f seconds", game.elapsedSeconds), 340, 426, 22, LIGHTGRAY);
    if (game.gameOverSeconds >= RestartDelay) { // Only advertise replay once it will work.
        DrawCentered("ENTER OR GROVE BUTTON = PLAY AGAIN", 470, 18, GOLD);
    }
    DrawCentered("ESC = QUIT", 500, 18, LIGHTGRAY);
}

// ---------------------------------- Input -------------------------------------

/**
 * @brief Gather this frame's actions from the Arduino and the keyboard.
 * Both sources produce the same Action values, so the rules treat them
 * identically. Menu-only keys (difficulty, theme, color) are applied directly.
 * @param game Game state; menu choices are written here.
 * @param serial Source of Arduino lines (returns nothing when closed).
 * @return Actions to pass to HandleAction, in the order they arrived.
 */
std::vector<Action> CollectActions(Game& game, SerialPort& serial) {
    std::vector<Action> actions;

    // Arduino messages: one event per line.
    for (const std::string& line : serial.ReadLines()) { // Empty when the port is closed.
        if (line == "BTN") actions.push_back(Action::Button);
        else if (line == "SOUND") actions.push_back(Action::Sound);
        else if (line == "LIGHT") actions.push_back(Action::Light);
        // READY, SENSORS ..., and anything else are diagnostics and ignored.
    }

    // Keyboard equivalents.
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) actions.push_back(Action::Start);
    if (game.screen == ScreenState::Playing) {
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP)) actions.push_back(Action::Button);
        if (IsKeyPressed(KEY_Y)) actions.push_back(Action::Sound); // Stand-in for the sound sensor.
        if (IsKeyPressed(KEY_C)) actions.push_back(Action::Light); // Stand-in for covering the light sensor.
    } else if (game.screen == ScreenState::Title) { // Menu-only choices.
        if (IsKeyPressed(KEY_ONE)) game.difficulty = Easy;
        if (IsKeyPressed(KEY_TWO)) game.difficulty = Challenge;
        if (IsKeyPressed(KEY_T)) game.themeIndex = (game.themeIndex + 1) % ThemeCount; // Wrap around.
        if (IsKeyPressed(KEY_B)) game.birdColorIndex = (game.birdColorIndex + 1) % BirdColorCount;
    }
    return actions;
}

} // namespace

/**
 * @brief Parse arguments, open the optional Arduino port, and run the game loop.
 * Usage: focusbird [--serial PORT]   (no arguments = keyboard-only play)
 * @param argc Number of command-line arguments.
 * @param argv Command-line arguments.
 * @return 0 on normal exit or --help, 1 for an unrecognized argument.
 */
int main(int argc, char** argv) {
    // Command line: the only option is --serial PORT; anything else prints usage.
    std::string serialName; // Empty means keyboard-only play.
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--serial" && i + 1 < argc) {
            serialName = argv[++i]; // Consume the port name that follows.
        } else {
            std::cout << "FocusBird - Raylib + Arduino reaction game\n"
                      << "Usage: " << argv[0] << " [--serial PORT]\n"
                      << "  e.g. --serial COM3, --serial /dev/ttyACM0, --serial /dev/cu.usbmodem1101\n";
            return (argument == "--help" || argument == "-h") ? 0 : 1;
        }
    }

    // Open the Arduino port if one was given. Failure is not fatal: the game
    // runs on the keyboard and keeps retrying in the main loop.
    SerialPort serial;
    const bool serialRequested = !serialName.empty();
    if (serialRequested && !serial.Open(serialName)) {
        std::cerr << "Could not open serial port '" << serialName
                  << "'. Keyboard controls still work; retrying every "
                  << SerialReconnectInterval << " s.\n";
    }
    float reconnectTimer = SerialReconnectInterval; // Countdown to the next reopen attempt.

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT); // Smooth edges, no tearing.
    InitWindow(ScreenWidth, ScreenHeight, "FocusBird - Focus, flap, fly!");
    SetTargetFPS(60);

    Game game;
    while (!WindowShouldClose()) { // Also true when Esc is pressed.
        const float frameTime = std::min(GetFrameTime(), 0.05f); // Clamp long stalls to avoid physics jumps.

        // Keep retrying a missing or unplugged board.
        if (serialRequested && !serial.IsOpen()) {
            reconnectTimer -= frameTime;
            if (reconnectTimer <= 0.0f) {
                reconnectTimer = SerialReconnectInterval;
                if (serial.Open(serialName)) {
                    std::cerr << "Connected to serial port '" << serialName << "'.\n";
                }
            }
        }

        // Input, then simulation.
        for (Action action : CollectActions(game, serial)) {
            HandleAction(game, action, serial);
        }
        UpdateGame(game, frameTime, serial);

        // Draw the current screen.
        const bool connected = serial.IsOpen();
        BeginDrawing();
        if (game.screen == ScreenState::Title) {
            DrawTitle(game, connected, serialRequested);
        } else if (game.screen == ScreenState::Playing) {
            DrawPlaying(game, connected, serialRequested);
        } else {
            DrawGameOver(game);
        }
        EndDrawing();
    }

    CloseWindow(); // Release the window and graphics resources.
    return 0;
}
