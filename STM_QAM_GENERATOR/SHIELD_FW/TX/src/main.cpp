#include <Arduino.h>
#include <stdlib.h>

// Hardware mapping and calibration copied from Testing_DAC.
constexpr uint8_t DAC_I = PA4;
constexpr uint8_t DAC_Q = PA5;
constexpr uint8_t OUTPUT_PC9 = PC9;
constexpr uint8_t CLOCK_OUTPUT_PIN = D8;
constexpr uint16_t DAC_MAX_VALUE = 4095;
constexpr uint16_t DAC_MIDDLE_VALUE = 1968;
constexpr int DAC_ALLOWED_MIN_MILLIVOLTS = -150;
constexpr int DAC_ALLOWED_MAX_MILLIVOLTS = 150;
constexpr uint16_t DAC_ALLOWED_MIN_RAW = 3403;
constexpr uint16_t DAC_ALLOWED_MAX_RAW = 524;

// Match the centered, blocking ADC receiver while the first link test is
// being brought up. Increase after timer/DMA sampling is implemented.
constexpr uint32_t DEFAULT_SYMBOL_RATE = 50;
constexpr uint32_t CLOCK_PULSE_US = 20;
constexpr uint8_t PAYLOAD_LIMIT = 128;
constexpr uint8_t AGC_SYMBOLS = 128;
constexpr uint16_t QPSK_AMPLITUDE_MV = 100;
const uint8_t SYNC_WORD[16] = {0, 3, 1, 2, 2, 1, 3, 0, 1, 2, 0, 3, 3, 0, 2, 1};
uint32_t symbolRate = DEFAULT_SYMBOL_RATE;

uint16_t millivoltsToRaw(int millivolts) {
  const float raw = DAC_ALLOWED_MAX_RAW +
      (DAC_ALLOWED_MAX_MILLIVOLTS - millivolts) *
          (DAC_ALLOWED_MIN_RAW - DAC_ALLOWED_MAX_RAW) /
          static_cast<float>(DAC_ALLOWED_MAX_MILLIVOLTS - DAC_ALLOWED_MIN_MILLIVOLTS);
  return static_cast<uint16_t>(raw + 0.5f);
}

uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  while (length--) {
    crc ^= static_cast<uint16_t>(*data++) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

void outputSymbol(uint8_t symbol) {
  const bool positiveI = symbol == 0 || symbol == 2;
  const bool positiveQ = symbol == 0 || symbol == 1;
  analogWrite(DAC_I, millivoltsToRaw(positiveI ? QPSK_AMPLITUDE_MV : -QPSK_AMPLITUDE_MV));
  analogWrite(DAC_Q, millivoltsToRaw(positiveQ ? QPSK_AMPLITUDE_MV : -QPSK_AMPLITUDE_MV));
  digitalWrite(CLOCK_OUTPUT_PIN, HIGH);
  delayMicroseconds(CLOCK_PULSE_US);
  digitalWrite(CLOCK_OUTPUT_PIN, LOW);
  const uint32_t symbolUs = 1000000UL / symbolRate;
  if (symbolUs > CLOCK_PULSE_US)
    delayMicroseconds(symbolUs - CLOCK_PULSE_US);
}

void outputByte(uint8_t value) {
  outputSymbol((value >> 6) & 3);
  outputSymbol((value >> 4) & 3);
  outputSymbol((value >> 2) & 3);
  outputSymbol(value & 3);
}

void sendFrame(uint8_t type, const uint8_t *payload, uint8_t length,
               uint16_t sequence) {
  uint8_t header[12] = {1, type, 0, 1, 2, 0,
                        static_cast<uint8_t>(sequence & 0xFF),
                        static_cast<uint8_t>(sequence >> 8), length, 0,
                        static_cast<uint8_t>(symbolRate & 0xFF),
                        static_cast<uint8_t>(symbolRate >> 8)};
  uint8_t crcData[12 + PAYLOAD_LIMIT];
  memcpy(crcData, header, sizeof(header));
  memcpy(crcData + sizeof(header), payload, length);
  const uint16_t crc = crc16(crcData, sizeof(header) + length);

  for (uint8_t index = 0; index < AGC_SYMBOLS; ++index) outputSymbol(0);
  for (uint8_t index = 0; index < 16; ++index) outputSymbol(SYNC_WORD[index]);
  for (uint8_t index = 0; index < 64; ++index) outputSymbol(index & 1 ? 3 : 0);
  for (uint8_t index = 0; index < sizeof(header); ++index) outputByte(header[index]);
  for (uint8_t index = 0; index < length; ++index) outputByte(payload[index]);
  outputByte(static_cast<uint8_t>(crc & 0xFF));
  outputByte(static_cast<uint8_t>(crc >> 8));
  for (uint8_t index = 0; index < 8; ++index) {
    analogWrite(DAC_I, DAC_MIDDLE_VALUE);
    analogWrite(DAC_Q, DAC_MIDDLE_VALUE);
    delayMicroseconds(1000000UL / symbolRate);
  }
}

void setup() {
  Serial.begin(115200);
  analogWriteResolution(12);
  pinMode(DAC_I, OUTPUT);
  pinMode(DAC_Q, OUTPUT);
  pinMode(OUTPUT_PC9, OUTPUT);
  pinMode(CLOCK_OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PC9, LOW);
  digitalWrite(CLOCK_OUTPUT_PIN, LOW);
  analogWrite(DAC_I, DAC_MIDDLE_VALUE);
  analogWrite(DAC_Q, DAC_MIDDLE_VALUE);
  Serial.println("TX_READY symbol_rate=50 clock_out=D8");
}

void loop() {
  if (!Serial.available()) return;
  String command = Serial.readStringUntil('\n');
  command.trim();
  if (!command.startsWith("SEND ")) {
    if (command.startsWith("RATE ")) {
      const long requestedRate = command.substring(5).toInt();
      if (requestedRate < 1 || requestedRate > 1000) {
        Serial.println("TX_CMD rate_hz=1..1000");
        return;
      }
      symbolRate = static_cast<uint32_t>(requestedRate);
      Serial.print("TX_RATE symbol_rate=");
      Serial.println(symbolRate);
      return;
    }
    Serial.println("TX_ERR reason=expected_SEND_hex_or_RATE");
    return;
  }
  String hex = command.substring(5);
  if (hex.length() > PAYLOAD_LIMIT * 2 || (hex.length() & 1)) {
    Serial.println("TX_ERR reason=payload_length");
    return;
  }
  static uint16_t sequence = 0;
  uint8_t payload[PAYLOAD_LIMIT];
  for (uint8_t index = 0; index < hex.length() / 2; ++index) {
    char *end = nullptr;
    char byteText[3] = {hex[index * 2], hex[index * 2 + 1], '\0'};
    const long value = strtol(byteText, &end, 16);
    if (*end != '\0') {
      Serial.println("TX_ERR reason=invalid_hex");
      return;
    }
    payload[index] = static_cast<uint8_t>(value);
  }
  sendFrame(0x04, payload, hex.length() / 2, sequence++);
  Serial.print("TX_OK seq=");
  Serial.println(sequence - 1);
}
