#!/usr/bin/env python3
"""CPU/GPU reference benchmark mirroring hipvs_cagra_bench.cpp.

hipVS exposes the same C API as upstream cuVS, so this script keeps the exact
same parameters and measurement method and only swaps the dependency:

  n=100000, dim=128, k=10, queries=100
  graph_degree=64, intermediate_graph_degree=96, metric=L2, build_algo=nn_descent

Usage: python bench_cuvs_reference.py [n] [dim] [k] [nq]

It runs on any CUDA GPU (or CPU-only for the hnswlib baseline). Numbers are a
*methodology* reference, not an AMD result.
"""
import sys
import time

import numpy as np


def log(msg):
    print(msg, flush=True)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100_000
    dim = int(sys.argv[2]) if len(sys.argv) > 2 else 128
    k = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    nq = int(sys.argv[4]) if len(sys.argv) > 4 else min(100, n)
    graph_degree = 64
    intermediate_degree = 96

    rng = np.random.default_rng(42)
    base = rng.uniform(-1.0, 1.0, size=(n, dim)).astype(np.float32)
    query = rng.uniform(-1.0, 1.0, size=(nq, dim)).astype(np.float32)
    log(f"dataset: n={n} dim={dim} k={k} queries={nq}")

    results = {}

    # ---------------- CPU exact ground truth ----------------
    log("\n[1] CPU exact ground truth (numpy, squared L2)")
    t0 = time.perf_counter()
    x2 = np.einsum("ij,ij->i", base, base)
    q2 = np.einsum("ij,ij->i", query, query)
    gt = np.empty((nq, k), dtype=np.int64)
    chunk = max(1, min(512, int(2e7 // max(n, 1))))
    for s in range(0, nq, chunk):
        e = min(s + chunk, nq)
        d = q2[s:e, None] + x2[None, :] - 2.0 * (query[s:e] @ base.T)
        part = np.argpartition(d, k, axis=1)[:, :k]
        gt[s:e] = np.take_along_axis(
            part, np.argsort(np.take_along_axis(d, part, axis=1), axis=1), axis=1
        )
    gt_ms = (time.perf_counter() - t0) * 1000.0
    results["cpu_gt_ms"] = gt_ms
    log(f"    {gt_ms:10.2f} ms")

    # ---------------- CPU HNSW baseline (hnswlib) ----------------
    log("\n[2] CPU HNSW build + search (hnswlib, M=16 ef_c=200)")
    import hnswlib

    t0 = time.perf_counter()
    cpu_index = hnswlib.Index(space="l2", dim=dim)
    cpu_index.init_index(max_elements=n, ef_construction=200, M=16)
    cpu_index.add_items(base, np.arange(n))
    cpu_index.set_ef(128)
    cpu_build_ms = (time.perf_counter() - t0) * 1000.0
    results["cpu_hnsw_build_ms"] = cpu_build_ms
    log(f"    build {cpu_build_ms:10.2f} ms")

    t0 = time.perf_counter()
    labels, _ = cpu_index.knn_query(query, k=k)
    cpu_search_ms = (time.perf_counter() - t0) * 1000.0
    results["cpu_hnsw_search_ms"] = cpu_search_ms
    hit = sum(len(set(labels[q]) & set(gt[q])) for q in range(nq))
    results["cpu_hnsw_recall"] = hit / (nq * k)
    log(f"    search {cpu_search_ms:10.2f} ms  recall@{k}={results['cpu_hnsw_recall']:.4f}")

    # ---------------- GPU CAGRA (cuVS, same API as hipVS) ----------------
    log("\n[3] GPU CAGRA build + search (cuVS / hipVS-identical C API)")
    import cupy as cp
    from cuvs.neighbors import cagra

    t0 = time.perf_counter()
    d_base = cp.asarray(base)
    d_query = cp.asarray(query)
    cp.cuda.Stream.null.synchronize()
    h2d_ms = (time.perf_counter() - t0) * 1000.0
    results["h2d_ms"] = h2d_ms
    log(f"    H2D copy {h2d_ms:10.2f} ms")

    metric = "sqeuclidean"
    params = cagra.IndexParams(
        metric=metric,
        graph_degree=graph_degree,
        intermediate_graph_degree=intermediate_degree,
        build_algo="nn_descent",
    )

    t0 = time.perf_counter()
    index = cagra.build(params, d_base)
    cp.cuda.Stream.null.synchronize()
    gpu_build_ms = (time.perf_counter() - t0) * 1000.0
    results["gpu_build_ms"] = gpu_build_ms
    log(f"    build {gpu_build_ms:10.2f} ms  ({n / max(gpu_build_ms, 1e-6):.1f} K vec/s)")

    search_params = cagra.SearchParams()
    t0 = time.perf_counter()
    distances, neighbors = cagra.search(search_params, index, d_query, k)
    cp.cuda.Stream.null.synchronize()
    gpu_search_ms = (time.perf_counter() - t0) * 1000.0
    results["gpu_search_ms"] = gpu_search_ms
    nb = neighbors.copy_to_host()
    hit = sum(len(set(nb[q].tolist()) & set(gt[q].tolist())) for q in range(nq))
    results["gpu_recall"] = hit / (nq * k)
    log(f"    search {gpu_search_ms:10.2f} ms  "
        f"({nq / max(gpu_search_ms, 1e-6) * 1000.0:.0f} q/s, "
        f"{gpu_search_ms / nq:.3f} ms/query)  recall@{k}={results['gpu_recall']:.4f}")

    # ---------------- summary ----------------
    log("\n===== summary (cuVS reference run, not an AMD result) =====")
    log(f"build  cpu_hnsw        : {results['cpu_hnsw_build_ms']:10.2f} ms")
    log(f"build  gpu_cagra       : {results['gpu_build_ms']:10.2f} ms"
        f"   speedup x{results['cpu_hnsw_build_ms'] / max(results['gpu_build_ms'], 1e-6):.1f}")
    log(f"search cpu_hnsw        : {results['cpu_hnsw_search_ms']:10.2f} ms"
        f"   recall {results['cpu_hnsw_recall']:.4f}")
    log(f"search gpu_cagra       : {results['gpu_search_ms']:10.2f} ms"
        f"   recall {results['gpu_recall']:.4f}")
    log(f"cpu exact ground truth : {results['cpu_gt_ms']:10.2f} ms")
    log(f"h2d copy               : {results['h2d_ms']:10.2f} ms")
    log(f"graph_degree/intermed  : {graph_degree} / {intermediate_degree}, metric={metric}")


if __name__ == "__main__":
    main()
