/*
 * FocusBird Arduino firmware
 *
 * Reads Grove components and exchanges newline messages with the game over
 * USB serial at 115200 baud:
 *   Arduino -> game: BTN, SOUND, LIGHT, and SENSORS <soundPeak> <light> <soundCutoff> <lightCutoff>
 *   Game -> Arduino: BUZZER <frequencyHz> <durationMs>
 *
 * Pins and thresholds are collected below so the sketch can be adjusted to the
 * exact Grove board and room.
 */

// ---------------------------- Hardware configuration -------------------------

const uint8_t BUTTON_PIN = 2; // Grove button; its onboard pull-down makes a press read HIGH.
const uint8_t BUZZER_PIN = 3; // Grove buzzer, driven with tone().
const uint8_t SOUND_SENSOR_PIN = A0; // Grove sound sensor (analog).
const uint8_t LIGHT_SENSOR_PIN = A1; // Grove light sensor (analog).

const int ANALOG_MAX_VALUE = 1023; // 10-bit ADC on Uno-compatible boards (UNO R4 too, by default).

// Sound. The Grove sound sensor outputs the sound *wave*, which swings up and
// down hundreds of times a second, so a single reading is mostly luck. Instead
// the sensor is read on every loop pass and the loudest reading in each short
// window is used: a yell reliably produces high peaks in every window.
const int SOUND_RAW_THRESHOLD = 700; // A window is "loud" when its peak reading is above this.
const unsigned long SOUND_WINDOW_MS = 20; // Window length; covers at least two cycles of any voice.
const unsigned int SOUND_CONFIRM_WINDOWS = 2; // Loud windows in a row needed (40 ms), so a click is ignored.
const unsigned long SOUND_REARM_MS = 500; // After a SOUND, the next can fire this much later, even mid-yell.
const unsigned long BUZZER_ECHO_MS = 100; // Ignore sound while the buzzer plays plus this long after it.

// Light.
const int LIGHT_RAW_THRESHOLD = ANALOG_MAX_VALUE / 10; // LIGHT fires at or below 10% of full scale (covered).
const int LIGHT_RAW_RELEASE_THRESHOLD = ANALOG_MAX_VALUE * 12 / 100; // Rearm above 12% (hysteresis gap stops chatter).
const unsigned int LIGHT_CONFIRM_SAMPLES = 3; // Dark samples in a row needed, so one ADC spike is ignored.
const unsigned long LIGHT_SAMPLE_INTERVAL_MS = 5; // Light sampling period.

const unsigned long BUTTON_DEBOUNCE_MS = 35; // Ignore button contact bounce shorter than this.
const unsigned long SENSOR_REPORT_INTERVAL_MS = 500; // How often the SENSORS diagnostic line is printed.

// ------------------------------- Runtime state -------------------------------

bool stableButtonPressed = false; // Debounced button state.
bool lastRawButtonPressed = false; // Last raw reading, to detect changes.
unsigned long buttonChangedAt = 0; // When the raw reading last changed.

unsigned long soundWindowStartedAt = 0; // When the current sound window began.
int soundWindowPeak = 0; // Loudest reading in the current window.
unsigned int loudWindows = 0; // Loud windows in a row so far.
bool soundArmed = true; // False for SOUND_REARM_MS after each SOUND.
unsigned long lastSoundEventAt = 0; // When SOUND was last sent.
unsigned long toneStartedAt = 0; // When the buzzer last started a tone.
unsigned long toneIgnoreMs = 0; // How long after toneStartedAt to ignore the microphone.

unsigned long lastLightSampleAt = 0; // When the light sensor was last read.
int lastLightReading = 0; // Most recent light reading (for the SENSORS line).
unsigned int darkSamples = 0; // Dark samples in a row so far.
bool lightCovered = false; // True after LIGHT until the sensor is uncovered again.

unsigned long lastReportAt = 0; // When SENSORS was last printed.
int reportSoundPeak = 0; // Loudest sound reading since the last SENSORS line.

char commandBuffer[32]; // Incoming command line from the game.
size_t commandLength = 0; // Characters collected so far.

// ------------------------------- Input handling ------------------------------

/**
 * @brief Debounce the Grove button and send BTN once when a press begins.
 * A raw change only counts after it has held for BUTTON_DEBOUNCE_MS, so
 * contact bounce never produces extra presses. Holding the button sends one BTN.
 * @param nowMs Current millis() value.
 * @return No value; may print "BTN".
 */
void UpdateButton(unsigned long nowMs) {
  const bool rawPressed = digitalRead(BUTTON_PIN) == HIGH; // Pressed reads HIGH on the Grove module.
  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed;
    buttonChangedAt = nowMs; // Restart the debounce timer on every contact change.
  }
  if (rawPressed != stableButtonPressed && nowMs - buttonChangedAt >= BUTTON_DEBOUNCE_MS) {
    stableButtonPressed = rawPressed; // Accept the state once it has been stable.
    if (stableButtonPressed) {
      Serial.println("BTN"); // Only on press, not on release.
    }
  }
}

/**
 * @brief Detect a yell and send SOUND.
 * Reads the microphone on every loop pass and keeps the loudest reading of each
 * SOUND_WINDOW_MS window. SOUND_CONFIRM_WINDOWS loud windows in a row send SOUND.
 * After that, another SOUND can follow SOUND_REARM_MS later even if the noise
 * never stops, so a yell that starts early still counts once the prompt opens.
 * Windows overlapping a buzzer tone are ignored, so the game's own beep can't
 * answer a prompt.
 * @param nowMs Current millis() value.
 * @return No value; may print "SOUND".
 */
void UpdateSound(unsigned long nowMs) {
  const int reading = analogRead(SOUND_SENSOR_PIN);
  soundWindowPeak = max(soundWindowPeak, reading); // Catch the wave's peaks.
  reportSoundPeak = max(reportSoundPeak, reading);
  if (nowMs - soundWindowStartedAt < SOUND_WINDOW_MS) {
    return; // Window not finished yet.
  }

  // The window is complete: decide whether it was loud.
  const bool hearingBuzzer = nowMs - toneStartedAt < toneIgnoreMs;
  const bool loud = soundWindowPeak > SOUND_RAW_THRESHOLD && !hearingBuzzer;
  loudWindows = loud ? loudWindows + 1 : 0; // A quiet window breaks the streak.
  soundWindowPeak = 0; // Start the next window.
  soundWindowStartedAt = nowMs;

  if (!soundArmed && nowMs - lastSoundEventAt >= SOUND_REARM_MS) {
    soundArmed = true; // Enough time has passed; allow the next SOUND.
  }
  if (soundArmed && loudWindows >= SOUND_CONFIRM_WINDOWS) {
    Serial.println("SOUND");
    soundArmed = false;
    lastSoundEventAt = nowMs;
    loudWindows = 0; // The next SOUND needs a fresh run of loud windows.
  }
}

/**
 * @brief Detect covering the light sensor and send LIGHT once per cover.
 * Fires after LIGHT_CONFIRM_SAMPLES dark readings in a row, then waits until the
 * reading rises above the release threshold (uncovered) before it can fire again.
 * @param nowMs Current millis() value.
 * @return No value; may print "LIGHT".
 */
void UpdateLight(unsigned long nowMs) {
  if (nowMs - lastLightSampleAt < LIGHT_SAMPLE_INTERVAL_MS) {
    return; // Not time to sample yet.
  }
  lastLightSampleAt = nowMs;

  lastLightReading = analogRead(LIGHT_SENSOR_PIN);
  if (!lightCovered) {
    darkSamples = lastLightReading <= LIGHT_RAW_THRESHOLD ? darkSamples + 1 : 0; // Count dark samples in a row.
    if (darkSamples >= LIGHT_CONFIRM_SAMPLES) {
      lightCovered = true; // Latch: holding a hand over it sends only one LIGHT.
      darkSamples = 0;
      Serial.println("LIGHT");
    }
  } else if (lastLightReading > LIGHT_RAW_RELEASE_THRESHOLD) {
    lightCovered = false; // Uncovered: the next cover can count.
  }
}

/**
 * @brief Print the SENSORS tuning line twice a second (the game ignores it).
 * Format: SENSORS <soundPeak> <light> <soundThreshold> <lightThreshold>, where
 * soundPeak is the loudest sound reading since the previous line, the same kind
 * of value the yell detector compares against SOUND_RAW_THRESHOLD.
 * @param nowMs Current millis() value.
 * @return No value; may print "SENSORS ...".
 */
void ReportSensors(unsigned long nowMs) {
  if (nowMs - lastReportAt < SENSOR_REPORT_INTERVAL_MS) {
    return;
  }
  lastReportAt = nowMs;
  Serial.print("SENSORS ");
  Serial.print(reportSoundPeak);
  Serial.print(' ');
  Serial.print(lastLightReading);
  Serial.print(' ');
  Serial.print(SOUND_RAW_THRESHOLD);
  Serial.print(' ');
  Serial.println(LIGHT_RAW_THRESHOLD);
  reportSoundPeak = 0; // Start measuring the next half second.
}

// ------------------------------- Game commands -------------------------------

/**
 * @brief Read bytes from the game and, at each newline, run the command.
 * The only command is "BUZZER <frequencyHz> <durationMs>", which plays a tone.
 * Reading never blocks, so sensors keep being sampled while commands arrive.
 * @return No value; may start a buzzer tone.
 */
void ReadComputerCommands() {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read()); // One byte at a time.
    if (incoming == '\n' || incoming == '\r') { // End of a command line.
      commandBuffer[commandLength] = '\0'; // Terminate the string for sscanf.
      unsigned int frequencyHz = 0; // Tone pitch in Hz.
      unsigned int durationMs = 0; // Tone length in milliseconds.
      // Lines that aren't a valid BUZZER command (including blank lines) are ignored.
      if (sscanf(commandBuffer, "BUZZER %u %u", &frequencyHz, &durationMs) == 2 && frequencyHz > 0) {
        durationMs = min(durationMs, 2000U); // Cap at 2 s.
        tone(BUZZER_PIN, frequencyHz, durationMs); // Non-blocking.
        toneStartedAt = millis(); // Tell UpdateSound to ignore the microphone meanwhile.
        toneIgnoreMs = durationMs + BUZZER_ECHO_MS;
      }
      commandLength = 0; // Start collecting the next command.
    } else if (commandLength < sizeof(commandBuffer) - 1) { // Leave room for the terminator.
      commandBuffer[commandLength++] = incoming;
    } else {
      commandLength = 0; // Oversized garbage: drop it and resync at the next newline.
    }
  }
}

// ------------------------------- Arduino entry points ------------------------

/**
 * @brief Configure pins and serial once after reset (Arduino calls this first).
 * @return No value; prints "READY" when done.
 */
void setup() {
  pinMode(BUTTON_PIN, INPUT); // Not INPUT_PULLUP: the Grove module has its own pull-down.
  pinMode(BUZZER_PIN, OUTPUT); // Analog pins need no pinMode for analogRead().
  Serial.begin(115200); // Must match the game.
  lastRawButtonPressed = stableButtonPressed = digitalRead(BUTTON_PIN) == HIGH; // No phantom BTN at boot.
  Serial.println("READY"); // Diagnostic; the game ignores it.
}

/**
 * @brief Main loop, called repeatedly by Arduino. Nothing here blocks, so the
 *        button, sensors, and buzzer commands all stay responsive.
 * @return No value.
 */
void loop() {
  const unsigned long nowMs = millis(); // One timestamp for this pass.
  ReadComputerCommands(); // Play buzzer tones requested by the game.
  UpdateButton(nowMs); // Send BTN on a new press.
  UpdateSound(nowMs); // Send SOUND (reads the microphone every pass).
  UpdateLight(nowMs); // Send LIGHT (reads the light sensor every 5 ms).
  ReportSensors(nowMs); // Print the SENSORS tuning line.
}
