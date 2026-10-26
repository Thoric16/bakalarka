#include <Arduino.h>

// Hardware mapping and calibration copied from Testin_ADC.
constexpr uint8_t ADC_I_PIN = PA0;
constexpr uint8_t ADC_Q_PIN = PA1;
constexpr uint8_t ADC_ENABLE_PIN = PC9;
constexpr uint8_t CLOCK_INPUT_PIN = D9;
constexpr uint16_t ADC_I_RAW_AT_NEGATIVE_MV = 3208;
constexpr uint16_t ADC_I_RAW_AT_POSITIVE_MV = 452;
constexpr uint16_t ADC_Q_RAW_AT_NEGATIVE_MV = 3270;
constexpr uint16_t ADC_Q_RAW_AT_POSITIVE_MV = 480;
constexpr float ADC_I_NEGATIVE_MV = -154.0f;
constexpr float ADC_I_POSITIVE_MV = 156.0f;
constexpr float ADC_Q_NEGATIVE_MV = -155.0f;
constexpr float ADC_Q_POSITIVE_MV = 154.0f;

// The first implementation uses blocking analogRead() sampling. One centered
// sample per symbol and a slow rate provide timing margin before DMA/timer
// sampling is introduced.
constexpr uint8_t SAMPLES_PER_SYMBOL = 1;
constexpr uint32_t DEFAULT_SYMBOL_RATE = 50;
constexpr uint8_t PAYLOAD_LIMIT = 128;
constexpr int16_t LEVEL_THRESHOLD_MV = 25;
constexpr uint8_t AGC_SYMBOLS = 128;
constexpr uint8_t SYNC_SYMBOLS = 16;
constexpr uint8_t TIMING_SYMBOLS = 64;
constexpr uint16_t SYNC_SEARCH_SYMBOLS = 256;
constexpr uint32_t DEFAULT_SIGNAL_TIMEOUT_US = 15000000;
constexpr uint16_t MAX_TRACE_SYMBOLS = 768;
const uint8_t SYNC_WORD[SYNC_SYMBOLS] = {
    0, 3, 1, 2, 2, 1, 3, 0, 1, 2, 0, 3, 3, 0, 2, 1};

bool debugEnabled = true;
bool rawTraceEnabled = false;
uint8_t symbolTrace[MAX_TRACE_SYMBOLS];
uint16_t symbolTraceLength = 0;
const char *lastFailure = "unknown";
uint16_t failureIndex = 0;
uint8_t failureExpected = 0;
uint8_t failureReceived = 0;
uint32_t signalTimeoutUs = DEFAULT_SIGNAL_TIMEOUT_US;
uint32_t symbolRate = DEFAULT_SYMBOL_RATE;
bool externalClockEnabled = false;
int16_t lastSignalI = 0;
int16_t lastSignalQ = 0;

float mapAdc(uint16_t raw, uint16_t rawNegative, float negativeMv,
             uint16_t rawPositive, float positiveMv) {
  return negativeMv +
         (static_cast<float>(raw) - rawNegative) *
             (positiveMv - negativeMv) / (rawPositive - rawNegative);
}

int16_t readI() {
  const uint16_t raw = analogRead(ADC_I_PIN);
  return static_cast<int16_t>(
      mapAdc(raw, ADC_I_RAW_AT_NEGATIVE_MV, ADC_I_NEGATIVE_MV,
             ADC_I_RAW_AT_POSITIVE_MV, ADC_I_POSITIVE_MV));
}

int16_t readQ() {
  const uint16_t raw = analogRead(ADC_Q_PIN);
  return static_cast<int16_t>(
      mapAdc(raw, ADC_Q_RAW_AT_NEGATIVE_MV, ADC_Q_NEGATIVE_MV,
             ADC_Q_RAW_AT_POSITIVE_MV, ADC_Q_POSITIVE_MV));
}

uint8_t classify(int16_t i, int16_t q) {
  const bool positiveI = i >= 0;
  const bool positiveQ = q >= 0;
  if (positiveI && positiveQ) return 0;
  if (!positiveI && positiveQ) return 1;
  if (positiveI && !positiveQ) return 2;
  return 3;
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

uint8_t readSymbol() {
  int32_t sumI = 0;
  int32_t sumQ = 0;
  for (uint8_t sample = 0; sample < SAMPLES_PER_SYMBOL; ++sample) {
    if (externalClockEnabled) {
      const uint32_t start = micros();
      while (digitalRead(CLOCK_INPUT_PIN) == HIGH) {
        if (micros() - start >= signalTimeoutUs) {
          lastFailure = "clock_timeout";
          return 0;
        }
      }
      const uint32_t edgeStart = micros();
      while (digitalRead(CLOCK_INPUT_PIN) == LOW) {
        if (micros() - edgeStart >= signalTimeoutUs) {
          lastFailure = "clock_timeout";
          return 0;
        }
      }
    }
    const uint32_t start = micros();
    sumI += readI();
    sumQ += readQ();
    const uint32_t sampleUs = 1000000UL / symbolRate;
    while (!externalClockEnabled &&
           static_cast<int32_t>(micros() - start) <
               static_cast<int32_t>(sampleUs)) {
    }
  }
  const uint8_t symbol = classify(static_cast<int16_t>(sumI / SAMPLES_PER_SYMBOL),
                                   static_cast<int16_t>(sumQ / SAMPLES_PER_SYMBOL));
  if (symbolTraceLength < MAX_TRACE_SYMBOLS)
    symbolTrace[symbolTraceLength++] = symbol;
  return symbol;
}

bool signalPresent() {
  lastSignalI = readI();
  lastSignalQ = readQ();
  return abs(lastSignalI) > LEVEL_THRESHOLD_MV ||
         abs(lastSignalQ) > LEVEL_THRESHOLD_MV;
}

void printHex(const uint8_t *data, uint16_t length) {
  for (uint16_t index = 0; index < length; ++index) {
    if (data[index] < 16) Serial.print('0');
    Serial.print(data[index], HEX);
  }
}

void printSymbolTrace() {
  if (!rawTraceEnabled) return;
  Serial.print("RAW_SYMBOLS count=");
  Serial.print(symbolTraceLength);
  Serial.print(" data=");
  for (uint16_t index = 0; index < symbolTraceLength; ++index)
    Serial.print(symbolTrace[index]);
  Serial.println();
}

bool waitForSignal() {
  const uint32_t start = micros();
  uint8_t stableSamples = 0;
  while (!signalPresent()) {
    stableSamples = 0;
    if (!externalClockEnabled)
      delayMicroseconds(1000000UL / symbolRate);
    if (micros() - start >= signalTimeoutUs) return false;
  }
  while (stableSamples < 3) {
    if (!externalClockEnabled)
      delayMicroseconds(1000000UL / symbolRate);
    if (signalPresent()) {
      ++stableSamples;
    } else {
      stableSamples = 0;
    }
    if (micros() - start >= signalTimeoutUs) return false;
  }
  if (!externalClockEnabled)
    delayMicroseconds(500000UL / symbolRate);
  return true;
}

bool receiveFrame(uint8_t *payload, uint8_t &length, uint16_t &sequence,
                  uint8_t &type) {
  symbolTraceLength = 0;
  lastFailure = "signal_timeout";
  if (!waitForSignal()) return false;

  // Do not assume that the first sample after signal detection is aligned to
  // the first AGC symbol. Search for the known sync word inside the AGC
  // preamble instead.
  uint8_t recentSymbols[SYNC_SYMBOLS] = {};
  bool syncFound = false;
  for (uint16_t symbol = 0; symbol < SYNC_SEARCH_SYMBOLS; ++symbol) {
    const uint8_t received = readSymbol();
    for (uint8_t index = 0; index < SYNC_SYMBOLS - 1; ++index)
      recentSymbols[index] = recentSymbols[index + 1];
    recentSymbols[SYNC_SYMBOLS - 1] = received;
    if (symbol + 1 < SYNC_SYMBOLS) continue;

    syncFound = true;
    for (uint8_t index = 0; index < SYNC_SYMBOLS; ++index) {
      if (recentSymbols[index] != SYNC_WORD[index]) {
        syncFound = false;
        break;
      }
    }
    if (syncFound) break;
  }
  if (!syncFound) {
    lastFailure = "sync_search";
    return false;
  }

  for (uint8_t symbol = 0; symbol < TIMING_SYMBOLS; ++symbol) {
    const uint8_t expected = symbol & 1 ? 3 : 0;
    const uint8_t received = readSymbol();
    if (received != expected) {
      lastFailure = "timing";
      failureIndex = symbol;
      failureExpected = expected;
      failureReceived = received;
      return false;
    }
  }

  uint8_t header[12];
  for (uint8_t index = 0; index < sizeof(header); ++index) {
    uint8_t value = 0;
    for (uint8_t dibit = 0; dibit < 4; ++dibit)
      value = static_cast<uint8_t>((value << 2) | readSymbol());
    header[index] = value;
  }
  const uint16_t payloadLength =
      static_cast<uint16_t>(header[8] | (header[9] << 8));
  if (header[0] != 1 || header[5] != 0 || payloadLength > PAYLOAD_LIMIT ||
      header[10] != (symbolRate & 0xFF) ||
      header[11] != (symbolRate >> 8)) {
    lastFailure = "header";
    if (debugEnabled) {
      Serial.print("RAW_HEADER data=");
      printHex(header, sizeof(header));
      Serial.println();
    }
    return false;
  }

  type = header[1];
  sequence = static_cast<uint16_t>(header[6] | (header[7] << 8));
  length = static_cast<uint8_t>(payloadLength);
  uint8_t crcData[12 + PAYLOAD_LIMIT];
  memcpy(crcData, header, sizeof(header));
  for (uint8_t index = 0; index < length; ++index) {
    uint8_t value = 0;
    for (uint8_t dibit = 0; dibit < 4; ++dibit)
      value = static_cast<uint8_t>((value << 2) | readSymbol());
    payload[index] = value;
    crcData[sizeof(header) + index] = value;
  }
  uint16_t receivedCrc = 0;
  for (uint8_t byte = 0; byte < 2; ++byte) {
    uint8_t value = 0;
    for (uint8_t dibit = 0; dibit < 4; ++dibit)
      value = static_cast<uint8_t>((value << 2) | readSymbol());
    receivedCrc |= static_cast<uint16_t>(value) << (byte * 8);
  }
  const uint16_t expectedCrc = crc16(crcData, sizeof(header) + length);
  if (receivedCrc != expectedCrc) {
    lastFailure = "crc";
    if (debugEnabled) {
      Serial.print("RAW_FRAME header=");
      printHex(header, sizeof(header));
      Serial.print(" payload=");
      printHex(payload, length);
      Serial.print(" crc_rx=");
      Serial.print(receivedCrc, HEX);
      Serial.print(" crc_calc=");
      Serial.println(expectedCrc, HEX);
    }
    return false;
  }
  lastFailure = "ok";
  return true;
}

void printDebugSettings() {
  Serial.print("RX_DEBUG enabled=");
  Serial.print(debugEnabled ? 1 : 0);
  Serial.print(" raw_symbols=");
  Serial.print(rawTraceEnabled ? 1 : 0);
  Serial.print(" timeout_ms=");
  Serial.print(signalTimeoutUs / 1000);
  Serial.print(" rate=");
  Serial.print(symbolRate);
  Serial.print(" clock=");
  Serial.println(externalClockEnabled ? "external" : "internal");
}

void processCommand() {
  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toUpperCase();
  if (command == "DEBUG ON") debugEnabled = true;
  else if (command == "DEBUG OFF") debugEnabled = false;
  else if (command == "RAW ON") rawTraceEnabled = true;
  else if (command == "RAW OFF") rawTraceEnabled = false;
  else if (command == "DEBUG?") {
    printDebugSettings();
    return;
  } else if (command == "CLOCK EXTERNAL") {
    externalClockEnabled = true;
    pinMode(CLOCK_INPUT_PIN, INPUT);
  } else if (command == "CLOCK INTERNAL") {
    externalClockEnabled = false;
  } else if (command.startsWith("RATE ")) {
    const long requestedRate = command.substring(5).toInt();
    if (requestedRate < 1 || requestedRate > 1000) {
      Serial.println("RX_CMD rate_hz=1..1000");
      return;
    }
    symbolRate = static_cast<uint32_t>(requestedRate);
  } else if (command.startsWith("TIMEOUT ")) {
    const long timeoutSeconds = command.substring(8).toInt();
    if (timeoutSeconds < 1 || timeoutSeconds > 300) {
      Serial.println("RX_CMD timeout_seconds=1..300");
      return;
    }
    signalTimeoutUs = static_cast<uint32_t>(timeoutSeconds) * 1000000UL;
  } else {
    Serial.println(
        "RX_CMD use=DEBUG ON|DEBUG OFF|RAW ON|RAW OFF|DEBUG?|TIMEOUT <seconds>|"
        "RATE <hz>|CLOCK INTERNAL|CLOCK EXTERNAL");
    return;
  }
  printDebugSettings();
}

void setup() {
  pinMode(ADC_ENABLE_PIN, OUTPUT);
  digitalWrite(ADC_ENABLE_PIN, HIGH);
  pinMode(ADC_I_PIN, INPUT_ANALOG);
  pinMode(ADC_Q_PIN, INPUT_ANALOG);
  pinMode(CLOCK_INPUT_PIN, INPUT);
  analogReadResolution(12);
  Serial.begin(115200);
  Serial.println("RX_READY symbol_rate=50 samples_per_symbol=1 clock=internal debug=1");
}

void loop() {
  if (Serial.available() > 0) {
    processCommand();
    return;
  }
  static uint8_t payload[PAYLOAD_LIMIT];
  uint8_t length = 0;
  uint8_t type = 0;
  uint16_t sequence = 0;
  if (!receiveFrame(payload, length, sequence, type)) {
    if (debugEnabled) {
      Serial.print("RX_ERR stage=");
      Serial.println(lastFailure);
      if (lastFailure == "signal_timeout") {
        Serial.print("RX_LEVEL i_mv=");
        Serial.print(lastSignalI);
        Serial.print(" q_mv=");
        Serial.print(lastSignalQ);
        Serial.print(" threshold_mv=");
        Serial.println(LEVEL_THRESHOLD_MV);
      }
      if (lastFailure == "agc" || lastFailure == "sync" ||
          lastFailure == "timing") {
        Serial.print("RX_MISMATCH index=");
        Serial.print(failureIndex);
        Serial.print(" expected=");
        Serial.print(failureExpected);
        Serial.print(" received=");
        Serial.println(failureReceived);
      }
      printSymbolTrace();
    }
    delay(1);
    return;
  }
  Serial.print("RX_OK type=0x");
  if (type < 16) Serial.print('0');
  Serial.print(type, HEX);
  Serial.print(" seq=");
  Serial.print(sequence);
  Serial.print(" len=");
  Serial.println(length);
  Serial.print("MSG seq=");
  Serial.print(sequence);
  Serial.print(" type=0x");
  Serial.print(type, HEX);
  Serial.print(" hex=");
  for (uint8_t index = 0; index < length; ++index) {
    if (payload[index] < 16) Serial.print('0');
    Serial.print(payload[index], HEX);
  }
  Serial.println();
}
