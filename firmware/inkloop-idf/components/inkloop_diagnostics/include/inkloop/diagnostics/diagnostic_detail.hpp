#pragma once

#include <cstddef>
#include <string>

namespace inkloop::diagnostics {

// Status details may originate at a remote service.  Before one crosses into
// the Portal, local chat, ESP log or typed serial diagnostics, reduce it to a
// single bounded UTF-8 line and reject credential-shaped material.  Callers
// still publish stable numeric error/source/http fields when the detail is
// empty, so redaction never hides the actionable error classification.
inline constexpr size_t kMaximumDiagnosticDetailBytes = 160U;

std::string sanitizeDiagnosticDetail(
    const std::string& input,
    size_t maximum_bytes = kMaximumDiagnosticDetailBytes);

// Defense-in-depth for fixed diagnostic envelopes: true only when `value` is
// already in the canonical form returned by sanitizeDiagnosticDetail().
bool isCanonicalDiagnosticDetail(const std::string& value,
                                 size_t maximum_bytes =
                                     kMaximumDiagnosticDetailBytes);

}  // namespace inkloop::diagnostics
