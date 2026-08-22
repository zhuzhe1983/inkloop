#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected_version="$(tr -d '[:space:]' < "${project_dir}/.idf-version")"
mode="${1:-}"

if [[ -n "${mode}" && "${mode}" != "--clean" ]]; then
  echo "Usage: $0 [--clean]" >&2
  exit 2
fi

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is required; expected ESP-IDF ${expected_version}." >&2
  exit 2
fi

if [[ ! -f "${IDF_PATH}/export.sh" ]]; then
  echo "IDF_PATH does not contain export.sh: ${IDF_PATH}" >&2
  exit 2
fi

actual_version="$(git -C "${IDF_PATH}" describe --tags --always 2>/dev/null || true)"
if [[ "${actual_version}" != "${expected_version}" ]]; then
  echo "ESP-IDF version mismatch: expected ${expected_version}, got ${actual_version:-unknown}." >&2
  exit 2
fi

# ESP-IDF owns the environment setup; shellcheck cannot resolve its generated paths.
# shellcheck disable=SC1090
source "${IDF_PATH}/export.sh"

cd "${project_dir}"
if [[ "${mode}" == "--clean" || ! -f sdkconfig ]]; then
  idf.py set-target esp32s3
elif ! grep -q '^CONFIG_IDF_TARGET="esp32s3"$' sdkconfig; then
  echo "Existing sdkconfig is not for esp32s3; rerun with --clean." >&2
  exit 2
fi
idf.py build
