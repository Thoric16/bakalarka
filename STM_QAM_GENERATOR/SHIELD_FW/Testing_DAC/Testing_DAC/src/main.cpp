#include <Arduino.h>
#include <stdlib.h>

constexpr uint8_t DAC_I = PA4;
constexpr uint8_t DAC_Q = PA5;
constexpr uint8_t OUTPUT_PC9 = PC9;
constexpr uint16_t DAC_MAX_VALUE = 4095;
constexpr uint16_t DAC_MIDDLE_VALUE = 1968;
constexpr int DAC_ALLOWED_MIN_MILLIVOLTS = -150;
constexpr int DAC_ALLOWED_MAX_MILLIVOLTS = 150;
constexpr uint16_t DAC_ALLOWED_MIN_RAW = 3403;
constexpr uint16_t DAC_ALLOWED_MAX_RAW = 524;

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  <mV>   - set both DAC outputs");
  Serial.println("  I <mV> - set DAC_I only");
  Serial.println("  Q <mV> - set DAC_Q only");
  Serial.println("  Voltage range: -150 to 150 mV");
}

uint16_t millivoltsToRaw(int millivolts) {
  const float raw =
      DAC_ALLOWED_MAX_RAW +
      (DAC_ALLOWED_MAX_MILLIVOLTS - millivolts) *
          (DAC_ALLOWED_MIN_RAW - DAC_ALLOWED_MAX_RAW) /
          static_cast<float>(DAC_ALLOWED_MAX_MILLIVOLTS -
                             DAC_ALLOWED_MIN_MILLIVOLTS);
  return static_cast<uint16_t>(raw + 0.5f);
}

void reportOutput(const char *channel, uint16_t raw) {
  const float millivolts =
      DAC_ALLOWED_MAX_MILLIVOLTS -
      static_cast<float>(raw - DAC_ALLOWED_MAX_RAW) *
          (DAC_ALLOWED_MAX_MILLIVOLTS - DAC_ALLOWED_MIN_MILLIVOLTS) /
          (DAC_ALLOWED_MIN_RAW - DAC_ALLOWED_MAX_RAW);

  Serial.print(channel);
  Serial.print(" set to ");
  Serial.print(millivolts, 3);
  Serial.print(" mV (raw value ");
  Serial.print(raw);
  Serial.println(")");
}

void setOutput(const char *channel, uint8_t pin, int millivolts) {
  const uint16_t raw = millivoltsToRaw(millivolts);
  analogWrite(pin, raw);
  reportOutput(channel, raw);
}

void processCommand(String command) {
  command.trim();

  if (command.length() == 0) {
    return;
  }

  char channel = 'B';
  const char firstCharacter = command.charAt(0);
  if (firstCharacter == 'I' || firstCharacter == 'i' ||
      firstCharacter == 'Q' || firstCharacter == 'q') {
    channel = firstCharacter & ~0x20;
    command.remove(0, 1);
    command.trim();
  }

  char *end = nullptr;
  const long millivolts = strtol(command.c_str(), &end, 10);
  if (*end != '\0' || millivolts < DAC_ALLOWED_MIN_MILLIVOLTS ||
      millivolts > DAC_ALLOWED_MAX_MILLIVOLTS) {
    Serial.println("Invalid command. Enter -150 to 150 mV.");
    return;
  }

  if (channel == 'I' || channel == 'B') {
    setOutput("DAC_I", DAC_I, millivolts);
  }
  if (channel == 'Q' || channel == 'B') {
    setOutput("DAC_Q", DAC_Q, millivolts);
  }
}

void setup() {
  Serial.begin(115200);

  analogWriteResolution(12);
  pinMode(DAC_I, OUTPUT);
  pinMode(DAC_Q, OUTPUT);
  pinMode(OUTPUT_PC9, OUTPUT);
  digitalWrite(OUTPUT_PC9, LOW);

  analogWrite(DAC_I, DAC_MIDDLE_VALUE);
  analogWrite(DAC_Q, DAC_MIDDLE_VALUE);

  Serial.println();
  printHelp();
}

void loop() {
  if (Serial.available() > 0) {
    processCommand(Serial.readStringUntil('\n'));
  }
}
