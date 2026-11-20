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

const uint8_t BUTTON_PIN = 2; // Grove button digital output; its pull-down makes a press read HIGH.
const uint8_t BUZZER_PIN = 3; // Grove buzzer digital output; uses tone() for feedback.
const uint8_t SOUND_SENSOR_PIN = A0; // Analog output from the Grove sound sensor.
const uint8_t LIGHT_SENSOR_PIN = A1; // Analog output from the Grove light sensor.

const int SOUND_DELTA_THRESHOLD = 450; // Yell must clearly exceed the calibrated room-noise level.
const int SOUND_REARM_DELTA = 85; // Sound must settle near baseline before another yell can count.
const int LIGHT_COVER_DELTA = 100; // Covering must lower the light reading this far from ambient.
const int LIGHT_RELEASE_DELTA = 45; // Light must return near ambient before another cover can count.
const unsigned long BUTTON_DEBOUNCE_MS = 35; // Ignore button contact bounce shorter than this interval.
const unsigned long SOUND_COOLDOWN_MS = 1000; // Separate distinct yells and suppress ambient-noise repeats.
const unsigned long SOUND_CALIBRATION_MS = 1000; // Learn the room level before accepting sound events.
const unsigned long SENSOR_SAMPLE_INTERVAL_MS = 5; // Sample analog sensors without busy waiting.
const unsigned long SENSOR_REPORT_INTERVAL_MS = 500; // Keep raw readings available for quick hardware checks.
const unsigned int SOUND_CONFIRM_SAMPLES = 3; // Require a short sustained increase, not one noisy ADC spike.
const unsigned int SOUND_REARM_SAMPLES = 20; // Require 100 ms of near-baseline sound before rearming.
const unsigned int LIGHT_CONFIRM_SAMPLES = 3; // Reject one-sample light fluctuations.
const unsigned int LIGHT_RELEASE_SAMPLES = 5; // Require a stable uncover before rearming the sensor.
const float SOUND_CALIBRATION_ALPHA = 0.10f; // Quickly learn ambient sound during the startup window.
const float SOUND_BASELINE_ALPHA = 0.025f; // Slowly track the room level without chasing a yell.
const float LIGHT_BASELINE_ALPHA = 0.005f; // Track slow room-light changes without following a hand cover.

// ------------------------------- Runtime state -------------------------------

bool stableButtonPressed = false; // Debounced state used to report one event per button press.
bool lastRawButtonPressed = false; // Most recent electrical state used to detect transitions.
unsigned long buttonChangeStartedAt = 0; // Time when the un-debounced button value last changed.
unsigned long lastSensorSampleAt = 0; // Time of the last non-blocking analog sensor sample.
unsigned long lastSensorReportAt = 0; // Time of the last raw sensor diagnostic line.
unsigned long lastSoundEventAt = 0; // Time of the last accepted sound trigger.
unsigned long soundCalibrationStartedAt = 0; // Start of the quiet-room baseline learning window.
float soundBaseline = 0.0f; // Slowly changing analog room-noise estimate.
float lightBaseline = 0.0f; // Ambient light level captured while the sensor is uncovered.
unsigned int soundAboveThresholdSamples = 0; // Consecutive high readings required to accept a yell.
unsigned int soundQuietSamples = 0; // Consecutive quiet readings required to rearm sound detection.
unsigned int lightCoveredSamples = 0; // Consecutive covered readings required to accept a cover.
unsigned int lightReleasedSamples = 0; // Consecutive bright readings required to rearm light detection.
bool soundArmed = true; // One sound event per burst until the reading returns near its baseline.
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
 * @brief Emit raw sensor values for calibration through Arduino Serial Monitor.
 * @param nowMs Current Arduino millis() value used to rate-limit diagnostics.
 * @param soundReading Current raw sound ADC value.
 * @param soundDelta Difference between sound reading and learned room baseline.
 * @param lightReading Current raw light ADC value.
 * @return No value; the desktop game ignores these diagnostic lines.
 */
void ReportSensorReadings(unsigned long nowMs, int soundReading, int soundDelta, int lightReading) {
  if ((nowMs - lastSensorReportAt) < SENSOR_REPORT_INTERVAL_MS) {
    return;
  }
  lastSensorReportAt = nowMs;

  Serial.print("SENSORS ");
  Serial.print(soundReading);
  Serial.print(' ');
  Serial.print(static_cast<int>(soundBaseline + 0.5f));
  Serial.print(' ');
  Serial.print(soundDelta);
  Serial.print(' ');
  Serial.print(lightReading);
  Serial.print(' ');
  Serial.println(static_cast<int>(lightBaseline + 0.5f));
}

/**
 * @brief Sample and debounce the Grove button, reporting only a new press.
 * @param nowMs Current Arduino millis() value used for debounce timing.
 * @return No value; emits BTN once when a stable press begins.
 */
void UpdateButton(unsigned long nowMs) {
  const bool rawPressed = (digitalRead(BUTTON_PIN) == HIGH); // Standard Grove button is HIGH while pressed.
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
  float soundDifference = soundReading - soundBaseline; // Compare signal against room noise.
  if ((nowMs - soundCalibrationStartedAt) < SOUND_CALIBRATION_MS) {
    soundBaseline += SOUND_CALIBRATION_ALPHA * soundDifference; // Learn the room instead of triggering at startup.
    soundAboveThresholdSamples = 0;
    soundArmed = true;
  } else if (soundArmed) {
    if (soundDifference >= SOUND_DELTA_THRESHOLD) {
      if (soundAboveThresholdSamples < SOUND_CONFIRM_SAMPLES) {
        soundAboveThresholdSamples += 1; // A sustained increase is more likely a deliberate yell.
      }
      if (soundAboveThresholdSamples >= SOUND_CONFIRM_SAMPLES
          && (nowMs - lastSoundEventAt) >= SOUND_COOLDOWN_MS) {
        lastSoundEventAt = nowMs; // Start the cooldown when the confirmed event is emitted.
        soundArmed = false; // Do not repeat while the same loud sound remains present.
        soundQuietSamples = 0;
        soundAboveThresholdSamples = 0;
        SendEvent("SOUND"); // The game accepts it only if a YELL prompt is currently active.
      }
    } else {
      soundAboveThresholdSamples = 0;
      soundBaseline += SOUND_BASELINE_ALPHA * soundDifference; // Adapt only below the trigger level.
    }
  } else {
    soundBaseline += SOUND_BASELINE_ALPHA * soundDifference; // Let a sustained ambient change become the new baseline.
    soundDifference = soundReading - soundBaseline;
    if (soundDifference <= SOUND_REARM_DELTA) {
      if (soundQuietSamples < SOUND_REARM_SAMPLES) {
        soundQuietSamples += 1;
      }
      if (soundQuietSamples >= SOUND_REARM_SAMPLES
          && (nowMs - lastSoundEventAt) >= SOUND_COOLDOWN_MS) {
        soundArmed = true; // Re-arm only after quiet and the cooldown have both elapsed.
        soundQuietSamples = 0;
      }
    } else {
      soundQuietSamples = 0; // Keep the detector latched through a sustained noisy interval.
    }
  }

  const int lightReading = analogRead(LIGHT_SENSOR_PIN); // Read light level for cover detection.
  const float lightDrop = lightBaseline - lightReading; // Standard Grove analog light values fall when covered.
  if (!lightCovered) {
    if (lightDrop >= LIGHT_COVER_DELTA) {
      if (lightCoveredSamples < LIGHT_CONFIRM_SAMPLES) {
        lightCoveredSamples += 1; // Reject a brief ADC fluctuation before accepting a cover.
      }
      if (lightCoveredSamples >= LIGHT_CONFIRM_SAMPLES) {
        lightCovered = true; // Latch one cover action until the sensor becomes bright again.
        lightCoveredSamples = 0;
        lightReleasedSamples = 0;
        SendEvent("LIGHT");
      }
    } else {
      lightCoveredSamples = 0;
      lightBaseline += LIGHT_BASELINE_ALPHA * (lightReading - lightBaseline);
    }
  } else if (lightReading >= (lightBaseline - LIGHT_RELEASE_DELTA)) {
    if (lightReleasedSamples < LIGHT_RELEASE_SAMPLES) {
      lightReleasedSamples += 1;
    }
    if (lightReleasedSamples >= LIGHT_RELEASE_SAMPLES) {
      lightCovered = false; // Require a stable uncover before another cover event can be sent.
      lightReleasedSamples = 0;
      lightBaseline += LIGHT_BASELINE_ALPHA * (lightReading - lightBaseline);
    }
  } else {
    lightReleasedSamples = 0;
  }

  const int reportedSoundDelta = soundReading - static_cast<int>(soundBaseline + 0.5f);
  ReportSensorReadings(nowMs, soundReading, reportedSoundDelta, lightReading);
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
  pinMode(BUTTON_PIN, INPUT); // Grove button module already provides a pull-down resistor.
  pinMode(BUZZER_PIN, OUTPUT); // Configure the buzzer as an Arduino-driven output.
  pinMode(SOUND_SENSOR_PIN, INPUT); // Select analog input mode for sound measurements.
  pinMode(LIGHT_SENSOR_PIN, INPUT); // Select analog input mode for light measurements.

  Serial.begin(115200); // Must match the desktop game's serial-port configuration.
  const int initialSound = analogRead(SOUND_SENSOR_PIN); // Seed baseline to avoid a startup false trigger.
  soundBaseline = initialSound; // Use current room conditions as the starting sound level.
  const int initialLight = analogRead(LIGHT_SENSOR_PIN); // Capture the actual room-light level at startup.
  lightBaseline = initialLight; // Learn the actual room-light level instead of assuming one ADC threshold.
  lightCovered = false; // Do not report a cover that predates startup.
  lightCoveredSamples = 0;
  lightReleasedSamples = 0;
  soundAboveThresholdSamples = 0;
  soundQuietSamples = 0;
  soundArmed = true;
  lastRawButtonPressed = (digitalRead(BUTTON_PIN) == HIGH); // Initialize debounce from the actual button state.
  stableButtonPressed = lastRawButtonPressed; // Avoid a phantom BTN event during initial stabilization.
  buttonChangeStartedAt = millis(); // Start the button debounce clock at boot.
  soundCalibrationStartedAt = millis(); // Give the sensor a full second to learn the room before game input.
  lastSensorSampleAt = millis();
  lastSensorReportAt = millis();
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
