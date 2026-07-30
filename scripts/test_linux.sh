#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <Debug|Release>" >&2
  exit 2
fi

build_type="$1"
case "${build_type}" in
  Debug)
    build_dir="${project_root}/build/linux-debug"
    ;;
  Release)
    build_dir="${project_root}/build/linux-release"
    ;;
  *)
    echo "unsupported build type: ${build_type}" >&2
    echo "usage: $0 <Debug|Release>" >&2
    exit 2
    ;;
esac

if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
  echo "build directory is not configured: ${build_dir}" >&2
  echo "run scripts/build_linux.sh ${build_type} first" >&2
  exit 1
fi

ctest --test-dir "${build_dir}" --output-on-failure

