#include <Arduino.h>

constexpr uint8_t ADC_I_PIN = PA0;
constexpr uint8_t ADC_Q_PIN = PA1;
constexpr uint8_t ADC_ENABLE_PIN = PC9;
constexpr uint16_t ADC_I_RAW_AT_NEGATIVE_MV = 3208;
constexpr uint16_t ADC_I_RAW_AT_POSITIVE_MV = 452;
constexpr uint16_t ADC_Q_RAW_AT_NEGATIVE_MV = 3270;
constexpr uint16_t ADC_Q_RAW_AT_POSITIVE_MV = 480;
constexpr float ADC_I_NEGATIVE_MV = -154.0f;
constexpr float ADC_I_POSITIVE_MV = 156.0f;
constexpr float ADC_Q_NEGATIVE_MV = -155.0f;
constexpr float ADC_Q_POSITIVE_MV = 154.0f;

float mapAdcToMillivolts(uint16_t raw, uint16_t rawAtNegativeMv,
                         float negativeMv, uint16_t rawAtPositiveMv,
                         float positiveMv) {
  return negativeMv +
         (static_cast<float>(raw) - rawAtNegativeMv) *
             (positiveMv - negativeMv) /
             (rawAtPositiveMv - rawAtNegativeMv);
}

void setup() {
  pinMode(ADC_ENABLE_PIN, OUTPUT);
  digitalWrite(ADC_ENABLE_PIN, HIGH);

  pinMode(ADC_I_PIN, INPUT_ANALOG);
  pinMode(ADC_Q_PIN, INPUT_ANALOG);
  analogReadResolution(12);

  Serial.begin(115200);
  Serial.println("Channel  Raw bit value  Calibrated voltage (mV)");
}

void loop() {
  const uint16_t adcI = analogRead(ADC_I_PIN);
  const uint16_t adcQ = analogRead(ADC_Q_PIN);
  const float voltageI =
      mapAdcToMillivolts(adcI, ADC_I_RAW_AT_NEGATIVE_MV, ADC_I_NEGATIVE_MV,
                         ADC_I_RAW_AT_POSITIVE_MV, ADC_I_POSITIVE_MV);
  const float voltageQ =
      mapAdcToMillivolts(adcQ, ADC_Q_RAW_AT_NEGATIVE_MV, ADC_Q_NEGATIVE_MV,
                         ADC_Q_RAW_AT_POSITIVE_MV, ADC_Q_POSITIVE_MV);

  Serial.print("ADC_I (PA0): ");
  Serial.print(adcI);
  Serial.print(" bits, ");
  Serial.print(voltageI, 3);
  Serial.print(" mV    ADC_Q (PA1): ");
  Serial.print(adcQ);
  Serial.print(" bits, ");
  Serial.print(voltageQ, 3);
  Serial.println(" mV");

  delay(100);
}