#include "serial_port.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

// ---------------------------- Window and game constants -----------------------

constexpr int ScreenWidth = 960; // Window width in pixels for the desktop game.
constexpr int ScreenHeight = 640; // Window height in pixels for the desktop game.
constexpr float GroundHeight = 72.0f; // Bottom playfield strip reserved for ground and UI.
constexpr float PlayerX = 185.0f; // Fixed horizontal bird position; obstacles scroll toward it.
constexpr float BirdRadius = 17.0f; // Bird collision radius and approximate drawn body size.
constexpr float Gravity = 920.0f; // Downward acceleration in pixels per second squared.
constexpr float FlapVelocity = -350.0f; // Upward velocity applied by a flap action.
constexpr float ObstacleWidth = 72.0f; // Width of each top/bottom obstacle column.
constexpr float PromptLeadDistance = 420.0f; // Obstacle distance at which its prompt becomes active.
constexpr int StartingLives = 3; // Number of misses/collisions allowed before game over.

// ----------------------------- Gameplay data types -----------------------------

enum class ScreenState {
    Title, // Waiting for the player to choose difficulty and begin.
    Playing, // Running physics, prompts, obstacle movement, and scoring.
    GameOver // Showing the finished run and its summary statistics.
};

enum class Action {
    Button, // Grove button press, or Space/Up on the keyboard.
    Sound, // Sound sensor event, or Y on the keyboard.
    Light // Covered-light-sensor event, or C on the keyboard.
};

enum class PromptType {
    Button, // Prompt the player to press the Grove button.
    Yell, // Prompt the player to make a sound for the sound sensor.
    Cover // Prompt the player to cover the Grove light sensor.
};

enum class PromptResult {
    Waiting, // Prompt has not yet been answered and may still be active.
    Correct, // Matching action arrived inside the prompt window.
    Failed // Prompt expired or the player submitted the wrong action.
};

struct DifficultySettings {
    const char* label; // Display name shown on the title screen.
    float obstacleSpeed; // Horizontal obstacle travel speed in pixels per second.
    float obstacleSpacing; // Target distance between sequential obstacles in pixels.
    float gapHalfHeight; // Half of the safe opening height in pixels.
    float promptLead; // Distance from the bird at which an answer window begins.
};

struct Obstacle {
    float x; // Left edge of this obstacle in screen coordinates.
    float gapCenter; // Vertical center of the navigable opening.
    PromptType prompt; // Required input for this obstacle's prompt.
    PromptResult result; // Answer state controls score, feedback, and safe passage.
    float promptStartedAt; // Game-clock time when this response window first became active.
    bool promptWasAnnounced; // Prevents sending repeated buzzer cues while prompt stays active.
    bool passed; // Ensures this obstacle increments the passed count at most once.
    bool collisionHandled; // Prevents repeated life loss while the bird overlaps one gate.
};

struct Game {
    ScreenState screen = ScreenState::Title; // Current top-level screen/state machine value.
    DifficultySettings difficulty{ "Easy", 220.0f, 540.0f, 112.0f, PromptLeadDistance }; // Safe default preset.
    float birdY = ScreenHeight * 0.48f; // Current vertical bird center in pixels.
    float birdVelocity = 0.0f; // Current vertical speed; positive values move downward.
    float elapsedSeconds = 0.0f; // Duration of this run, used for the final summary.
    float feedbackTimer = 0.0f; // Time remaining for the temporary success/error message.
    float boundaryDamageCooldown = 0.0f; // Prevents frame-by-frame life loss while touching a boundary.
    float groundOffset = 0.0f; // Scrolling ground texture offset for a sense of movement.
    int score = 0; // Number of safely passed obstacles.
    int correctResponses = 0; // Number of prompts answered with the expected action.
    int incorrectResponses = 0; // Number of wrong actions during an active prompt.
    int missedPrompts = 0; // Number of prompt windows that expired without an answer.
    int reactionCount = 0; // Number of correctly answered prompts with a measured reaction time.
    float reactionTimeTotal = 0.0f; // Sum of correct prompt response times in seconds.
    float fastestReactionTime = 0.0f; // Fastest correct response, or zero before any answer.
    int lives = StartingLives; // Remaining mistakes before the session ends.
    std::string feedbackText; // Latest short feedback phrase displayed over gameplay.
    std::vector<Obstacle> obstacles; // Active obstacles, ordered from oldest to newest.
    std::mt19937 random{ std::random_device{}() }; // Random engine selects gap positions and prompt types.
};

/**
 * @brief Describe the player action required by a prompt.
 * @param prompt The prompt type to display.
 * @return Human-readable uppercase action name.
 */
const char* PromptActionText(PromptType prompt) {
    switch (prompt) {
        case PromptType::Button: return "PRESS THE BUTTON!";
        case PromptType::Yell: return "YELL!";
        case PromptType::Cover: return "COVER THE LIGHT!";
    }
    return "READY!"; // Defensive fallback if an invalid enum reaches the display layer.
}

/**
 * @brief Map an incoming serial event token to the game's input-action enum.
 * @param line One complete newline-terminated message without its line ending.
 * @param action Output reference populated only when a recognized token is found.
 * @return true for BTN, SOUND, or LIGHT; false for diagnostics/unknown messages.
 */
bool ParseSerialAction(const std::string& line, Action& action) {
    if (line == "BTN") {
        action = Action::Button; // Hardware button press is also the flap action.
        return true;
    }
    if (line == "SOUND") {
        action = Action::Sound; // Sound threshold crossing from Arduino firmware.
        return true;
    }
    if (line == "LIGHT") {
        action = Action::Light; // Light sensor changed into its covered state.
        return true;
    }
    return false; // Ignore READY and any other diagnostic/protocol lines.
}

/**
 * @brief Apply the selected title-screen difficulty to the game state.
 * @param game Game data whose difficulty settings will be replaced.
 * @param challenge true selects the faster/harder preset; false selects Easy.
 * @return No value; updates the preset used for the next run.
 */
void SetDifficulty(Game& game, bool challenge) {
    if (challenge) {
        game.difficulty = { "Challenge", 285.0f, 540.0f, 94.0f, PromptLeadDistance };
    } else {
        game.difficulty = { "Easy", 220.0f, 540.0f, 112.0f, PromptLeadDistance };
    }
}

/**
 * @brief Reset run statistics and prepare the play screen.
 * @param game Game data to initialize for a fresh run.
 * @return No value; the game enters Playing with no active obstacles.
 */
void StartGame(Game& game) {
    game.screen = ScreenState::Playing; // Switch from title/game over into active play.
    game.birdY = ScreenHeight * 0.48f; // Place the bird near the vertical center.
    game.birdVelocity = 0.0f; // Start without inherited momentum from an earlier run.
    game.elapsedSeconds = 0.0f; // Reset the session clock for the final summary.
    game.feedbackTimer = 0.0f; // Hide any old feedback at the start of a run.
    game.boundaryDamageCooldown = 0.0f; // Allow an initial boundary contact to count immediately.
    game.score = 0; // Clear passed-obstacle score.
    game.correctResponses = 0; // Clear prompt successes.
    game.incorrectResponses = 0; // Clear wrong prompt responses.
    game.missedPrompts = 0; // Clear unanswered prompt count.
    game.reactionCount = 0; // Clear reaction-time sample count.
    game.reactionTimeTotal = 0.0f; // Clear reaction-time sum.
    game.fastestReactionTime = 0.0f; // Clear fastest measured response.
    game.lives = StartingLives; // Restore the run's full mistake allowance.
    game.obstacles.clear(); // Remove objects left by a previous game.
    game.feedbackText.clear(); // Remove stale text from the prior screen.
}

/**
 * @brief Spend one life and show a short child-friendly failure message.
 * @param game Game data whose lives and screen state may change.
 * @param message Reason displayed to the player.
 * @return No value; zero lives immediately transitions to the game-over screen.
 */
void LoseLife(Game& game, const char* message) {
    if (game.screen != ScreenState::Playing) {
        return; // Do not modify run statistics after leaving gameplay.
    }
    game.lives = std::max(0, game.lives - 1); // Clamp lives so drawing never shows a negative count.
    game.feedbackText = message; // Explain the latest mistake without a harsh game-over tone.
    game.feedbackTimer = 1.35f; // Keep the feedback visible long enough to read.
    if (game.lives == 0) {
        game.screen = ScreenState::GameOver; // End the run as soon as the last life is spent.
    }
}

/**
 * @brief Select the closest unresolved obstacle whose answer window is active.
 * @param game Current game state containing ordered active obstacles.
 * @return Pointer to the active obstacle, or nullptr when no prompt is answerable.
 */
Obstacle* FindActivePrompt(Game& game) {
    for (Obstacle& obstacle : game.obstacles) { // Search oldest-first so only the next gate is answered.
        const bool hasReachedPrompt = obstacle.x <= PlayerX + game.difficulty.promptLead;
        const bool hasNotPassedBird = obstacle.x + ObstacleWidth >= PlayerX - BirdRadius;
        if (obstacle.result == PromptResult::Waiting && hasReachedPrompt && hasNotPassedBird) {
            return &obstacle; // The first eligible gate owns the current prompt window.
        }
    }
    return nullptr; // No obstacle is currently waiting for a response.
}

/**
 * @brief Apply a button/sound/light action to the bird and active prompt.
 * @param game Current game state to update.
 * @param action Input event from keyboard or Arduino.
 * @param serial Optional open serial port used to play a buzzer feedback tone.
 * @return No value; the action can flap, answer a prompt, or be ignored.
 */
void HandleAction(Game& game, Action action, SerialPort* serial) {
    if (game.screen != ScreenState::Playing) {
        return; // Actions outside a run do not affect bird physics or prompt statistics.
    }

    if (action == Action::Button) {
        game.birdVelocity = FlapVelocity; // Button always flaps, even when no button prompt is active.
    }

    Obstacle* active = FindActivePrompt(game); // Resolve at most the currently visible prompt.
    if (active == nullptr) {
        return; // An early/late sensor event cannot accidentally count for a future prompt.
    }
    if (action == Action::Button && active->prompt != PromptType::Button) {
        return; // Preserve button-as-flight control without penalizing a flap during another prompt.
    }

    const bool matches = (active->prompt == PromptType::Button && action == Action::Button)
        || (active->prompt == PromptType::Yell && action == Action::Sound)
        || (active->prompt == PromptType::Cover && action == Action::Light);
    if (matches) {
        active->result = PromptResult::Correct; // Mark the gate answered to ignore repeat sensor events.
        game.correctResponses += 1; // Record this response for the final results screen.
        const float reactionTime = std::max(0.0f, game.elapsedSeconds - active->promptStartedAt); // Time from prompt activation to event receipt.
        game.reactionTimeTotal += reactionTime; // Accumulate successful response time for an average.
        game.reactionCount += 1; // Count only correct, in-window responses in this metric.
        if (game.reactionCount == 1 || reactionTime < game.fastestReactionTime) {
            game.fastestReactionTime = reactionTime; // Keep the minimum valid response time seen so far.
        }
        game.feedbackText = "GREAT JOB!"; // Positive reinforcement for a correct action.
        game.feedbackTimer = 1.0f; // Briefly show the congratulatory message.
        if (serial != nullptr && serial->IsOpen()) {
            serial->WriteLine("BUZZER 1350 100"); // Send a short high tone as success feedback.
        }
    } else {
        active->result = PromptResult::Failed; // First wrong response closes this prompt window.
        game.incorrectResponses += 1; // Preserve a separate incorrect-response statistic.
        LoseLife(game, "NOT THAT ONE - TRY THE NEXT!"); // Explain the mistake and spend one life.
        if (serial != nullptr && serial->IsOpen()) {
            serial->WriteLine("BUZZER 380 150"); // Use a lower tone to distinguish an incorrect action.
        }
    }
}

/**
 * @brief Create a randomized obstacle with a centered safe gap and prompt.
 * @param game Current game state supplying difficulty and random number engine.
 * @return No value; a new obstacle is appended after the existing obstacles.
 */
void SpawnObstacle(Game& game) {
    const float minimumCenter = game.difficulty.gapHalfHeight + 58.0f; // Keep the opening clear of the ceiling.
    const float maximumCenter = ScreenHeight - GroundHeight - game.difficulty.gapHalfHeight - 58.0f; // Keep it above ground.
    std::uniform_real_distribution<float> gapDistribution(minimumCenter, maximumCenter); // Sample a safe opening height.
    std::uniform_int_distribution<int> promptDistribution(0, 2); // Give each prompt type equal initial probability.
    const int selectedPrompt = promptDistribution(game.random); // Randomly choose button, yell, or cover.
    const PromptType prompt = static_cast<PromptType>(selectedPrompt); // Enum order matches the random values.
    game.obstacles.push_back({
        static_cast<float>(ScreenWidth + 24), // Start just outside the right edge of the window.
        gapDistribution(game.random), // Place the gap at a playable randomized height.
        prompt, // Bind one required action to this obstacle.
        PromptResult::Waiting, // Start with its prompt unanswered.
        0.0f, // Prompt clock is initialized when this gate enters its response window.
        false, // Buzzer cue has not yet been sent for this prompt.
        false, // The obstacle has not yet passed the bird.
        false // The bird has not yet collided with this obstacle.
    });
}

/**
 * @brief Advance physics, prompts, obstacles, score, and collision checks.
 * @param game Current mutable game state.
 * @param frameTime Elapsed frame duration in seconds, clamped by the caller.
 * @param serial Optional open port for an audible prompt-start cue.
 * @return No value; updates the run and may transition to GameOver.
 */
void UpdateGame(Game& game, float frameTime, SerialPort* serial) {
    if (game.screen != ScreenState::Playing) {
        return; // Title and results screens do not advance gameplay simulation.
    }

    game.elapsedSeconds += frameTime; // Accumulate active-play time for results.
    game.feedbackTimer = std::max(0.0f, game.feedbackTimer - frameTime); // Count down temporary feedback.
    game.boundaryDamageCooldown = std::max(0.0f, game.boundaryDamageCooldown - frameTime); // Re-arm boundary penalty after a pause.
    game.birdVelocity += Gravity * frameTime; // Apply gravity in a frame-rate-independent way.
    game.birdY += game.birdVelocity * frameTime; // Integrate vertical velocity into bird position.
    game.groundOffset = std::fmod(game.groundOffset + game.difficulty.obstacleSpeed * frameTime, 48.0f); // Loop ground tiles.

    const float floorY = ScreenHeight - GroundHeight - BirdRadius; // Lowest center that keeps the bird above ground.
    if (game.birdY > floorY) {
        game.birdY = floorY; // Clamp bird to the playfield instead of drawing it through the ground.
        game.birdVelocity = 0.0f; // Remove downward speed while the bird is on the floor.
        if (game.boundaryDamageCooldown <= 0.0f) {
            LoseLife(game, "WATCH THE GROUND!"); // Ground contact costs one life.
            game.boundaryDamageCooldown = 1.0f; // Require a full second before another boundary penalty.
        }
    }
    if (game.birdY < BirdRadius) {
        game.birdY = BirdRadius; // Keep the bird's full body visible below the top boundary.
        game.birdVelocity = 0.0f; // Stop upward motion at the ceiling.
        if (game.boundaryDamageCooldown <= 0.0f) {
            LoseLife(game, "STAY IN THE SKY!"); // Ceiling contact costs one life.
            game.boundaryDamageCooldown = 1.0f; // Require a full second before another boundary penalty.
        }
    }
    if (game.screen != ScreenState::Playing) {
        return; // A boundary hit may have ended the run; do not continue its simulation.
    }

    if (game.obstacles.empty()
        || game.obstacles.back().x < ScreenWidth - game.difficulty.obstacleSpacing) {
        SpawnObstacle(game); // Keep one obstacle entering from the right at a time.
    }

    const Rectangle birdBounds{ PlayerX - BirdRadius, game.birdY - BirdRadius,
                                BirdRadius * 2.0f, BirdRadius * 2.0f }; // Approximate the circular bird for collision tests.
    for (Obstacle& obstacle : game.obstacles) { // Move and resolve every currently active obstacle.
        obstacle.x -= game.difficulty.obstacleSpeed * frameTime; // Scroll obstacle left toward the fixed bird.

        const bool promptIsActive = obstacle.result == PromptResult::Waiting
            && obstacle.x <= PlayerX + game.difficulty.promptLead
            && obstacle.x + ObstacleWidth >= PlayerX - BirdRadius;
        if (promptIsActive && !obstacle.promptWasAnnounced) {
            obstacle.promptWasAnnounced = true; // Record that this obstacle has entered its response window.
            obstacle.promptStartedAt = game.elapsedSeconds; // Start reaction timing when the prompt becomes active.
            if (serial != nullptr && serial->IsOpen()) {
                serial->WriteLine("BUZZER 900 70"); // Signal the start of a prompt without blocking gameplay.
            }
        }

        const bool hasPassedBird = obstacle.x + ObstacleWidth < PlayerX - BirdRadius;
        if (hasPassedBird && !obstacle.passed) {
            obstacle.passed = true; // Ensure score is awarded once even across many render frames.
            game.score += 1; // Award a point for navigating through the gate.
            if (obstacle.result == PromptResult::Waiting) {
                obstacle.result = PromptResult::Failed; // Expire a response that was never submitted.
                game.missedPrompts += 1; // Track prompt misses separately from wrong answers.
                LoseLife(game, "PROMPT MISSED!"); // A missed prompt costs one life.
            }
        }

        const float gapTop = obstacle.gapCenter - game.difficulty.gapHalfHeight; // Top edge of safe opening.
        const float gapBottom = obstacle.gapCenter + game.difficulty.gapHalfHeight; // Bottom edge of safe opening.
        const Rectangle topColumn{ obstacle.x, 0.0f, ObstacleWidth, gapTop }; // Solid upper part of the obstacle.
        const Rectangle bottomColumn{ obstacle.x, gapBottom, ObstacleWidth,
                                      ScreenHeight - GroundHeight - gapBottom }; // Solid lower part of the obstacle.
        const bool gateWasAnsweredCorrectly = obstacle.result == PromptResult::Correct; // A correct response opens this gate.
        const bool hitColumn = CheckCollisionRecs(birdBounds, topColumn)
            || CheckCollisionRecs(birdBounds, bottomColumn);
        if (hitColumn && !gateWasAnsweredCorrectly && !obstacle.collisionHandled) {
            obstacle.collisionHandled = true; // Count only the first overlapping frame as a collision.
            obstacle.result = PromptResult::Failed; // Mark the gate closed after a collision.
            LoseLife(game, "BUMP! TRY AGAIN!"); // Collision is counted as one mistake for this contact.
            if (game.screen != ScreenState::Playing) {
                return; // Stop as soon as collision loss ends the run.
            }
        }
    }

    game.obstacles.erase(
        std::remove_if(game.obstacles.begin(), game.obstacles.end(),
                       [](const Obstacle& obstacle) {
                           return obstacle.x + ObstacleWidth < -20.0f; // Remove gates fully off-screen.
                       }),
        game.obstacles.end()); // Erase only after the update loop to keep its references valid.
}

/**
 * @brief Draw one obstacle and visually distinguish the prompt result.
 * @param obstacle Obstacle whose geometry and state should be drawn.
 * @param difficulty Active settings supplying the safe-gap half-height.
 * @return No value; submits drawing commands to the current Raylib frame.
 */
void DrawObstacle(const Obstacle& obstacle, const DifficultySettings& difficulty) {
    const float gapTop = obstacle.gapCenter - difficulty.gapHalfHeight; // Upper boundary of the opening.
    const float gapBottom = obstacle.gapCenter + difficulty.gapHalfHeight; // Lower boundary of the opening.
    const Color columnColor = obstacle.result == PromptResult::Correct ? GREEN : Color{ 63, 151, 103, 255 }; // Green tint marks a solved gate.
    const Rectangle topColumn{ obstacle.x, 0.0f, ObstacleWidth, gapTop }; // Upper rectangular obstacle body.
    const Rectangle bottomColumn{ obstacle.x, gapBottom, ObstacleWidth,
                                  ScreenHeight - GroundHeight - gapBottom }; // Lower rectangular obstacle body.
    DrawRectangleRec(topColumn, columnColor); // Draw upper obstacle column.
    DrawRectangleRec(bottomColumn, columnColor); // Draw lower obstacle column.
    DrawRectangleLinesEx(topColumn, 3.0f, DARKGREEN); // Outline top column for crisp cartoon styling.
    DrawRectangleLinesEx(bottomColumn, 3.0f, DARKGREEN); // Outline bottom column for crisp cartoon styling.

    if (obstacle.result == PromptResult::Waiting) {
        const char* icon = obstacle.prompt == PromptType::Button ? "B" // Button prompt icon.
            : obstacle.prompt == PromptType::Yell ? "!" // Sound prompt icon.
            : "C"; // Cover prompt icon.
        DrawCircle(static_cast<int>(obstacle.x + ObstacleWidth / 2.0f),
                   static_cast<int>(obstacle.gapCenter), 19.0f, GOLD); // Place cue badge in the safe gap.
        DrawText(icon, static_cast<int>(obstacle.x + ObstacleWidth / 2.0f - 7.0f),
                 static_cast<int>(obstacle.gapCenter - 11.0f), 22, DARKBROWN); // Label the required action.
    }
}

/**
 * @brief Draw a friendly bird from Raylib primitives without external assets.
 * @param centerY Vertical center of the bird in the playfield.
 * @return No value; draws the bird body, wing, beak, and eye.
 */
void DrawBird(float centerY) {
    DrawCircle(static_cast<int>(PlayerX), static_cast<int>(centerY), BirdRadius, GOLD); // Main round bird body.
    DrawEllipse(static_cast<int>(PlayerX - 3.0f), static_cast<int>(centerY + 4.0f),
                8.0f, 5.5f, ORANGE); // Wing gives the simple sprite a bit of character.
    DrawTriangle({ PlayerX + 13.0f, centerY - 2.0f }, // Beak tip near the bird's front.
                 { PlayerX + 26.0f, centerY + 2.0f }, // Lower beak point.
                 { PlayerX + 13.0f, centerY + 6.0f }, ORANGE); // Complete the small beak shape.
    DrawCircle(static_cast<int>(PlayerX + 6.0f), static_cast<int>(centerY - 5.0f), 3.0f, WHITE); // Eye white.
    DrawCircle(static_cast<int>(PlayerX + 7.0f), static_cast<int>(centerY - 5.0f), 1.5f, BLACK); // Pupil.
}

/**
 * @brief Draw the sky, parallax clouds, scrolling ground, and playfield border.
 * @param game Current game state supplies the ground scroll offset.
 * @return No value; draws the background before foreground objects.
 */
void DrawBackground(const Game& game) {
    ClearBackground(SKYBLUE); // Fill the entire frame with a calm blue sky.
    DrawCircle(790, 115, 44.0f, Fade(YELLOW, 0.85f)); // Decorative sun in the upper-right sky.
    DrawCircle(155, 130, 25.0f, Fade(WHITE, 0.85f)); // First cloud puff.
    DrawCircle(185, 125, 34.0f, Fade(WHITE, 0.85f)); // Larger middle cloud puff.
    DrawCircle(218, 134, 23.0f, Fade(WHITE, 0.85f)); // Last cloud puff.
    DrawCircle(535, 200, 24.0f, Fade(WHITE, 0.65f)); // Second cloud puff.
    DrawCircle(565, 194, 33.0f, Fade(WHITE, 0.65f)); // Second cloud's middle puff.
    DrawCircle(598, 205, 22.0f, Fade(WHITE, 0.65f)); // Second cloud's final puff.
    DrawRectangle(0, ScreenHeight - static_cast<int>(GroundHeight), ScreenWidth,
                  static_cast<int>(GroundHeight), Color{ 218, 190, 112, 255 }); // Ground base.
    DrawRectangle(0, ScreenHeight - static_cast<int>(GroundHeight), ScreenWidth, 10,
                  Color{ 108, 190, 95, 255 }); // Bright grass strip separates ground and sky.
    for (int x = -48; x < ScreenWidth + 48; x += 48) { // Tile ground marks across the whole width.
        const int tileX = x - static_cast<int>(game.groundOffset); // Shift tiles for scrolling animation.
        DrawLine(tileX, ScreenHeight - 35, tileX + 24, ScreenHeight - 35,
                 Color{ 189, 160, 91, 255 }); // Small repeated soil mark indicates movement.
    }
}

/**
 * @brief Draw the gameplay HUD, active instruction, lives, and feedback message.
 * @param game Current game state containing score and prompt information.
 * @param serialConnected Whether an Arduino serial connection is currently open.
 * @return No value; draws text and panels on top of the playfield.
 */
void DrawHud(Game& game, bool serialConnected) {
    DrawRectangle(0, 0, ScreenWidth, 64, Fade(DARKBLUE, 0.76f)); // Dark header improves text contrast.
    DrawText(TextFormat("SCORE  %03i", game.score), 24, 18, 28, WHITE); // Passed gate count.
    DrawText(TextFormat("LIVES  %i", game.lives), 245, 20, 23, GOLD); // Remaining mistakes.
    DrawText(TextFormat("GOOD  %i", game.correctResponses), 388, 22, 20, LIME); // Correct prompt count.
    const float averageReaction = game.reactionCount > 0
        ? game.reactionTimeTotal / game.reactionCount : 0.0f; // Average correct response time so far.
    DrawText(game.reactionCount > 0 ? TextFormat("AVG RT %.2fs", averageReaction) : "AVG RT --",
             525, 24, 17, WHITE); // Show reaction timing separately from the gate score.
    const char* connectionText = serialConnected ? "ARDUINO CONNECTED" : "KEYBOARD MODE"; // Explain active input source.
    const Color connectionColor = serialConnected ? LIME : RAYWHITE; // Highlight the physical-device connection.
    DrawText(connectionText, ScreenWidth - MeasureText(connectionText, 16) - 20, 24, 16,
             connectionColor); // Keep status at the upper-right edge.

    Obstacle* active = FindActivePrompt(game); // Identify the prompt that currently accepts an action.
    if (active != nullptr) {
        const char* promptText = PromptActionText(active->prompt); // Convert prompt enum into clear instructions.
        const int promptWidth = MeasureText(promptText, 30); // Center instruction based on rendered width.
        DrawRectangle(ScreenWidth / 2 - 230, 82, 460, 58, Fade(WHITE, 0.9f)); // High-contrast prompt panel.
        DrawText(promptText, ScreenWidth / 2 - promptWidth / 2, 96, 30, MAROON); // Display current required action.
    }

    if (game.feedbackTimer > 0.0f && !game.feedbackText.empty()) {
        const int textWidth = MeasureText(game.feedbackText.c_str(), 24); // Measure temporary feedback for centering.
        DrawText(game.feedbackText.c_str(), ScreenWidth / 2 - textWidth / 2, 164, 24,
                 game.feedbackText == "GREAT JOB!" ? DARKGREEN : MAROON); // Positive and corrective messages use distinct colors.
    }
}

/**
 * @brief Draw the title screen and available difficulty/control choices.
 * @param game Current state supplies the selected difficulty label.
 * @return No value; draws the title menu.
 */
void DrawTitle(const Game& game) {
    DrawBackground(game); // Reuse the same setting so menu and game feel connected.
    DrawRectangle(150, 105, 660, 420, Fade(DARKBLUE, 0.82f)); // Centered title card.
    const char* title = "FOCUSBIRD"; // Project name presented prominently.
    DrawText(title, ScreenWidth / 2 - MeasureText(title, 58) / 2, 137, 58, GOLD); // Large playful heading.
    const char* subtitle = "Fly through the gates. Follow the prompts!"; // One-line explanation for new players.
    DrawText(subtitle, ScreenWidth / 2 - MeasureText(subtitle, 22) / 2, 216, 22, RAYWHITE); // Center the purpose statement.
    DrawText(TextFormat("Selected: %s", game.difficulty.label), ScreenWidth / 2 - 92, 267, 22, WHITE); // Current difficulty.
    DrawText("1  EASY       2  CHALLENGE", ScreenWidth / 2 - 162, 308, 21, LIME); // Keyboard difficulty shortcuts.
    DrawText("ENTER  START GAME", ScreenWidth / 2 - 132, 356, 23, GOLD); // Start instruction.
    DrawText("SPACE / UP = flap (button)", 270, 411, 18, RAYWHITE); // Keyboard button fallback.
    DrawText("Y = yell input       C = cover input", 270, 439, 18, RAYWHITE); // Keyboard sensor fallbacks.
    DrawText("Arduino events are accepted only during the matching prompt.",
             207, 481, 17, LIGHTGRAY); // Clarify timing and input matching.
}

/**
 * @brief Draw active gameplay objects and the information HUD.
 * @param game Current mutable game state; prompt lookup is observational.
 * @param serialConnected Whether the optional hardware link is open.
 * @return No value; draws one complete play frame.
 */
void DrawPlaying(Game& game, bool serialConnected) {
    DrawBackground(game); // Draw sky and floor first so obstacles overlay them.
    for (const Obstacle& obstacle : game.obstacles) { // Paint each active gate behind the bird.
        DrawObstacle(obstacle, game.difficulty); // Gate color and icon communicate its state/type.
    }
    DrawBird(game.birdY); // Fixed-x bird is drawn after obstacles for clear overlap visibility.
    DrawHud(game, serialConnected); // Add score, lives, hardware status, and active prompt.
}

/**
 * @brief Draw the run summary and replay controls.
 * @param game Current game state containing final counters.
 * @return No value; draws the results card.
 */
void DrawGameOver(const Game& game) {
    DrawBackground(game); // Preserve the play setting behind the results overlay.
    DrawRectangle(190, 105, 580, 438, Fade(DARKBLUE, 0.9f)); // Large centered results panel.
    const char* heading = "NICE FLYING!"; // Friendly title avoids framing failure harshly.
    DrawText(heading, ScreenWidth / 2 - MeasureText(heading, 46) / 2, 137, 46, GOLD); // Show game-over heading.
    DrawText(TextFormat("Gates passed: %i", game.score), 340, 220, 25, WHITE); // Final gate score.
    DrawText(TextFormat("Correct prompts: %i", game.correctResponses), 340, 264, 22, LIME); // Prompt successes.
    DrawText(TextFormat("Wrong responses: %i", game.incorrectResponses), 340, 304, 22, ORANGE); // Wrong actions.
    DrawText(TextFormat("Missed prompts: %i", game.missedPrompts), 340, 344, 22, LIGHTGRAY); // Expired prompts.
    const float averageReaction = game.reactionCount > 0
        ? game.reactionTimeTotal / game.reactionCount : 0.0f; // Average successful response time.
    DrawText(game.reactionCount > 0
                 ? TextFormat("Reaction time: avg %.2fs  |  fastest %.2fs", averageReaction, game.fastestReactionTime)
                 : "Reaction time: no correct responses",
             250, 390, 20, SKYBLUE); // Report prompt timing independently from score and misses.
    DrawText(TextFormat("Play time: %.1f seconds", game.elapsedSeconds), 340, 426, 22, LIGHTGRAY); // Session duration.
    DrawText("ENTER = PLAY AGAIN     ESC = QUIT", 264, 480, 20, GOLD); // Restart/exit controls.
}

/**
 * @brief Print command-line usage for launching with or without an Arduino.
 * @param programName Executable name supplied by the operating system.
 * @return No value; writes usage to standard output.
 */
void PrintUsage(const char* programName) {
    std::cout << "FocusBird - Raylib + Arduino reaction game\n"
              << "Usage: " << programName << " [--serial PORT] [--help]\n"
              << "Examples:\n"
              << "  " << programName << "\n"
              << "  " << programName << " --serial /dev/ttyACM0\n"
              << "  " << programName << " --serial COM3\n";
}

/**
 * @brief Parse a requested optional serial device from command-line arguments.
 * @param argc Number of command-line arguments.
 * @param argv Argument values supplied by the operating system.
 * @param serialName Output string populated with the requested device path.
 * @return true when arguments are valid; false after printing help/error text.
 */
bool ParseArguments(int argc, char** argv, std::string& serialName) {
    for (int index = 1; index < argc; ++index) { // Walk every user-supplied option once.
        const std::string argument = argv[index]; // Copy for safe string comparisons.
        if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]); // Explain launch options without opening the game window.
            return false;
        }
        if (argument == "--serial") {
            if (index + 1 >= argc) {
                std::cerr << "Error: --serial requires a port name.\n"; // Report missing option value.
                PrintUsage(argv[0]); // Show a valid command form.
                return false;
            }
            serialName = argv[++index]; // Consume the next argument as the serial device.
        } else {
            std::cerr << "Unknown option: " << argument << '\n'; // Reject misspelled flags early.
            PrintUsage(argv[0]); // Give the user the supported syntax.
            return false;
        }
    }
    return true; // All supplied options were recognized.
}

} // namespace

/**
 * @brief Initialize Raylib, optional Arduino serial, and the FocusBird game loop.
 * @param argc Count of command-line arguments.
 * @param argv Command-line argument vector.
 * @return Process exit status: zero after normal exit, nonzero for bad arguments.
 */
int main(int argc, char** argv) {
    std::string serialName; // Empty means the player explicitly uses keyboard-only controls.
    if (!ParseArguments(argc, argv, serialName)) {
        for (int index = 1; index < argc; ++index) { // Check whether any argument requested help.
            if (std::string(argv[index]) == "--help" || std::string(argv[index]) == "-h") {
                return 0; // Help is a successful informational exit even after other valid options.
            }
        }
        return 1; // All other parse failures indicate invalid launch arguments.
    }

    SerialPort serial; // Own the optional Arduino connection for the application's lifetime.
    bool serialConnected = false; // Status shown to players and updated if the device disconnects.
    if (!serialName.empty()) {
        serialConnected = serial.Open(serialName, 115200); // Connect using the firmware's configured baud.
        if (!serialConnected) {
            std::cerr << "Could not open serial port '" << serialName
                      << "'. Continuing in keyboard mode.\n"; // Hardware is optional, not a launch blocker.
        }
    }

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT); // Smooth primitive edges and synchronize drawing.
    InitWindow(ScreenWidth, ScreenHeight, "FocusBird - Focus, flap, fly!"); // Create the Raylib desktop window.
    SetTargetFPS(60); // Request a steady frame cadence while physics uses measured frame time.

    Game game; // Store all mutable gameplay state in one easy-to-reset aggregate.
    bool running = true; // Main-loop flag remains true until Escape or window close.
    while (running && !WindowShouldClose()) { // Continue until the player/window requests exit.
        const float frameTime = std::min(GetFrameTime(), 0.05f); // Clamp long stalls to avoid large physics jumps.

        if (IsKeyPressed(KEY_ESCAPE)) {
            running = false; // Escape exits from any screen without changing the run result.
        }

        if (serialConnected) {
            const std::vector<std::string> serialLines = serial.ReadLines(); // Collect events before state-specific input.
            if (game.screen == ScreenState::Playing) {
                for (const std::string& line : serialLines) { // Route only events received during active gameplay.
                    Action hardwareAction = Action::Button; // Temporary output populated for recognized event lines.
                    if (ParseSerialAction(line, hardwareAction)) {
                        HandleAction(game, hardwareAction, &serial); // Treat physical and keyboard actions identically.
                    }
                }
            }
            serialConnected = serial.IsOpen(); // Detect unplugging/errors on every screen, not just gameplay.
        }

        if (game.screen == ScreenState::Title) {
            if (IsKeyPressed(KEY_ONE)) {
                SetDifficulty(game, false); // Select relaxed speed and a wider safe gap.
            }
            if (IsKeyPressed(KEY_TWO)) {
                SetDifficulty(game, true); // Select faster obstacles and a smaller safe gap.
            }
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                StartGame(game); // Begin after the user has selected a difficulty.
            }
        } else if (game.screen == ScreenState::GameOver) {
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                StartGame(game); // Reset counters and replay using the current difficulty.
            }
        } else {
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP)) {
                HandleAction(game, Action::Button, serialConnected ? &serial : nullptr); // Keyboard flap/button prompt.
            }
            if (IsKeyPressed(KEY_Y)) {
                HandleAction(game, Action::Sound, serialConnected ? &serial : nullptr); // Simulate sound sensor event.
            }
            if (IsKeyPressed(KEY_C)) {
                HandleAction(game, Action::Light, serialConnected ? &serial : nullptr); // Simulate covered light sensor.
            }

            UpdateGame(game, frameTime, serialConnected ? &serial : nullptr); // Advance physics after handling events.
        }

        BeginDrawing(); // Begin submitting the current rendered frame.
        if (game.screen == ScreenState::Title) {
            DrawTitle(game); // Show menu and controls while waiting to start.
        } else if (game.screen == ScreenState::Playing) {
            DrawPlaying(game, serialConnected); // Show the live game and its HUD.
        } else {
            DrawGameOver(game); // Show statistics and replay instructions.
        }
        EndDrawing(); // Present the completed frame to the window.
    }

    CloseWindow(); // Release Raylib display/audio/input resources before process exit.
    return 0; // Report normal completion to the operating system.
}
