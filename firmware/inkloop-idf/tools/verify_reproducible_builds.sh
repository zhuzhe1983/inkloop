#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
expected_version="$(tr -d '[:space:]' < "${project_dir}/.idf-version")"
expected_idf_commit="$(tr -d '[:space:]' < "${project_dir}/.idf-commit")"
requested_output="${1:-}"

git_without_replacements() {
  GIT_NO_REPLACE_OBJECTS=1 command git "$@"
}

if [[ ! "${expected_idf_commit}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "Invalid ESP-IDF commit pin in ${project_dir}/.idf-commit." >&2
  exit 2
fi

if [[ "$#" -ne 1 || -z "${requested_output}" || "${requested_output}" != /* ]]; then
  echo "Usage: IDF_PATH=/absolute/esp-idf $0 /absolute/new-output-root" >&2
  exit 2
fi
if [[ -e "${requested_output}" || -L "${requested_output}" ]]; then
  echo "Output root must not already exist: ${requested_output}" >&2
  exit 2
fi

output_parent="$(dirname "${requested_output}")"
output_name="$(basename "${requested_output}")"
if [[ ! -d "${output_parent}" || "${output_name}" == "." ||
      "${output_name}" == ".." || "${output_name}" == */* ]]; then
  echo "Output parent must be an existing directory." >&2
  exit 2
fi
output_parent="$(cd "${output_parent}" && pwd -P)"
output_root="${output_parent}/${output_name}"
case "${output_root}/" in
  "${project_dir}/"*)
    echo "Reproducibility evidence must be stored outside the project tree." >&2
    exit 2
    ;;
esac

if ! project_git_root="$(git_without_replacements -C "${project_dir}" rev-parse --show-toplevel 2>/dev/null)"; then
  echo "Project must be inside a Git worktree." >&2
  exit 2
fi
project_git_root="$(cd "${project_git_root}" && pwd -P)"
if ! project_replace_refs="$(git_without_replacements -C "${project_git_root}" for-each-ref --format='%(refname)' refs/replace/ 2>/dev/null)"; then
  echo "Unable to inspect project Git replacement refs." >&2
  exit 2
fi
if [[ -n "${project_replace_refs}" ]]; then
  echo "Project Git worktree must not contain refs/replace entries." >&2
  exit 2
fi
if ! project_commit="$(git_without_replacements -C "${project_git_root}" rev-parse --verify 'HEAD^{commit}' 2>/dev/null)"; then
  echo "Project Git worktree must have a committed HEAD." >&2
  exit 2
fi
if ! project_status="$(git_without_replacements -C "${project_git_root}" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)"; then
  echo "Unable to inspect the project Git worktree." >&2
  exit 2
fi
if [[ -n "${project_status}" ]]; then
  echo "Project Git worktree must be clean before reproducibility verification." >&2
  exit 2
fi

if [[ -z "${IDF_PATH:-}" || "${IDF_PATH}" != /* ||
      ! -f "${IDF_PATH}/export.sh" ]]; then
  echo "IDF_PATH must name an absolute ESP-IDF checkout with export.sh." >&2
  exit 2
fi
idf_path="$(cd "${IDF_PATH}" && pwd -P)"
if ! idf_git_root="$(git_without_replacements -C "${idf_path}" rev-parse --show-toplevel 2>/dev/null)"; then
  echo "IDF_PATH must name an ESP-IDF Git worktree." >&2
  exit 2
fi
idf_git_root="$(cd "${idf_git_root}" && pwd -P)"
if [[ "${idf_git_root}" != "${idf_path}" ]]; then
  echo "IDF_PATH must name the root of the ESP-IDF Git worktree." >&2
  exit 2
fi
if ! idf_replace_refs="$(git_without_replacements -C "${idf_path}" for-each-ref --format='%(refname)' refs/replace/ 2>/dev/null)"; then
  echo "Unable to inspect ESP-IDF Git replacement refs." >&2
  exit 2
fi
if [[ -n "${idf_replace_refs}" ]]; then
  echo "ESP-IDF Git worktree must not contain refs/replace entries." >&2
  exit 2
fi
if ! actual_idf_commit="$(git_without_replacements -C "${idf_path}" rev-parse --verify 'HEAD^{commit}' 2>/dev/null)"; then
  echo "ESP-IDF Git worktree must have a committed HEAD." >&2
  exit 2
fi
if [[ "${actual_idf_commit}" != "${expected_idf_commit}" ]]; then
  echo "ESP-IDF commit mismatch: expected ${expected_idf_commit}, got ${actual_idf_commit}." >&2
  exit 2
fi
if ! tag_commit="$(git_without_replacements -C "${idf_path}" rev-parse --verify "refs/tags/${expected_version}^{commit}" 2>/dev/null)" ||
   [[ "${tag_commit}" != "${expected_idf_commit}" ]]; then
  echo "ESP-IDF tag mismatch: ${expected_version} must resolve to ${expected_idf_commit}." >&2
  exit 2
fi
if ! idf_status="$(git_without_replacements -C "${idf_path}" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)"; then
  echo "Unable to inspect the ESP-IDF Git worktree." >&2
  exit 2
fi
if [[ -n "${idf_status}" ]]; then
  echo "ESP-IDF Git worktree must be clean." >&2
  exit 2
fi

idf_py_candidate="${idf_path}/tools/idf.py"
if [[ ! -f "${idf_py_candidate}" || -L "${idf_py_candidate}" ||
      ! -x "${idf_py_candidate}" ]]; then
  echo "ESP-IDF tools/idf.py must be an executable regular non-symlink file." >&2
  exit 2
fi
if ! idf_py="$(realpath "${idf_py_candidate}" 2>/dev/null)"; then
  echo "Unable to resolve ESP-IDF tools/idf.py." >&2
  exit 2
fi
case "${idf_py}" in
  "${idf_path}/"*) ;;
  *)
    echo "ESP-IDF tools/idf.py must resolve inside the pinned IDF worktree." >&2
    exit 2
    ;;
esac
readonly idf_py

# ESP-IDF owns the toolchain environment assembled by this pinned export.
# shellcheck disable=SC1090
unset IDF_PY
source "${idf_path}/export.sh"
export IDF_COMPONENT_STRICT_CHECKSUM=1

mkdir "${output_root}"

last_hash=""
last_size=""
build_once() {
  local board="$1"
  local label="$2"
  local copy="$3"
  local build_dir="${output_root}/${label}-${copy}"
  local sdkconfig="${build_dir}/sdkconfig"
  local binary="${build_dir}/inkloop_idf.bin"
  local cache="${build_dir}/CMakeCache.txt"

  local build_args=(
    -C "${project_dir}"
    -B "${build_dir}"
    -DIDF_TARGET=esp32s3
    -DINKLOOP_BOARD="${board}"
    -DSDKCONFIG="${sdkconfig}"
    build
  )
  "${idf_py}" "${build_args[@]}"

  grep -qx 'CONFIG_APP_REPRODUCIBLE_BUILD=y' "${sdkconfig}" ||
    { echo "${label}-${copy}: reproducible build is disabled" >&2; exit 1; }
  if grep -qx 'CONFIG_APP_COMPILE_TIME_DATE=y' "${sdkconfig}"; then
    echo "${label}-${copy}: app compile time is embedded" >&2
    exit 1
  fi
  if grep -qx 'CONFIG_BOOTLOADER_COMPILE_TIME_DATE=y' "${sdkconfig}"; then
    echo "${label}-${copy}: bootloader compile time is embedded" >&2
    exit 1
  fi
  grep -qx 'CONFIG_IDF_TARGET="esp32s3"' "${sdkconfig}" ||
    { echo "${label}-${copy}: unexpected IDF target" >&2; exit 1; }
  grep -qx "INKLOOP_BOARD:STRING=${board}" "${cache}" ||
    { echo "${label}-${copy}: unexpected board cache" >&2; exit 1; }
  if [[ ! -f "${binary}" || -L "${binary}" ]]; then
    echo "${label}-${copy}: missing direct application binary" >&2
    exit 1
  fi

  last_hash="$(shasum -a 256 "${binary}" | awk '{print $1}')"
  last_size="$(wc -c < "${binary}" | tr -d '[:space:]')"
}

build_once "m5_papercolor_c151" "c151" "a"
c151_a_hash="${last_hash}"
c151_a_size="${last_size}"
build_once "m5_papercolor_c151" "c151" "b"
c151_b_hash="${last_hash}"
c151_b_size="${last_size}"
if [[ "${c151_a_hash}" != "${c151_b_hash}" ||
      "${c151_a_size}" != "${c151_b_size}" ]]; then
  echo "C151 reproducibility mismatch: ${c151_a_hash} != ${c151_b_hash}" >&2
  exit 1
fi

build_once "mock_minimal" "mock" "a"
mock_a_hash="${last_hash}"
mock_a_size="${last_size}"
build_once "mock_minimal" "mock" "b"
mock_b_hash="${last_hash}"
mock_b_size="${last_size}"
if [[ "${mock_a_hash}" != "${mock_b_hash}" ||
      "${mock_a_size}" != "${mock_b_size}" ]]; then
  echo "mock-minimal reproducibility mismatch: ${mock_a_hash} != ${mock_b_hash}" >&2
  exit 1
fi

printf 'C151_SHA256=%s\n' "${c151_a_hash}"
printf 'C151_BYTES=%s\n' "${c151_a_size}"
printf 'MOCK_SHA256=%s\n' "${mock_a_hash}"
printf 'MOCK_BYTES=%s\n' "${mock_a_size}"
printf 'PROJECT_COMMIT=%s\n' "${project_commit}"
printf 'IDF_COMMIT=%s\n' "${actual_idf_commit}"
printf 'EVIDENCE_ROOT=%s\n' "${output_root}"
