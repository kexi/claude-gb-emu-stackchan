#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
# The repository-controlled lock keeps the source, output name, and digest together.
# The source path is resolved from this script's absolute directory.
# shellcheck disable=SC1091
source "${project_dir}/rom.lock"

rom_path="${project_dir}/${ROM_FILE}"
if [[ -f "${rom_path}" ]]; then
    actual_sha="$(sha256sum "${rom_path}" | cut -d ' ' -f 1)"
    is_current_rom="$([[ "${actual_sha}" == "${ROM_SHA256}" ]] && echo true || echo false)"
    if [[ "${is_current_rom}" == "true" ]]; then
        exit 0
    fi
fi

mkdir -p "$(dirname "${rom_path}")"
download_path="$(mktemp "${rom_path}.download.XXXXXX")"
trap 'rm -f "${download_path}"' EXIT
curl --fail --location --proto '=https' --tlsv1.2 --output "${download_path}" "${ROM_URL}"

actual_sha="$(sha256sum "${download_path}" | cut -d ' ' -f 1)"
is_expected_rom="$([[ "${actual_sha}" == "${ROM_SHA256}" ]] && echo true || echo false)"
if [[ "${is_expected_rom}" != "true" ]]; then
    echo "ROM checksum mismatch: expected ${ROM_SHA256}, got ${actual_sha}" >&2
    exit 1
fi

mv "${download_path}" "${rom_path}"
trap - EXIT
