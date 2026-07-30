#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
server="${project_root}/build/linux-release/iaisf_server"
example_config="${project_root}/config/iaisf.example.json"

if [[ $# -ne 0 ]]; then
  echo "usage: $0" >&2
  exit 2
fi

if [[ ! -x "${server}" ]]; then
  echo "release executable is missing or not executable: ${server}" >&2
  echo "run scripts/build_linux.sh Release first" >&2
  exit 1
fi

"${server}" --version
"${server}" --config "${example_config}"

