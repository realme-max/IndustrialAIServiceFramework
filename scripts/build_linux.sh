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

cmake \
  -S "${project_root}" \
  -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_BUILD_LINUX_NETWORK=ON \
  -DIAISF_USE_SYSTEM_DEPS=OFF

cmake --build "${build_dir}" --parallel
