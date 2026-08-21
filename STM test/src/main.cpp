#include <Arduino.h>
#include <HardwareTimer.h>

constexpr uint8_t DAC_I_PIN = PA4;
constexpr uint8_t DAC_Q_PIN = PA5;
constexpr uint8_t ADC_I_PIN = PA0;
constexpr uint8_t ADC_Q_PIN = PA1;
constexpr uint8_t TX_CLOCK_PIN = PB6;
constexpr uint8_t RX_CLOCK_PIN = PB7;
constexpr uint8_t USER_BUTTON_PIN = PC13;
constexpr uint16_t DAC_MAX = 4095;
constexpr uint32_t SYMBOL_PERIOD_US = 1000;
constexpr uint32_t CLOCK_FREQUENCY_HZ = 1000000UL / SYMBOL_PERIOD_US;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t LONG_PRESS_MS = 1500;
constexpr float ADC_REFERENCE_VOLTAGE = 3.3f;
constexpr uint8_t RX_PACKET_HEADER = 0xA5;
constexpr uint16_t RX_BUFFER_SIZE = 512;

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
bool stableButtonState = HIGH;
bool candidateButtonState = HIGH;
uint32_t buttonStateChangedAt = 0;
uint32_t buttonPressStart = 0;
bool longPressHandled = false;
bool receiverMode = false;
struct RxSample {
  uint16_t i;
  uint16_t q;
};

volatile RxSample rxBuffer[RX_BUFFER_SIZE];
volatile uint16_t rxBufferHead = 0;
volatile uint16_t rxBufferTail = 0;
volatile uint32_t rxDroppedSamples = 0;
HardwareTimer *txClockTimer = nullptr;

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
  Serial.print("--- ");
  Serial.print(receiverMode ? "RX" : "TX");
  Serial.println(" mode ---");
  Serial.print("Current modulation: ");
  Serial.println(modulation.name);
  Serial.print("States: ");
  Serial.println(modulation.order);
  Serial.print("Bits per symbol: ");
  Serial.println(2 * modulation.bitsPerAxis);
  Serial.println("DAC pins: I=PA4, Q=PA5, range=0..4095");
  Serial.println("ADC pins: I=PA0, Q=PA1");
  Serial.println("Clock pins: TX=PB6, RX=PB7");
  Serial.println("RX samples ADC values on each rising clock edge.");
  Serial.println("Short press: next modulation. Hold 1.5 s: toggle TX/RX.");
}

void receiveClockEdge() {
  const uint16_t nextHead = (rxBufferHead + 1) % RX_BUFFER_SIZE;
  if (nextHead == rxBufferTail) {
    rxDroppedSamples++;
    return;
  }

  rxBuffer[rxBufferHead].i = analogRead(ADC_I_PIN);
  rxBuffer[rxBufferHead].q = analogRead(ADC_Q_PIN);
  rxBufferHead = nextHead;
}

void setMode(bool newReceiverMode) {
  receiverMode = newReceiverMode;
  symbolIndex = 0;
  noInterrupts();
  rxBufferHead = 0;
  rxBufferTail = 0;
  rxDroppedSamples = 0;
  interrupts();

  if (receiverMode) {
    txClockTimer->pause();
    detachInterrupt(digitalPinToInterrupt(RX_CLOCK_PIN));
    pinMode(TX_CLOCK_PIN, INPUT);
    pinMode(RX_CLOCK_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(RX_CLOCK_PIN), receiveClockEdge, RISING);
  } else {
    detachInterrupt(digitalPinToInterrupt(RX_CLOCK_PIN));
    pinMode(RX_CLOCK_PIN, INPUT);
    txClockTimer->setMode(1, TIMER_OUTPUT_COMPARE_PWM1, TX_CLOCK_PIN);
    txClockTimer->setCount(0);
    txClockTimer->resume();
  }

  printModulationInfo();
}

void streamReceivedSamples() {
  uint8_t streamedSamples = 0;
  while (streamedSamples < 32) {
    uint16_t currentI;
    uint16_t currentQ;

    noInterrupts();
    if (rxBufferTail == rxBufferHead) {
      interrupts();
      return;
    }
    currentI = rxBuffer[rxBufferTail].i;
    streamedSamples++;
    currentQ = rxBuffer[rxBufferTail].q;
    rxBufferTail = (rxBufferTail + 1) % RX_BUFFER_SIZE;
    interrupts();

    const uint8_t packet[] = {
        RX_PACKET_HEADER,
        static_cast<uint8_t>(currentI & 0xFF),
        static_cast<uint8_t>(currentI >> 8),
        static_cast<uint8_t>(currentQ & 0xFF),
        static_cast<uint8_t>(currentQ >> 8)};
    Serial.write(packet, sizeof(packet));
  }
}

void processSerialCommands() {
  static char command[16];
  static uint8_t commandLength = 0;

  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      command[commandLength] = '\0';
      if (strcmp(command, "RX") == 0) {
        setMode(true);
      } else if (strcmp(command, "TX") == 0) {
        setMode(false);
      } else if (strcmp(command, "NEXT") == 0 && !receiverMode) {
        modulationIndex = (modulationIndex + 1) % MODULATION_COUNT;
        symbolIndex = 0;
        printModulationInfo();
      } else if (strncmp(command, "MOD ", 4) == 0 && !receiverMode) {
        const int requestedIndex = atoi(command + 4);
        if (requestedIndex >= 0 && requestedIndex < static_cast<int>(MODULATION_COUNT)) {
          modulationIndex = requestedIndex;
          symbolIndex = 0;
          printModulationInfo();
        }
      } else if (strcmp(command, "DROPPED") == 0) {
        Serial.print("DROPPED ");
        Serial.println(rxDroppedSamples);
      }
      commandLength = 0;
    } else if (commandLength < sizeof(command) - 1) {
      command[commandLength++] = character;
    }
  }
}

void checkButton() {
  const bool buttonState = digitalRead(USER_BUTTON_PIN);
  const uint32_t now = millis();

  if (buttonState != candidateButtonState) {
    candidateButtonState = buttonState;
    buttonStateChangedAt = now;
  }

  if (candidateButtonState != stableButtonState &&
      now - buttonStateChangedAt >= BUTTON_DEBOUNCE_MS) {
    stableButtonState = candidateButtonState;
    if (stableButtonState == LOW) {
      buttonPressStart = now;
      longPressHandled = false;
      Serial.println("Button pressed: hold for 1.5 s to toggle TX/RX.");
    } else {
      const uint32_t pressDuration = now - buttonPressStart;
      if (!longPressHandled && pressDuration < LONG_PRESS_MS && !receiverMode) {
        modulationIndex = (modulationIndex + 1) % MODULATION_COUNT;
        symbolIndex = 0;
        printModulationInfo();
      }
    }
  }

  if (stableButtonState == LOW && !longPressHandled &&
      now - buttonPressStart >= LONG_PRESS_MS) {
    longPressHandled = true;
    Serial.println("Long press detected: switching mode.");
    setMode(!receiverMode);
  }
}

void transmitSymbolOnClockFallingEdge() {
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

void setup() {
  Serial.begin(115200);
  pinMode(DAC_I_PIN, OUTPUT);
  pinMode(DAC_Q_PIN, OUTPUT);
  pinMode(ADC_I_PIN, INPUT_ANALOG);
  pinMode(ADC_Q_PIN, INPUT_ANALOG);
  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RX_CLOCK_PIN, INPUT);
  stableButtonState = digitalRead(USER_BUTTON_PIN);
  candidateButtonState = stableButtonState;
  buttonStateChangedAt = millis();
  analogWriteResolution(12);
  analogReadResolution(12);

  txClockTimer = new HardwareTimer(TIM4);
  txClockTimer->setOverflow(CLOCK_FREQUENCY_HZ, HERTZ_FORMAT);
  txClockTimer->setCaptureCompare(1, 50, PERCENT_COMPARE_FORMAT);
  txClockTimer->setMode(1, TIMER_OUTPUT_COMPARE_PWM1, TX_CLOCK_PIN);
  txClockTimer->attachInterrupt(1, transmitSymbolOnClockFallingEdge);

  analogWrite(DAC_I_PIN, axisLevel(0, modulations[modulationIndex].bitsPerAxis));
  analogWrite(DAC_Q_PIN, axisLevel(0, modulations[modulationIndex].bitsPerAxis));
  printModulationInfo();
  txClockTimer->setCount(0);
  txClockTimer->resume();
}

void loop() {
  processSerialCommands();
  checkButton();

  if (receiverMode) {
    streamReceivedSamples();
    return;
  }

}