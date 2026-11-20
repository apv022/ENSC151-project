#include "serial_port.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <utility>
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
constexpr float FlapVelocity = -350.0f; // Upward velocity applied by a flap.
constexpr float ObstacleWidth = 72.0f; // Width of each gate column.
constexpr float ObstacleSpacing = 540.0f; // Distance between gates; larger than the prompt lead so windows never overlap.
constexpr float PromptLeadDistance = 420.0f; // A gate's prompt becomes active within this distance of the bird.
constexpr int StartingLives = 3; // Mistakes/collisions allowed before game over.
constexpr float SerialReconnectInterval = 2.0f; // Seconds between attempts to reopen a lost Arduino port.
constexpr float RestartDelay = 1.0f; // Ignore start input briefly on the results screen so a flap doesn't skip it.

// ----------------------------- Gameplay data types -----------------------------

enum class ScreenState { Title, Playing, GameOver };

enum class Action {
    Start, // Enter: start a run from a menu.
    Button, // Grove button or Space/Up: start from a menu, flap during play; never answers a prompt.
    Sound, // Sound sensor event, or Y on the keyboard.
    Light // Covered-light-sensor event, or C on the keyboard.
};

enum class PromptType { Yell, Cover };

enum class PromptResult {
    Waiting, // Not yet answered; may still be active.
    Correct, // Matching action arrived inside the prompt window.
    Failed // Wrong action, expired, or the gate was hit.
};

struct DifficultySettings {
    const char* label; // Name shown on the title screen.
    float obstacleSpeed; // Gate travel speed in pixels per second.
    float gapHalfHeight; // Half of the safe opening height in pixels.
};

constexpr DifficultySettings Easy{ "Easy", 220.0f, 112.0f }; // Slower gates, wider openings.
constexpr DifficultySettings Challenge{ "Challenge", 285.0f, 94.0f }; // Faster gates, narrower openings.

struct NamedColor {
    const char* name; // Label shown on the title screen.
    Color color; // Bird body color.
};

const NamedColor BirdColors[] = {
    { "GOLD", GOLD }, { "BLUE", Color{ 30, 75, 180, 255 } }, { "PINK", PINK },
    { "PURPLE", VIOLET }, { "GREEN", LIME },
};
constexpr int BirdColorCount = sizeof(BirdColors) / sizeof(BirdColors[0]);

const char* const ThemeNames[] = { "MEADOW", "SUNSET", "NIGHT" }; // Index matches DrawBackground's cases.
constexpr int ThemeCount = sizeof(ThemeNames) / sizeof(ThemeNames[0]);

struct Obstacle {
    float x; // Left edge in screen coordinates.
    float gapCenter; // Vertical center of the opening.
    PromptType prompt; // Action this gate asks for.
    PromptResult result = PromptResult::Waiting; // Answer state; controls color and scoring.
    float promptStartedAt = -1.0f; // Game time the prompt became active; negative until then.
    bool passed = false; // Score is awarded at most once.
    bool collisionHandled = false; // A life is lost at most once per gate.
};

struct Game {
    // Player choices: kept across runs.
    DifficultySettings difficulty = Easy;
    int themeIndex = 0;
    int birdColorIndex = 0;

    // Per-run state: reset by StartGame.
    ScreenState screen = ScreenState::Title;
    float birdY = ScreenHeight * 0.48f; // Vertical bird center.
    float birdVelocity = 0.0f; // Positive values move downward.
    float elapsedSeconds = 0.0f; // Run duration for the summary.
    float gameOverSeconds = 0.0f; // Time spent on the results screen.
    float boundaryDamageCooldown = 0.0f; // Prevents frame-by-frame life loss on the floor/ceiling.
    float groundOffset = 0.0f; // Scrolling ground animation offset.
    int score = 0; // Gates passed.
    int lives = StartingLives;
    int correctResponses = 0;
    int incorrectResponses = 0;
    int missedPrompts = 0;
    float reactionTimeTotal = 0.0f; // Sum of correct response times (count = correctResponses).
    float fastestReactionTime = 0.0f; // Fastest correct response.
    std::string feedbackText; // Short message shown over gameplay.
    Color feedbackColor = MAROON;
    float feedbackTimer = 0.0f; // Seconds the message stays visible.
    std::vector<Obstacle> obstacles; // Oldest first.
    std::mt19937 random{ std::random_device{}() };
};

// ------------------------------- Game rules -----------------------------------

/**
 * @brief Return the rectangles of a gate's solid top and bottom columns.
 * Shared by collision and drawing so they can never disagree.
 */
void GateColumns(const Obstacle& obstacle, float gapHalfHeight, Rectangle& top, Rectangle& bottom) {
    const float gapTop = obstacle.gapCenter - gapHalfHeight;
    const float gapBottom = obstacle.gapCenter + gapHalfHeight;
    top = { obstacle.x, 0.0f, ObstacleWidth, gapTop };
    bottom = { obstacle.x, gapBottom, ObstacleWidth, PlayfieldBottom - gapBottom };
}

/**
 * @brief True while a gate is close enough to the bird that its prompt can be answered.
 */
bool IsPromptActive(const Obstacle& obstacle) {
    return obstacle.result == PromptResult::Waiting
        && obstacle.x <= PlayerX + PromptLeadDistance
        && obstacle.x + ObstacleWidth >= PlayerX - BirdRadius;
}

/**
 * @brief The single gate whose prompt is currently answerable, or nullptr.
 * Searching oldest-first means only the next gate can be answered.
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
 * @return Seconds, or zero before the first correct response.
 */
float AverageReactionTime(const Game& game) {
    return game.correctResponses > 0 ? game.reactionTimeTotal / game.correctResponses : 0.0f;
}

/**
 * @brief Show a short message over gameplay for a few seconds.
 */
void ShowFeedback(Game& game, const char* text, Color color, float seconds) {
    game.feedbackText = text;
    game.feedbackColor = color;
    game.feedbackTimer = seconds;
}

/**
 * @brief Reset per-run statistics while keeping difficulty, theme, and bird color.
 */
void StartGame(Game& game) {
    Game fresh; // Default member values describe a brand-new run.
    fresh.difficulty = game.difficulty;
    fresh.themeIndex = game.themeIndex;
    fresh.birdColorIndex = game.birdColorIndex;
    fresh.random = game.random; // Keep the existing random sequence going.
    fresh.screen = ScreenState::Playing;
    game = std::move(fresh);
}

/**
 * @brief Spend one life with a short message; zero lives ends the run.
 */
void LoseLife(Game& game, const char* message) {
    if (game.screen != ScreenState::Playing) {
        return; // Statistics are frozen once the run has ended.
    }
    game.lives -= 1;
    ShowFeedback(game, message, MAROON, 1.35f);
    if (game.lives <= 0) {
        game.screen = ScreenState::GameOver;
        game.gameOverSeconds = 0.0f;
    }
}

/**
 * @brief Apply one keyboard or Arduino action to the game.
 * @param serial Used for buzzer feedback; writes are ignored when it is closed.
 */
void HandleAction(Game& game, Action action, SerialPort& serial) {
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

    Obstacle* active = FindActivePrompt(game);
    if (active == nullptr) {
        return; // Early/late sensor events cannot count for a future prompt.
    }
    const bool matches = (active->prompt == PromptType::Yell && action == Action::Sound)
        || (active->prompt == PromptType::Cover && action == Action::Light);
    if (matches) {
        active->result = PromptResult::Correct;
        const float reactionTime = std::max(0.0f, game.elapsedSeconds - active->promptStartedAt);
        if (game.correctResponses == 0 || reactionTime < game.fastestReactionTime) {
            game.fastestReactionTime = reactionTime;
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
 * @brief Append a gate just off the right edge with a random gap height and prompt.
 */
void SpawnObstacle(Game& game) {
    const float margin = game.difficulty.gapHalfHeight + 58.0f; // Keep the opening away from ceiling and ground.
    std::uniform_real_distribution<float> gapDistribution(margin, PlayfieldBottom - margin);
    std::uniform_int_distribution<int> promptDistribution(0, 1);
    Obstacle obstacle{};
    obstacle.x = ScreenWidth + 24.0f;
    obstacle.gapCenter = gapDistribution(game.random);
    obstacle.prompt = promptDistribution(game.random) == 0 ? PromptType::Yell : PromptType::Cover;
    game.obstacles.push_back(obstacle);
}

/**
 * @brief Penalize touching the floor or ceiling at most once per second.
 */
void HitBoundary(Game& game, float clampedY, const char* message) {
    game.birdY = clampedY;
    game.birdVelocity = 0.0f;
    if (game.boundaryDamageCooldown <= 0.0f) {
        LoseLife(game, message);
        game.boundaryDamageCooldown = 1.0f;
    }
}

/**
 * @brief Advance physics, prompts, gates, scoring, and collisions by one frame.
 */
void UpdateGame(Game& game, float frameTime, SerialPort& serial) {
    if (game.screen == ScreenState::GameOver) {
        game.gameOverSeconds += frameTime;
    }
    if (game.screen != ScreenState::Playing) {
        return;
    }

    game.elapsedSeconds += frameTime;
    game.feedbackTimer = std::max(0.0f, game.feedbackTimer - frameTime);
    game.boundaryDamageCooldown = std::max(0.0f, game.boundaryDamageCooldown - frameTime);
    game.birdVelocity += Gravity * frameTime; // Frame-rate-independent physics.
    game.birdY += game.birdVelocity * frameTime;
    game.groundOffset = std::fmod(game.groundOffset + game.difficulty.obstacleSpeed * frameTime, 48.0f);

    if (game.birdY > PlayfieldBottom - BirdRadius) {
        HitBoundary(game, PlayfieldBottom - BirdRadius, "WATCH THE GROUND!");
    } else if (game.birdY < BirdRadius) {
        HitBoundary(game, BirdRadius, "STAY IN THE SKY!");
    }

    if (game.obstacles.empty() || game.obstacles.back().x < ScreenWidth - ObstacleSpacing) {
        SpawnObstacle(game);
    }

    const Rectangle birdBounds{ PlayerX - BirdRadius, game.birdY - BirdRadius,
                                BirdRadius * 2.0f, BirdRadius * 2.0f };
    for (Obstacle& obstacle : game.obstacles) {
        if (game.screen != ScreenState::Playing) {
            return; // The last life was just lost.
        }
        obstacle.x -= game.difficulty.obstacleSpeed * frameTime;

        if (IsPromptActive(obstacle) && obstacle.promptStartedAt < 0.0f) {
            obstacle.promptStartedAt = game.elapsedSeconds; // Reaction timing starts now.
            serial.WriteLine("BUZZER 900 70"); // Prompt cue.
        }

        if (!obstacle.passed && obstacle.x + ObstacleWidth < PlayerX - BirdRadius) {
            obstacle.passed = true;
            game.score += 1;
            if (obstacle.result == PromptResult::Waiting) {
                obstacle.result = PromptResult::Failed;
                game.missedPrompts += 1;
                LoseLife(game, "PROMPT MISSED!");
            }
        }

        Rectangle top;
        Rectangle bottom;
        GateColumns(obstacle, game.difficulty.gapHalfHeight, top, bottom);
        const bool hitColumn = CheckCollisionRecs(birdBounds, top) || CheckCollisionRecs(birdBounds, bottom);
        if (hitColumn && !obstacle.collisionHandled) {
            obstacle.collisionHandled = true; // Contact costs a life even on a green (answered) gate.
            if (obstacle.result == PromptResult::Waiting) {
                obstacle.result = PromptResult::Failed;
            }
            LoseLife(game, "BUMP! TRY AGAIN!");
        }
    }

    game.obstacles.erase(
        std::remove_if(game.obstacles.begin(), game.obstacles.end(),
                       [](const Obstacle& obstacle) { return obstacle.x + ObstacleWidth < -20.0f; }),
        game.obstacles.end()); // Drop gates that are fully off-screen.
}

// --------------------------------- Drawing ------------------------------------

/**
 * @brief Draw text horizontally centered on the window.
 */
void DrawCentered(const char* text, int y, int fontSize, Color color) {
    DrawText(text, ScreenWidth / 2 - MeasureText(text, fontSize) / 2, y, fontSize, color);
}

/**
 * @brief Draw one gate; answered gates turn green, unanswered ones show a prompt badge.
 */
void DrawObstacle(const Obstacle& obstacle, float gapHalfHeight) {
    Rectangle top;
    Rectangle bottom;
    GateColumns(obstacle, gapHalfHeight, top, bottom);
    const Color columnColor = obstacle.result == PromptResult::Correct ? GREEN : Color{ 63, 151, 103, 255 };
    DrawRectangleRec(top, columnColor);
    DrawRectangleRec(bottom, columnColor);
    DrawRectangleLinesEx(top, 3.0f, DARKGREEN);
    DrawRectangleLinesEx(bottom, 3.0f, DARKGREEN);

    if (obstacle.result == PromptResult::Waiting) { // Badge in the gap shows the required action.
        const int centerX = static_cast<int>(obstacle.x + ObstacleWidth / 2.0f);
        DrawCircle(centerX, static_cast<int>(obstacle.gapCenter), 19.0f, GOLD);
        DrawText(obstacle.prompt == PromptType::Yell ? "!" : "C", centerX - 7,
                 static_cast<int>(obstacle.gapCenter - 11.0f), 22, DARKBROWN);
    }
}

/**
 * @brief Draw the bird from primitives (no image assets).
 */
void DrawBird(float centerY, Color bodyColor) {
    const int x = static_cast<int>(PlayerX);
    const int y = static_cast<int>(centerY);
    DrawCircle(x, y, BirdRadius, bodyColor); // Body.
    DrawEllipse(x - 3, y + 4, 8.0f, 5.5f, ColorBrightness(bodyColor, -0.25f)); // Wing.
    DrawTriangle({ PlayerX + 13.0f, centerY - 2.0f }, { PlayerX + 26.0f, centerY + 2.0f },
                 { PlayerX + 13.0f, centerY + 6.0f }, ORANGE); // Beak.
    DrawCircle(x + 6, y - 5, 3.0f, WHITE); // Eye.
    DrawCircle(x + 7, y - 5, 1.5f, BLACK); // Pupil.
}

/**
 * @brief Draw the selected scene and the scrolling ground.
 */
void DrawBackground(const Game& game) {
    Color groundColor;
    Color groundEdgeColor;
    Color groundMarkColor;

    switch (game.themeIndex) {
        case 1: // Sunset
            ClearBackground(Color{ 247, 137, 111, 255 });
            DrawCircle(755, 245, 70.0f, Fade(Color{ 255, 224, 139, 255 }, 0.95f));
            DrawTriangle({ 0.0f, 475.0f }, { 250.0f, 290.0f }, { 510.0f, 475.0f }, Color{ 202, 101, 105, 255 });
            DrawTriangle({ 360.0f, 490.0f }, { 690.0f, 330.0f }, { 960.0f, 490.0f }, Color{ 176, 86, 103, 255 });
            DrawTriangle({ 0.0f, 520.0f }, { 450.0f, 400.0f }, { 800.0f, 520.0f }, Color{ 128, 77, 103, 255 });
            groundColor = Color{ 124, 77, 84, 255 };
            groundEdgeColor = Color{ 220, 126, 91, 255 };
            groundMarkColor = Color{ 167, 99, 86, 255 };
            break;
        case 2: // Night
            ClearBackground(Color{ 20, 32, 68, 255 });
            for (int i = 0; i < 18; ++i) {
                DrawCircle(35 + (i * 137) % (ScreenWidth - 70), 38 + (i * 83) % 310,
                           (i % 3 == 0) ? 2.5f : 1.5f, Fade(WHITE, 0.85f));
            }
            DrawCircle(780, 125, 43.0f, Color{ 255, 238, 183, 255 }); // Moon...
            DrawCircle(799, 108, 40.0f, Color{ 20, 32, 68, 255 }); // ...cut into a crescent.
            DrawTriangle({ 0.0f, 510.0f }, { 255.0f, 340.0f }, { 520.0f, 510.0f }, Color{ 41, 63, 101, 255 });
            DrawTriangle({ 350.0f, 520.0f }, { 700.0f, 365.0f }, { 960.0f, 520.0f }, Color{ 34, 54, 91, 255 });
            groundColor = Color{ 27, 40, 67, 255 };
            groundEdgeColor = Color{ 86, 134, 128, 255 };
            groundMarkColor = Color{ 54, 77, 98, 255 };
            break;
        default: // Meadow
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

    const int groundTop = static_cast<int>(PlayfieldBottom);
    DrawRectangle(0, groundTop, ScreenWidth, static_cast<int>(GroundHeight), groundColor);
    DrawRectangle(0, groundTop, ScreenWidth, 10, groundEdgeColor);
    for (int x = -48; x < ScreenWidth + 48; x += 48) { // Scrolling soil marks show movement.
        const int tileX = x - static_cast<int>(game.groundOffset);
        DrawLine(tileX, ScreenHeight - 35, tileX + 24, ScreenHeight - 35, groundMarkColor);
    }
}

/**
 * @brief Draw the Arduino connection status line.
 * @param connected The port is open. @param requested A port was given with --serial.
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
 */
void DrawHud(Game& game, bool connected, bool requested) {
    DrawRectangle(0, 0, ScreenWidth, 64, Fade(DARKBLUE, 0.76f));
    DrawText(TextFormat("SCORE  %03i", game.score), 24, 18, 28, WHITE);
    DrawText(TextFormat("LIVES  %i", game.lives), 245, 20, 23, GOLD);
    DrawText(TextFormat("GOOD  %i", game.correctResponses), 388, 22, 20, LIME);
    DrawText(game.correctResponses > 0 ? TextFormat("AVG RT %.2fs", AverageReactionTime(game)) : "AVG RT --",
             525, 24, 17, WHITE);
    DrawConnectionStatus(connected, requested, ScreenWidth - 20, 24, false);

    if (const Obstacle* active = FindActivePrompt(game)) {
        DrawRectangle(ScreenWidth / 2 - 230, 82, 460, 58, Fade(WHITE, 0.9f));
        DrawCentered(active->prompt == PromptType::Yell ? "YELL!" : "COVER THE LIGHT!", 96, 30, MAROON);
    }
    if (game.feedbackTimer > 0.0f) {
        DrawCentered(game.feedbackText.c_str(), 164, 24, game.feedbackColor);
    }
}

/**
 * @brief Draw the title menu with the current difficulty, theme, and bird color.
 */
void DrawTitle(const Game& game, bool connected, bool requested) {
    const Color birdColor = BirdColors[game.birdColorIndex].color;
    DrawBackground(game);
    DrawRectangle(150, 80, 660, 490, Fade(DARKBLUE, 0.82f));
    DrawCentered("FOCUSBIRD", 137, 58, GOLD);
    DrawCentered("Fly through the gates. Follow the prompts!", 216, 22, RAYWHITE);
    DrawCentered(TextFormat("Selected: %s", game.difficulty.label), 267, 22, WHITE);
    DrawCentered("1  EASY       2  CHALLENGE", 308, 21, LIME);
    DrawCentered("ENTER OR GROVE BUTTON  START GAME", 348, 21, GOLD);
    DrawCentered(TextFormat("T  BACKGROUND: %s", ThemeNames[game.themeIndex]), 386, 19, SKYBLUE);
    DrawCentered(TextFormat("B  BIRD COLOR: %s", BirdColors[game.birdColorIndex].name), 416, 19, birdColor);
    DrawCircle(670, 425, 10.0f, birdColor); // Color preview.
    DrawCircleLines(670, 425, 10.0f, RAYWHITE);
    DrawText("SPACE / UP = flap only", 270, 449, 18, RAYWHITE);
    DrawText("Y = yell input       C = cover input", 270, 476, 18, RAYWHITE);
    DrawCentered("Grove button starts; sensor events answer matching prompts.", 505, 16, LIGHTGRAY);
    DrawConnectionStatus(connected, requested, 0, 532, true);
}

/**
 * @brief Draw one gameplay frame: scene, gates, bird, then HUD on top.
 */
void DrawPlaying(Game& game, bool connected, bool requested) {
    DrawBackground(game);
    for (const Obstacle& obstacle : game.obstacles) {
        DrawObstacle(obstacle, game.difficulty.gapHalfHeight);
    }
    DrawBird(game.birdY, BirdColors[game.birdColorIndex].color);
    DrawHud(game, connected, requested);
}

/**
 * @brief Draw the results card; the replay hint appears once restarting is allowed.
 */
void DrawGameOver(const Game& game) {
    DrawBackground(game);
    DrawRectangle(190, 105, 580, 438, Fade(DARKBLUE, 0.9f));
    DrawCentered("NICE FLYING!", 137, 46, GOLD);
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
    if (game.gameOverSeconds >= RestartDelay) {
        DrawCentered("ENTER OR GROVE BUTTON = PLAY AGAIN", 470, 18, GOLD);
    }
    DrawCentered("ESC = QUIT", 500, 18, LIGHTGRAY);
}

// ---------------------------------- Input -------------------------------------

/**
 * @brief Gather this frame's actions from the keyboard and the Arduino.
 * Both sources produce the same Action values, so the rules treat them identically.
 */
std::vector<Action> CollectActions(Game& game, SerialPort& serial) {
    std::vector<Action> actions;
    for (const std::string& line : serial.ReadLines()) { // Empty when the port is closed.
        if (line == "BTN") actions.push_back(Action::Button);
        else if (line == "SOUND") actions.push_back(Action::Sound);
        else if (line == "LIGHT") actions.push_back(Action::Light);
        // READY, SENSORS ..., and anything else are diagnostics and ignored.
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) actions.push_back(Action::Start);
    if (game.screen == ScreenState::Playing) {
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP)) actions.push_back(Action::Button);
        if (IsKeyPressed(KEY_Y)) actions.push_back(Action::Sound);
        if (IsKeyPressed(KEY_C)) actions.push_back(Action::Light);
    } else if (game.screen == ScreenState::Title) { // Menu-only choices.
        if (IsKeyPressed(KEY_ONE)) game.difficulty = Easy;
        if (IsKeyPressed(KEY_TWO)) game.difficulty = Challenge;
        if (IsKeyPressed(KEY_T)) game.themeIndex = (game.themeIndex + 1) % ThemeCount;
        if (IsKeyPressed(KEY_B)) game.birdColorIndex = (game.birdColorIndex + 1) % BirdColorCount;
    }
    return actions;
}

} // namespace

/**
 * @brief Parse arguments, open the optional Arduino port, and run the game loop.
 * Usage: focusbird [--serial PORT]
 */
int main(int argc, char** argv) {
    std::string serialName; // Empty means keyboard-only play.
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--serial" && i + 1 < argc) {
            serialName = argv[++i];
        } else {
            std::cout << "FocusBird - Raylib + Arduino reaction game\n"
                      << "Usage: " << argv[0] << " [--serial PORT]\n"
                      << "  e.g. --serial COM3, --serial /dev/ttyACM0, --serial /dev/cu.usbmodem1101\n";
            return (argument == "--help" || argument == "-h") ? 0 : 1;
        }
    }

    SerialPort serial;
    const bool serialRequested = !serialName.empty();
    if (serialRequested && !serial.Open(serialName)) {
        std::cerr << "Could not open serial port '" << serialName
                  << "'. Keyboard controls still work; retrying every "
                  << SerialReconnectInterval << " s.\n";
    }
    float reconnectTimer = SerialReconnectInterval;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(ScreenWidth, ScreenHeight, "FocusBird - Focus, flap, fly!");
    SetTargetFPS(60);

    Game game;
    while (!WindowShouldClose()) { // Also true when Esc is pressed.
        const float frameTime = std::min(GetFrameTime(), 0.05f); // Clamp long stalls to avoid physics jumps.

        if (serialRequested && !serial.IsOpen()) { // Keep retrying a missing/unplugged board.
            reconnectTimer -= frameTime;
            if (reconnectTimer <= 0.0f) {
                reconnectTimer = SerialReconnectInterval;
                if (serial.Open(serialName)) {
                    std::cerr << "Connected to serial port '" << serialName << "'.\n";
                }
            }
        }

        for (Action action : CollectActions(game, serial)) {
            HandleAction(game, action, serial);
        }
        UpdateGame(game, frameTime, serial);

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

    CloseWindow();
    return 0;
}
