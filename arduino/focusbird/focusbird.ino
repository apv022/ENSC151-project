/*
 * FocusBird Arduino firmware
 *
 * Reads three Grove-style components and exchanges newline messages with the
 * Raylib game over USB serial at 115200 baud:
 *   Arduino -> game: BTN, SOUND, LIGHT
 *   Game -> Arduino: BUZZER <frequencyHz> <durationMs>
 *
 * Pin numbers and analog thresholds are intentionally collected below so the
 * sketch can be adjusted to the exact Grove board and classroom lighting.
 */

#include <Arduino.h>
#include <stdio.h>

// ---------------------------- Hardware configuration -------------------------

const uint8_t BUTTON_PIN = 2; // Grove button digital output; active LOW with INPUT_PULLUP.
const uint8_t BUZZER_PIN = 3; // Grove buzzer digital output; uses tone() for feedback.
const uint8_t SOUND_SENSOR_PIN = A0; // Analog output from the Grove sound sensor.
const uint8_t LIGHT_SENSOR_PIN = A1; // Analog output from the Grove light sensor.

const int SOUND_DELTA_THRESHOLD = 95; // Sound must rise this far above the learned room baseline.
const int LIGHT_COVERED_THRESHOLD = 320; // Reading at/below this value means the sensor is covered.
const int LIGHT_RELEASE_HYSTERESIS = 70; // Extra light before another cover event can be armed.
const unsigned long BUTTON_DEBOUNCE_MS = 35; // Ignore button contact bounce shorter than this interval.
const unsigned long SOUND_COOLDOWN_MS = 500; // Limit repeated sound events from one yell/noise.
const unsigned long SENSOR_SAMPLE_INTERVAL_MS = 5; // Sample analog sensors without busy waiting.
const float SOUND_BASELINE_ALPHA = 0.015f; // Slow baseline adaptation for changing room noise.

// ------------------------------- Runtime state -------------------------------

bool stableButtonPressed = false; // Debounced state used to report one event per button press.
bool lastRawButtonPressed = false; // Most recent electrical state used to detect transitions.
unsigned long buttonChangeStartedAt = 0; // Time when the un-debounced button value last changed.
unsigned long lastSensorSampleAt = 0; // Time of the last non-blocking analog sensor sample.
unsigned long lastSoundEventAt = 0; // Time of the last accepted sound trigger.
float soundBaseline = 0.0f; // Slowly changing analog room-noise estimate.
bool lightCovered = false; // Hysteresis state prevents repeated events while the sensor stays covered.
char commandBuffer[64] = {}; // Fixed-size command buffer avoids dynamic serial parsing allocations.
size_t commandLength = 0; // Number of command characters currently collected.

/**
 * @brief Send one named input event to the computer game.
 * @param eventName Protocol event token: BTN, SOUND, or LIGHT.
 * @return No value; writes the event followed by a newline to USB serial.
 */
void SendEvent(const char* eventName) {
  Serial.println(eventName); // A line boundary lets the C++ game parse partial USB packets safely.
}

/**
 * @brief Sample and debounce the Grove button, reporting only a new press.
 * @param nowMs Current Arduino millis() value used for debounce timing.
 * @return No value; emits BTN once when a stable press begins.
 */
void UpdateButton(unsigned long nowMs) {
  const bool rawPressed = (digitalRead(BUTTON_PIN) == LOW); // INPUT_PULLUP makes LOW the pressed state.
  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed; // Remember the latest electrical transition.
    buttonChangeStartedAt = nowMs; // Restart the debounce timer after a contact change.
  }

  if ((nowMs - buttonChangeStartedAt) >= BUTTON_DEBOUNCE_MS
      && stableButtonPressed != rawPressed) {
    stableButtonPressed = rawPressed; // Accept the new state only after it remains stable.
    if (stableButtonPressed) {
      SendEvent("BTN"); // A press event both flaps the bird and can answer a button prompt.
    }
  }
}

/**
 * @brief Sample sound/light sensors and emit threshold-crossing events.
 * @param nowMs Current Arduino millis() value for sampling and cooldown timing.
 * @return No value; emits SOUND or LIGHT only on a newly detected action.
 */
void UpdateAnalogSensors(unsigned long nowMs) {
  if ((nowMs - lastSensorSampleAt) < SENSOR_SAMPLE_INTERVAL_MS) {
    return; // Keep the loop responsive to serial commands and button changes.
  }
  lastSensorSampleAt = nowMs; // Schedule the next sample relative to this one.

  const int soundReading = analogRead(SOUND_SENSOR_PIN); // Read sound sensor's current analog level.
  const float soundDifference = soundReading - soundBaseline; // Compare signal against room noise.
  if (soundDifference > SOUND_DELTA_THRESHOLD
      && (nowMs - lastSoundEventAt) >= SOUND_COOLDOWN_MS) {
    lastSoundEventAt = nowMs; // Start cooldown before sending to suppress repeated peaks.
    SendEvent("SOUND"); // The game accepts it only if a YELL prompt is currently active.
  } else if (soundDifference < SOUND_DELTA_THRESHOLD * 0.5f) {
    soundBaseline += SOUND_BASELINE_ALPHA * (soundReading - soundBaseline); // Adapt only in quiet periods.
  }

  const int lightReading = analogRead(LIGHT_SENSOR_PIN); // Read light level for cover detection.
  if (!lightCovered && lightReading <= LIGHT_COVERED_THRESHOLD) {
    lightCovered = true; // Lock the sensor until it is uncovered past the hysteresis point.
    SendEvent("LIGHT"); // One cover action is sent, even if the sensor remains covered.
  } else if (lightCovered
             && lightReading >= LIGHT_COVERED_THRESHOLD + LIGHT_RELEASE_HYSTERESIS) {
    lightCovered = false; // Re-arm only after a sufficiently clear uncover transition.
  }
}

/**
 * @brief Parse and execute one computer-to-Arduino buzzer command.
 * @param command Null-terminated line, such as "BUZZER 1200 100".
 * @return No value; a valid positive frequency plays a bounded tone.
 */
void HandleSerialCommand(const char* command) {
  unsigned int frequencyHz = 0; // Parsed buzzer pitch; zero means silence/invalid input.
  unsigned int durationMs = 0; // Parsed tone duration in milliseconds.
  if (sscanf(command, "BUZZER %u %u", &frequencyHz, &durationMs) == 2) {
    if (frequencyHz == 0 || durationMs == 0) {
      noTone(BUZZER_PIN); // A zero-valued command explicitly stops any tone.
    } else {
      const unsigned int safeDuration = min(durationMs, 2000U); // Bound host requests to two seconds.
      tone(BUZZER_PIN, frequencyHz, safeDuration); // Play non-blocking feedback while inputs continue.
    }
  }
}

/**
 * @brief Collect incoming USB serial bytes and dispatch each complete line.
 * @param none Reads from the Arduino USB serial receive buffer.
 * @return No value; partial commands stay buffered until newline arrives.
 */
void ReadComputerCommands() {
  while (Serial.available() > 0) { // Drain all waiting bytes without delaying the sensor loop.
    const char incoming = static_cast<char>(Serial.read()); // Consume one byte from the host.
    if (incoming == '\n' || incoming == '\r') {
      if (commandLength > 0) {
        commandBuffer[commandLength] = '\0'; // Terminate the complete command for sscanf.
        HandleSerialCommand(commandBuffer); // Execute only a fully received line.
        commandLength = 0; // Start collecting the next protocol message.
      }
    } else if (commandLength < sizeof(commandBuffer) - 1) {
      commandBuffer[commandLength++] = incoming; // Append while reserving space for the terminator.
    } else {
      commandLength = 0; // Drop oversized malformed input and resynchronize at the next newline.
    }
  }
}

/**
 * @brief Initialize sensor pins and the 115200-baud host protocol.
 * @param none Arduino invokes setup once after reset.
 * @return No value; emits a READY line after initialization.
 */
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Use the internal pull-up for a common two-wire Grove button.
  pinMode(BUZZER_PIN, OUTPUT); // Configure the buzzer as an Arduino-driven output.
  pinMode(SOUND_SENSOR_PIN, INPUT); // Select analog input mode for sound measurements.
  pinMode(LIGHT_SENSOR_PIN, INPUT); // Select analog input mode for light measurements.

  Serial.begin(115200); // Must match the desktop game's serial-port configuration.
  const int initialSound = analogRead(SOUND_SENSOR_PIN); // Seed baseline to avoid a startup false trigger.
  soundBaseline = initialSound; // Use current room conditions as the starting sound level.
  const int initialLight = analogRead(LIGHT_SENSOR_PIN); // Determine whether the light sensor starts covered.
  lightCovered = (initialLight <= LIGHT_COVERED_THRESHOLD); // Do not report a cover that predates startup.
  lastRawButtonPressed = (digitalRead(BUTTON_PIN) == LOW); // Initialize debounce from the actual button state.
  stableButtonPressed = lastRawButtonPressed; // Avoid a phantom BTN event during initial stabilization.
  buttonChangeStartedAt = millis(); // Start the button debounce clock at boot.
  lastSoundEventAt = millis() - SOUND_COOLDOWN_MS; // Permit the first genuine sound promptly.
  Serial.println("READY"); // Optional diagnostic line; the game safely ignores unknown messages.
}

/**
 * @brief Run non-blocking input sampling and serial command handling forever.
 * @param none Arduino invokes loop repeatedly after setup has completed.
 * @return No value; control returns to the Arduino runtime for the next iteration.
 */
void loop() {
  const unsigned long nowMs = millis(); // Snapshot time once for consistent checks this iteration.
  ReadComputerCommands(); // Handle buzzer feedback before the next sensor sample.
  UpdateButton(nowMs); // Emit a debounced event without blocking other hardware work.
  UpdateAnalogSensors(nowMs); // Sample sound and light at the configured interval.
}
