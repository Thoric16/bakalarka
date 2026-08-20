#include <Arduino.h>

constexpr uint8_t DAC_I_PIN = PA4;
constexpr uint8_t DAC_Q_PIN = PA5;
constexpr uint8_t USER_BUTTON_PIN = PC13;
constexpr uint16_t DAC_MAX = 4095;
constexpr uint32_t SYMBOL_PERIOD_US = 100;

struct Modulation {
  const char *name;
  uint16_t order;
  uint8_t bitsPerAxis;
};

const Modulation modulations[] = {
  {"QPSK", 4, 1},
  {"16-QAM", 16, 2},
  {"64-QAM", 64, 3},
  {"256-QAM", 256, 4},
  {"1024-QAM", 1024, 5},
  {"4096-QAM", 4096, 6}
};

constexpr size_t MODULATION_COUNT = sizeof(modulations) / sizeof(modulations[0]);
size_t modulationIndex = 0;
uint16_t symbolIndex = 0;
bool previousButtonState = HIGH;
uint32_t lastButtonChange = 0;
uint32_t lastSymbolTime = 0;

uint16_t axisLevel(uint16_t axisIndex, uint8_t bitsPerAxis) {
  const uint16_t axisLevels = 1U << bitsPerAxis;
  if (axisLevels == 1) {
    return DAC_MAX / 2;
  }
  return static_cast<uint16_t>((static_cast<uint32_t>(axisIndex) * DAC_MAX) / (axisLevels - 1));
}

void printModulationInfo() {
  const Modulation &modulation = modulations[modulationIndex];
  Serial.println();
  Serial.println("--- QAM generator ---");
  Serial.print("Current modulation: ");
  Serial.println(modulation.name);
  Serial.print("States: ");
  Serial.println(modulation.order);
  Serial.print("Bits per symbol: ");
  Serial.println(2 * modulation.bitsPerAxis);
  Serial.print("DAC pins: I=PA4, Q=PA5, range=0.. ");
  Serial.println(DAC_MAX);
  Serial.println("Press the blue USER button to change modulation.");
}

void checkButton() {
  const bool buttonState = digitalRead(USER_BUTTON_PIN);
  const uint32_t now = millis();
  if (buttonState != previousButtonState && now - lastButtonChange >= 40) {
    previousButtonState = buttonState;
    lastButtonChange = now;
    if (buttonState == LOW) {
      modulationIndex = (modulationIndex + 1) % MODULATION_COUNT;
      symbolIndex = 0;
      printModulationInfo();
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(DAC_I_PIN, OUTPUT);
  pinMode(DAC_Q_PIN, OUTPUT);
  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  analogWriteResolution(12);
  printModulationInfo();
}

void loop() {
  checkButton();

  if (micros() - lastSymbolTime >= SYMBOL_PERIOD_US) {
    lastSymbolTime = micros();
    const Modulation &modulation = modulations[modulationIndex];
    const uint16_t axisLevels = 1U << modulation.bitsPerAxis;
    const uint16_t iIndex = symbolIndex % axisLevels;
    const uint16_t qIndex = symbolIndex / axisLevels;

    analogWrite(DAC_I_PIN, axisLevel(iIndex, modulation.bitsPerAxis));
    analogWrite(DAC_Q_PIN, axisLevel(qIndex, modulation.bitsPerAxis));

    symbolIndex++;
    if (symbolIndex >= modulation.order) {
      symbolIndex = 0;
    }
  }
}