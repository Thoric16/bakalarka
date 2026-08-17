#include <Arduino.h>
#include "arduinoFFT.h"

#define SAMPLES 128             // Must be a power of 2
#define SAMPLING_FREQ 2000      // Sampling frequency in Hz (Nyquist limit: 1000 Hz)
#define ANALOG_PIN A0

double vReal[SAMPLES];
double vImag[SAMPLES];

ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);
unsigned int sampling_period_us;

void setup() {
  Serial.begin(115200);
  pinMode(ANALOG_PIN, INPUT);
  sampling_period_us = round(1000000.0 * (1.0 / SAMPLING_FREQ));
}

void loop() {
  // 1. Sample ADC on pin A0
  for (int i = 0; i < SAMPLES; i++) {
    unsigned long start_time = micros();
    vReal[i] = analogRead(ANALOG_PIN);
    vImag[i] = 0; // Imaginary part is 0 for real signals

    while ((micros() - start_time) < sampling_period_us) {
      // Wait for exact sampling window
    }
  }

  // 2. Apply Windowing and Compute FFT
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // 3. Find Dominant Peak Frequency
  double peakFrequency = FFT.majorPeak();

  // 4. Print Spectrum to Serial Monitor
  Serial.println("\n--- FFT Spectrum Analysis ---");
  Serial.print("Dominant Peak: ");
  Serial.print(peakFrequency, 1);
  Serial.println(" Hz");
  Serial.println("Freq (Hz) | Magnitude");

  // Loop through bins (skip bin 0 = DC offset)
  for (int i = 1; i < (SAMPLES / 2); i++) {
    double freq = (i * 1.0 * SAMPLING_FREQ) / SAMPLES;
    
    // Print frequency label
    if (freq < 100) Serial.print(" ");
    Serial.print((int)freq);
    Serial.print(" Hz   | ");

    // Print magnitude value
    Serial.print((int)vReal[i]);
    Serial.print("\t");

    // Draw ASCII bar for visual feedback
    int barLength = map((int)vReal[i], 0, 2000, 0, 40);
    barLength = constrain(barLength, 0, 40);
    for (int b = 0; b < barLength; b++) {
      Serial.print("=");
    }
    Serial.println();
  }

  delay(400);
}