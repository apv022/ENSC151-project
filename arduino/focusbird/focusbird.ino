/*
 * FocusBird Arduino firmware
 *
 * Reads Grove components and exchanges newline messages with the game over
 * USB serial at 115200 baud:
 *   Arduino -> game: BTN, SOUND, LIGHT, and SENSORS <sound> <light> <soundCutoff> <lightCutoff>
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

const int ANALOG_MAX_VALUE = 1023; // 10-bit ADC on Uno-compatible boards; change for other ADC ranges.
const int SOUND_RAW_THRESHOLD = 800; // SOUND fires when the raw reading stays above this.
const int LIGHT_RAW_THRESHOLD = ANALOG_MAX_VALUE / 10; // LIGHT fires at or below 10% of full scale (covered).
const int LIGHT_RAW_RELEASE_THRESHOLD = ANALOG_MAX_VALUE * 12 / 100; // Rearm above 12% (hysteresis gap stops chatter).
const unsigned int CONFIRM_SAMPLES = 3; // Consecutive samples required, so one ADC spike is ignored.
const unsigned long BUTTON_DEBOUNCE_MS = 35; // Ignore button contact bounce shorter than this.
const unsigned long SOUND_COOLDOWN_MS = 1000; // One SOUND per yell; quiet time before the next can fire.
const unsigned long SENSOR_SAMPLE_INTERVAL_MS = 5; // Analog sampling period.
const unsigned long SENSOR_REPORT_INTERVAL_MS = 500; // How often the SENSORS diagnostic line is printed.

// ------------------------------- Runtime state -------------------------------

bool stableButtonPressed = false; // Debounced button state.
bool lastRawButtonPressed = false; // Last raw reading, to detect changes.
unsigned long buttonChangedAt = 0; // When the raw reading last changed.
unsigned long lastSampleAt = 0; // When the analog sensors were last sampled.
unsigned long lastReportAt = 0; // When SENSORS was last printed.
unsigned long lastSoundAt = 0; // When the last loud sample was seen (drives the cooldown).
unsigned int loudSamples = 0; // Consecutive samples above the sound threshold.
unsigned int darkSamples = 0; // Consecutive samples at or below the light threshold.
bool soundArmed = true; // False after SOUND until the room has been quiet for the cooldown.
bool lightCovered = false; // True after LIGHT until the sensor is uncovered again.
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
 * @brief Sample the sound and light sensors every SENSOR_SAMPLE_INTERVAL_MS and
 *        send one SOUND per yell and one LIGHT per cover. Also prints the
 *        SENSORS diagnostic line used for tuning.
 * @param nowMs Current millis() value.
 * @return No value; may print "SOUND", "LIGHT", and "SENSORS ...".
 */
void UpdateAnalogSensors(unsigned long nowMs) {
  if (nowMs - lastSampleAt < SENSOR_SAMPLE_INTERVAL_MS) {
    return; // Not time to sample yet; keep loop() fast for the button and serial.
  }
  lastSampleAt = nowMs;

  // Sound: fire after CONFIRM_SAMPLES loud samples, then stay silent until it
  // has been quiet for SOUND_COOLDOWN_MS (a long yell counts once).
  const int sound = analogRead(SOUND_SENSOR_PIN);
  if (sound > SOUND_RAW_THRESHOLD) {
    lastSoundAt = nowMs; // Any loud sample pushes the cooldown back.
    loudSamples = loudSamples < CONFIRM_SAMPLES ? loudSamples + 1 : loudSamples; // Count up, capped.
    if (soundArmed && loudSamples >= CONFIRM_SAMPLES) {
      soundArmed = false; // Ignore the rest of this yell.
      Serial.println("SOUND");
    }
  } else {
    loudSamples = 0; // A quiet sample breaks the streak.
    if (!soundArmed && nowMs - lastSoundAt >= SOUND_COOLDOWN_MS) {
      soundArmed = true; // Quiet long enough: the next yell can count.
    }
  }

  // Light: fire after CONFIRM_SAMPLES dark samples, then wait until the reading
  // rises above the release threshold before another cover can count.
  const int light = analogRead(LIGHT_SENSOR_PIN);
  if (!lightCovered) {
    darkSamples = light <= LIGHT_RAW_THRESHOLD ? darkSamples + 1 : 0; // Count consecutive dark samples.
    if (darkSamples >= CONFIRM_SAMPLES) {
      lightCovered = true; // Latch: holding a hand over it sends only one LIGHT.
      darkSamples = 0;
      Serial.println("LIGHT");
    }
  } else if (light > LIGHT_RAW_RELEASE_THRESHOLD) {
    lightCovered = false; // Uncovered: the next cover can count.
  }

  if (nowMs - lastReportAt >= SENSOR_REPORT_INTERVAL_MS) { // Diagnostics for Serial Monitor tuning.
    lastReportAt = nowMs;
    // Format: SENSORS <soundRaw> <lightRaw> <soundThreshold> <lightThreshold>
    Serial.print("SENSORS ");
    Serial.print(sound);
    Serial.print(' ');
    Serial.print(light);
    Serial.print(' ');
    Serial.print(SOUND_RAW_THRESHOLD);
    Serial.print(' ');
    Serial.println(LIGHT_RAW_THRESHOLD);
  }
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
        tone(BUZZER_PIN, frequencyHz, min(durationMs, 2000U)); // Non-blocking; capped at 2 s.
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
  UpdateAnalogSensors(nowMs); // Send SOUND / LIGHT / SENSORS.
}
