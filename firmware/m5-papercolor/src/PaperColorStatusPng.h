#pragma once

#include <Arduino.h>

#include <string>

namespace inkloop {

struct GeneratedStatusPng {
  uint8_t* bytes = nullptr;
  size_t length = 0;
  void release();
  ~GeneratedStatusPng() { release(); }
};

bool validPairingStatusInputs(
    const std::string& sixDigitCode,
    const std::string& bindingUrl);

// Produces an exact 400x600 RGB PNG in the bottom-down device orientation with
// a fixed MyAI activation QR target and the authoritative shared six-digit
// code. The output uses uncompressed
// DEFLATE so firmware does not depend on an unbounded image encoder.
bool makePairingStatusPng(
    const std::string& sixDigitCode,
    const std::string& bindingUrl,
    GeneratedStatusPng& output);

// Produces a code-free terminal screen. It deliberately contains no QR input,
// URL, device identifier, or digits from the completed onboarding secret.
bool makeBoundStatusPng(GeneratedStatusPng& output);

// Fixed nonsecret failure screen used when the public MyAI response does not
// provide both an authoritative six-digit code and a valid HTTPS binding URL.
// It contains no QR and never substitutes a local Wi-Fi target.
bool makePairingUnavailableStatusPng(GeneratedStatusPng& output);

}  // namespace inkloop
