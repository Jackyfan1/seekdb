#!/usr/bin/env bash
# Build and run the hipVS CAGRA build/search benchmark.
# Usage: ./build_and_run.sh [n] [dim] [k]
set -euo pipefail

cd "$(dirname "$0")"

N=${1:-100000}
DIM=${2:-128}
K=${3:-10}
ROCM_PATH=${ROCM_PATH:-/opt/rocm}

echo "[1/2] compiling hipvs_cagra_bench (ROCm: ${ROCM_PATH})"
hipcc -O3 -std=c++17 -fopenmp \
  -I"${ROCM_PATH}/include" \
  hipvs_cagra_bench.cpp -o hipvs_cagra_bench \
  -L"${ROCM_PATH}/lib" -lcuvs

echo "[2/2] running benchmark n=${N} dim=${DIM} k=${K}"
LD_LIBRARY_PATH="${ROCM_PATH}/lib:${LD_LIBRARY_PATH:-}" ./hipvs_cagra_bench "${N}" "${DIM}" "${K}"
