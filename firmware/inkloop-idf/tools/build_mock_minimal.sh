#!/usr/bin/env bash
set -euo pipefail

tool_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$tool_dir/.." && pwd)"
proof_root="$project_root/build-mock-minimal"
project_dir="$proof_root/project"
build_dir="$proof_root/build-esp32s3"
cache="$build_dir/CMakeCache.txt"

mkdir -p "$project_dir"
for entry in CMakeLists.txt boards components main partitions.csv sdkconfig.defaults version.txt; do
  target="$project_dir/$entry"
  if [[ -e "$target" && ! -L "$target" ]]; then
    echo "refusing non-symlink proof input: $target" >&2
    exit 2
  fi
  ln -sfn "$project_root/$entry" "$target"
done

if [[ -f "$cache" ]] &&
   ! grep -q '^INKLOOP_BOARD:STRING=mock_minimal$' "$cache"; then
  echo "refusing non-mock CMake cache: $cache" >&2
  exit 2
fi
if [[ -f "$cache" ]] &&
   ! grep -q '^IDF_TARGET:STRING=esp32s3$' "$cache"; then
  echo "refusing non-ESP32-S3 CMake cache: $cache" >&2
  exit 2
fi

idf_py="${IDF_PY:-idf.py}"
cd "$project_dir"
exec "$idf_py" \
  -B "$build_dir" \
  -DIDF_TARGET=esp32s3 \
  -DINKLOOP_BOARD=mock_minimal \
  -DSDKCONFIG="$build_dir/sdkconfig" \
  build
