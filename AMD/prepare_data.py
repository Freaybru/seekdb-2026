#!/usr/bin/env python3
"""Prepare public SIFT subsets or reproducible synthetic FP32 L2 fixtures.

Ground truth is exhaustive over the selected base vectors, never the 1M-base
SIFT ground-truth file. Dataset preparation runs before all measured builds.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def read_fvecs(path, count):
    dim = int(np.fromfile(path, dtype="<i4", count=1)[0])
    if not 0 < dim <= 65536 or path.stat().st_size % (4 * (dim + 1)):
        raise ValueError("Invalid fvecs dimensions or file size")
    records = np.memmap(path, dtype="<i4", mode="r").reshape(-1, dim + 1)
    if len(records) < count or not np.all(records[:count, 0] == dim):
        raise ValueError("Not enough fvecs rows or inconsistent dimensions")
    return np.ascontiguousarray(records[:count, 1:].view("<f4"))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--sift-base", type=Path)
    p.add_argument("--sift-queries", type=Path)
    p.add_argument("--kind", choices=("gaussian", "clustered"), default="gaussian")
    p.add_argument("--rows", type=int, default=250000)
    p.add_argument("--dim", type=int, default=256)
    p.add_argument("--nq", type=int, default=4096)
    p.add_argument("--seed", type=int, default=20261104)
    p.add_argument("--truth-device", choices=("cpu", "cuda"), default="cpu")
    a = p.parse_args()
    if a.rows < 100 or a.nq < 1 or a.dim < 1:
        p.error("Need rows>=100, nq>=1 and dim>=1")
    a.output.mkdir(parents=True, exist_ok=False)
    if bool(a.sift_base) != bool(a.sift_queries):
        p.error("Provide both SIFT input files")
    if a.sift_base:
        base = read_fvecs(a.sift_base, a.rows)
        queries = read_fvecs(a.sift_queries, a.nq)
        if base.shape[1] != queries.shape[1]:
            raise ValueError("Base and query dimensions differ")
        source = "public SIFT1M, first selected base and query rows"
    else:
        rng = np.random.default_rng(a.seed)
        if a.kind == "gaussian":
            base = rng.standard_normal((a.rows, a.dim), dtype=np.float32)
            queries = rng.standard_normal((a.nq, a.dim), dtype=np.float32)
        else:
            centers = rng.standard_normal((256, a.dim), dtype=np.float32) * 1.2
            scale = np.geomspace(0.3, 1.8, a.dim).astype(np.float32)
            base = (rng.standard_normal((a.rows, a.dim), dtype=np.float32)
                    + centers[rng.integers(0, len(centers), a.rows)]) * scale
            queries = (rng.standard_normal((a.nq, a.dim), dtype=np.float32)
                       + centers[rng.integers(0, len(centers), a.nq)]) * scale
        source = f"synthetic {a.kind}, independent queries, seed={a.seed}"
    if not np.isfinite(base).all() or not np.isfinite(queries).all():
        raise ValueError("Non-finite input")
    np.save(a.output / "base.npy", base)
    np.save(a.output / "queries.npy", queries)
    truth = []
    if a.truth_device == "cuda":
        # Optional preparation accelerator, not part of seekdb or its build.
        import torch
        torch.backends.cuda.matmul.allow_tf32 = False
        torch.set_num_threads(12)
        b = torch.from_numpy(base).cuda()
        bn = (b*b).sum(1)
        for i in range(0, len(queries), 128):
            q = torch.from_numpy(queries[i:i+128]).cuda()
            distances = (q*q).sum(1, keepdim=True) + bn[None, :] - 2*q@b.T
            truth.append(distances.topk(100, largest=False, sorted=True).indices.cpu().numpy())
    else:
        bn = (base*base).sum(1)
        for i in range(0, len(queries), 128):
            q = queries[i:i+128]
            distances = (q*q).sum(1, keepdims=True) + bn[None, :] - 2*q@base.T
            ids = np.argpartition(distances, 99, axis=1)[:, :100]
            selected = np.take_along_axis(distances, ids, axis=1)
            truth.append(np.take_along_axis(ids, np.argsort(selected, axis=1), axis=1))
    np.save(a.output / "truth.npy", np.concatenate(truth))
    metadata = {"source":source, "rows":len(base), "dim":base.shape[1], "nq":len(queries),
                "dtype":"float32", "distance":"squared L2",
                "ground_truth":"exhaustive FP32; reduced-precision matmul disabled",
                "truth_device":a.truth_device, "sha256":{}}
    for name in ("base.npy", "queries.npy", "truth.npy"):
        with (a.output / name).open("rb") as stream:
            metadata["sha256"][name] = hashlib.file_digest(stream, "sha256").hexdigest()
    (a.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata))


if __name__ == "__main__":
    main()
