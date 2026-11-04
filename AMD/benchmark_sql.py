#!/usr/bin/env python3
"""Fresh SQL DDL + first-query timing and single-row ANN validation.

Test-only dependencies: numpy and PyMySQL. Never used by the production build.
Each invocation creates a new private run directory and stops only its own server.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time

import numpy as np
import pymysql


def literal(vector):
    return "'[" + ",".join(format(float(x), ".9g") for x in vector) + "]'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--bridge", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--backend", choices=("cuvs", "vsag"), default="cuvs")
    parser.add_argument("--nq", type=int, default=4096)
    parser.add_argument("--restart", action="store_true")
    parser.add_argument("--concurrent", type=int, default=0,
                        help="Additional post-timing SQL concurrency regression")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    bridge = args.bridge.resolve(strict=True)
    data = args.data.resolve(strict=True)
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    (work / "tmp").mkdir()
    base = np.load(data / "base.npy", mmap_mode="r")
    queries = np.load(data / "queries.npy")[:args.nq]
    truth = np.load(data / "truth.npy")[:len(queries)]
    if len(queries) == 0 or len(base) < 100:
        raise ValueError("Need queries and at least 100 base vectors")
    # Non-contiguous signed external IDs test the real SQL ID mapping.
    offset, multiplier = 10000019, 1009
    ids = np.arange(len(base), dtype=np.int64) * multiplier - offset
    tsv = work / "items.tsv"
    with tsv.open("w") as out:
        for ident, vector in zip(ids, base):
            out.write(str(int(ident)) + "\t" + literal(vector)[1:-1] + "\n")
    sql_queries = [literal(q) for q in queries]
    env = os.environ.copy()
    env["TMPDIR"] = str(work / "tmp")
    env["LD_LIBRARY_PATH"] = ":".join((str(bridge.parent), "/opt/hipvs/lib", "/opt/rocm/lib",
                                         env.get("LD_LIBRARY_PATH", "")))
    cpus = sorted(os.sched_getaffinity(0))[:24]
    command = ["taskset", "-c", ",".join(map(str, cpus)), str(binary), "--nodaemon",
               "--port", str(args.port), "--data-dir", str(work / "store"),
               "--base-dir", str(work),
               "--parameter", "__min_full_resource_pool_memory=2147483648",
               "--parameter", "memory_budget=16G", "--parameter", "datafile_size=20G",
               "--parameter", "cpu_count=24", "--parameter", "local_ip=127.0.0.1"]
    server = None
    log = None
    connection = None
    result = {"backend":args.backend, "rows":len(base), "dim":base.shape[1],
              "nq":len(queries), "cpu_slots":len(cpus), "status":"running"}
    stopped = threading.Event()
    gpu_memory = None
    if args.backend == "cuvs":
        buses = json.loads(subprocess.check_output(["rocm-smi", "--showbus", "--json"], text=True))
        bus = next(value["PCI Bus"] for value in buses.values() if "PCI Bus" in value)
        gpu_memory = Path("/sys/bus/pci/devices") / bus.lower() / "mem_info_vram_used"
        if not gpu_memory.is_file():
            raise RuntimeError("GPU memory counter unavailable")
    result["vram_peak_bytes"] = 0
    result["server_rss_peak_bytes"] = 0

    def sample_resources():
        while not stopped.is_set():
            try:
                if gpu_memory:
                    result["vram_peak_bytes"] = max(result["vram_peak_bytes"], int(gpu_memory.read_text()))
                proc = server
                if proc:
                    status = Path(f"/proc/{proc.pid}/status").read_text()
                    rss = next(int(line.split()[1])*1024 for line in status.splitlines() if line.startswith("VmHWM:"))
                    result["server_rss_peak_bytes"] = max(result["server_rss_peak_bytes"], rss)
            except (OSError, StopIteration):
                pass
            stopped.wait(0.05)

    sampler = threading.Thread(target=sample_resources, daemon=True)
    sampler.start()

    def start_server():
        nonlocal server, log, connection
        log = (work / "server.log").open("ab")
        server = subprocess.Popen(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            if server.poll() is not None:
                raise RuntimeError("Server exited before SQL readiness; inspect server.log")
            try:
                connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root",
                    autocommit=True, local_infile=True, connect_timeout=3,
                    read_timeout=600, write_timeout=600)
                with connection.cursor() as cur:
                    cur.execute("SET ob_query_timeout=600000000")
                return
            except pymysql.MySQLError:
                time.sleep(1)
        raise RuntimeError("SQL readiness timeout")

    def stop_server():
        nonlocal server, log, connection
        if connection:
            connection.close()
            connection = None
        if server:
            server.terminate()
            try:
                server.wait(timeout=30)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=10)
            server = None
        if log:
            log.close()
            log = None

    def execute(sql):
        with connection.cursor() as cur:
            cur.execute(sql)
            return cur.fetchall()

    def query(vector, k):
        rows = execute(f"SELECT id FROM amd_bench.items ORDER BY l2_distance(v, {vector}) APPROXIMATE LIMIT {k}")
        returned = [int(row[0]) for row in rows]
        if len(returned) != k or len(set(returned)) != k:
            raise AssertionError("Missing or duplicate result ID")
        offsets = []
        for ident in returned:
            index, remainder = divmod(ident + offset, multiplier)
            if remainder or not 0 <= index < len(base):
                raise AssertionError("Illegal external result ID")
            offsets.append(index)
        return offsets

    try:
        start_server()
        execute("ALTER SYSTEM SET ob_vector_memory_limit_percentage=50")
        execute("CREATE DATABASE amd_bench")
        execute(f"CREATE TABLE amd_bench.items(id BIGINT PRIMARY KEY, v VECTOR({base.shape[1]}))")
        execute("LOAD DATA LOCAL INFILE '" + str(tsv).replace("'", "''") + "' INTO TABLE amd_bench.items "
                "FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id,v)")
        assert execute("SELECT COUNT(*) FROM amd_bench.items")[0][0] == len(base)
        print("Imported", len(base), "rows", flush=True)
        start = time.perf_counter()
        execute("CREATE VECTOR INDEX idx_items ON amd_bench.items(v) "
                f"WITH (distance=l2, type=hnsw, lib={args.backend}, m=16, ef_construction=200, ef_search=64)")
        result["ddl_s"] = time.perf_counter() - start
        first = query(sql_queries[0], 10)
        result["build_through_first_s"] = time.perf_counter() - start
        for k in (10, 100):
            elapsed = []
            hits = 0
            start = time.perf_counter()
            for i, vector in enumerate(sql_queries):
                query_start = time.perf_counter()
                found = query(vector, k)
                elapsed.append(time.perf_counter() - query_start)
                hits += len(set(found).intersection(truth[i, :k]))
            total = time.perf_counter() - start
            result[str(k)] = {"qps":len(queries)/total, "p95_ms":float(np.percentile(elapsed,95)*1000),
                               "recall":hits/(len(queries)*k), "result_integrity":True}
            print(k, result[str(k)], flush=True)
        if args.concurrent:
            expected = query(sql_queries[0], 10)
            def concurrent_client(_):
                client = pymysql.connect(host="127.0.0.1", port=args.port, user="root",
                                         autocommit=True, read_timeout=60)
                try:
                    with client.cursor() as cur:
                        for repeat in range(32):
                            cur.execute("SELECT id FROM amd_bench.items ORDER BY l2_distance(v, "
                                        + sql_queries[0] + ") APPROXIMATE LIMIT 10")
                            found = [divmod(int(row[0]) + offset, multiplier)[0] for row in cur.fetchall()]
                            if found != expected:
                                raise AssertionError("Concurrent SQL changed results")
                finally:
                    client.close()
            with ThreadPoolExecutor(max_workers=args.concurrent) as pool:
                list(pool.map(concurrent_client, range(args.concurrent)))
            result["concurrent_clients"] = args.concurrent
            result["concurrent_queries"] = 32 * args.concurrent
        if args.restart:
            stop_server()
            start_server()
            restarted = query(sql_queries[0], 10)
            assert restarted == first, "Snapshot reload changed first-query results"
            result["restart_same_ids"] = True
        result["status"] = "pass"
    finally:
        stop_server()
        stopped.set()
        sampler.join()
        if args.backend == "cuvs":
            serialized = reloaded = fallbacks = 0
            for path in (work / "log").glob("seekdb.log*"):
                with path.open(errors="replace") as stream:
                    for line in stream:
                        serialized += " serialized GPU CAGRA snapshot" in line
                        reloaded += " deserialized GPU CAGRA snapshot" in line
                        fallbacks += "lazily built VSAG fallback" in line
            result["gpu_snapshots_serialized"] = serialized
            result["gpu_snapshots_reloaded"] = reloaded
            result["cpu_fallbacks"] = fallbacks
            if result["status"] == "pass" and (not serialized or fallbacks or (args.restart and not reloaded)):
                result["status"] = "fail_gpu_lifecycle"
        result["binary_sha256"] = hashlib.file_digest(binary.open("rb"), "sha256").hexdigest()
        result["bridge_sha256"] = hashlib.file_digest(bridge.open("rb"), "sha256").hexdigest()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    if result["status"] != "pass":
        raise RuntimeError("Validation failed: " + result["status"])


if __name__ == "__main__":
    main()
