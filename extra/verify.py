"""Black-box SQL, durability, bounded-memory and optional timing checks."""

import argparse
import math
from pathlib import Path
import random
import resource
import shutil
import signal
import subprocess
from tempfile import TemporaryDirectory
from time import perf_counter


class Session:
    def __init__(self, binary, directory, address_space=None):
        def limit_memory():
            resource.setrlimit(resource.RLIMIT_AS, (address_space, address_space))

        self.process = subprocess.Popen(
            [str(binary), "--data-dir", str(directory)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            preexec_fn=limit_memory if address_space else None,
        )
        try:
            assert self.read() == "READY"
        except BaseException:
            self.__exit__()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()

    def read(self):
        line = self.process.stdout.readline()
        assert line, f"unexpected EOF (exit={self.process.poll()})"
        return line.rstrip("\n")

    def send(self, sql):
        self.process.stdin.write(sql + "\n")
        self.process.stdin.flush()
        return self.read()

    def ok(self, sql, affected=0):
        result = self.send(sql)
        assert result == f"OK {affected}", (sql[:120], result)

    def error(self, sql, code):
        result = self.send(sql)
        assert result.startswith(f"ERROR {code} "), (sql[:120], result)

    def query(self, sql, expected_columns=None):
        header = self.send(sql).split()
        assert len(header) == 3 and header[0] == "RESULT", (sql[:120], header)
        width, count = map(int, header[1:])
        columns = self.read().split("\t")
        assert len(columns) == width
        if expected_columns is not None:
            assert columns == expected_columns, columns
        rows = [self.read().split("\t") for _ in range(count)]
        assert all(len(row) == width for row in rows), rows
        assert self.read() == "END"
        return rows

    def quit(self):
        assert self.send("QUIT") == "BYE"
        assert self.process.wait(timeout=5) == 0

    def kill(self):
        self.process.kill()
        assert self.process.wait(timeout=5) == -signal.SIGKILL


def functional(binary, root):
    with Session(binary, root) as db:
        db.ok("CREATE TABLE Docs (label VARCHAR(3), v VECTOR(2), id BIGINT, "
              "score BIGINT, PRIMARY KEY (id));")
        db.ok("INSERT INTO docs (id, score, label, v) VALUES "
              "(4,5,'猫🦊é','[3,4]'),(2,5,'猫🦊é','[0,5]'),(5,3,'z','[0,0]'),"
              "(-9223372036854775808,2,'z','[1,1]'),(9223372036854775807,4,'a','[3,0]')", 5)
        assert db.query("SELECT id, label FROM DOCS WHERE score >= 4 AND (label > 'z') "
                        "ORDER BY score DESC LIMIT 4", ["id", "label"]) == [
                            ["2", "猫🦊é"], ["4", "猫🦊é"]]
        assert db.query("SELECT id FROM docs WHERE id < -9223372036854775808") == []
        assert db.query("SELECT id FROM docs WHERE id > 9223372036854775807") == []
        assert db.query("SELECT id FROM docs ORDER BY id DESC LIMIT 1") == [
            ["9223372036854775807"]]
        assert db.query("SELECT label, v FROM docs WHERE id = 4", ["label", "v"]) == [
            ["猫🦊é", "[3,4]"]]
        assert db.query("SELECT * FROM docs WHERE id = 4", ["label", "v", "id", "score"]) == [
            ["猫🦊é", "[3,4]", "4", "5"]]
        top = "SELECT id, L2_DISTANCE(v, '[0,0]') AS d FROM docs WHERE label = '猫🦊é' ORDER BY d LIMIT 2"
        assert db.query(top, ["id", "d"]) == [["2", "5"], ["4", "5"]]
        assert db.query("SELECT id FROM docs ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 0") == []
        for value in ("'[1]'", "'[1e39,0]'", "'[NaN,0]'", "'[Infinity,0]'", "'[1,0,]'"):
            db.error(f"INSERT INTO docs VALUES ('a',{value},8,1)", "VALUE")
        db.error("INSERT INTO docs VALUES ('四字符数','[1,0]',8,1)", "VALUE")
        db.error("INSERT INTO docs VALUES ('a','[1,0]',9223372036854775808,1)", "VALUE")
        db.error("UPDATE docs SET id=0 WHERE id=4", "UNSUPPORTED")
        db.error("SELECT id FROM docs; DELETE FROM docs", "SYNTAX")
        db.error("DROP TABLE docs", "UNSUPPORTED")
        db.error("SELECT absent FROM docs", "UNKNOWN_COLUMN")
        db.error("SELECT * FROM absent", "UNKNOWN_TABLE")
        db.ok("INSERT INTO docs VALUES ('a''b','[1e-80,3.4028235e38]',8,1)", 1)
        cells = db.query("SELECT label,v,L2_DISTANCE(v,'[0,0]') AS d FROM docs WHERE id=8")[0]
        assert cells[0:2] == ["a'b", "[0,3.4028235e+38]"] and float(cells[2]) > 3e38, cells
        db.ok("CREATE VECTOR INDEX docs_v ON docs(v)")
        db.ok("MODE ann")
        assert db.query(top) == [["2", "5"], ["4", "5"]]
        db.ok("BEGIN")
        db.ok("UPDATE docs SET score=9,v='[0,1]' WHERE id=4", 1)
        db.ok("DELETE FROM docs WHERE id=2", 1)
        assert db.query(top) == [["4", "1"]]
        db.error("UPDATE docs SET score=6,v='[0]' WHERE id=4", "VALUE")
        assert db.query("SELECT score FROM docs WHERE id=4") == [["9"]]
        db.ok("ROLLBACK")
        assert db.query(top) == [["2", "5"], ["4", "5"]]
        db.ok("BEGIN")
        db.ok("INSERT INTO docs VALUES ('b','[1,0]',9,6)", 1)
        db.quit()
    with Session(binary, root) as db:
        assert db.query("SELECT id FROM docs WHERE id=9") == []
        assert db.query(top) == [["2", "5"], ["4", "5"]]
        db.quit()
    print("SQL, Unicode, float32, filtered TopK, transaction visibility and normal restart: passed")


def atomicity(binary, root):
    with Session(binary, root) as db:
        db.ok("CREATE TABLE docs (id BIGINT PRIMARY KEY, body VARCHAR(4096))")
        db.ok("INSERT INTO docs VALUES (1,'original')", 1)
        db.ok("BEGIN")
        db.ok("INSERT INTO docs VALUES (2,'earlier successful statement')", 1)
        large = "x" * 1024
        rows = ",".join(f"({i},'{large}')" for i in range(3, 350))
        db.error(f"INSERT INTO docs VALUES {rows},(1,'duplicate')", "DUPLICATE_KEY")
        assert db.query("SELECT id,body FROM docs ORDER BY id") == [
            ["1", "original"], ["2", "earlier successful statement"]]
        db.ok("COMMIT")
        db.ok("BEGIN")
        db.ok("UPDATE docs SET body='uncommitted' WHERE id=1", 1)
        db.ok("DELETE FROM docs WHERE id=2", 1)
        db.ok("CREATE TABLE uncommitted (id BIGINT PRIMARY KEY)")
        db.ok(f"INSERT INTO docs VALUES {rows}", 347)
        db.kill()
    with Session(binary, root) as db:
        assert db.query("SELECT id,body FROM docs ORDER BY id") == [
            ["1", "original"], ["2", "earlier successful statement"]]
        db.error("SELECT * FROM uncommitted", "UNKNOWN_TABLE")
        db.ok("UPDATE docs SET body='acknowledged' WHERE id=2", 1)
        db.kill()
    with Session(binary, root) as db:
        assert db.query("SELECT body FROM docs WHERE id=2") == [["acknowledged"]]
        db.quit()
    print("Failed 348-row INSERT across a log flush and two SIGKILL/restart sequences: passed")


def partial_log(binary, root):
    source = root / "source"
    with Session(binary, source) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT)")
        db.ok("INSERT INTO t VALUES (1,7)", 1)
        log = source / "minivectordb.log"
        before = log.stat().st_size
        db.ok("BEGIN")
        db.ok("UPDATE t SET v=8 WHERE id=1", 1)
        db.ok("INSERT INTO t VALUES (2,3)", 1)
        db.ok("COMMIT")
        db.quit()
    after = log.stat().st_size
    for number, cut in enumerate((before + 1, before + 28, before + 40, after - 32, after - 1, after)):
        directory = root / f"prefix-{number}"
        directory.mkdir()
        copy = directory / log.name
        shutil.copyfile(log, copy)
        with copy.open("r+b") as target:
            target.truncate(cut)
        expected = [["1", "8"], ["2", "3"]] if cut == after else [["1", "7"]]
        for _ in range(2):
            with Session(binary, directory) as db:
                assert db.query("SELECT * FROM t ORDER BY id") == expected, cut
                db.quit()
    print("Six log cut points, with two successive recoveries each: passed")


def bounded_memory(binary, root):
    limit = 18 * 1024 * 1024
    size, batch = 1600, 10
    with Session(binary, root, limit) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, body VARCHAR(24000), v VECTOR(2), tag BIGINT)")
        body = "x" * 24000
        for start in range(0, size, batch):
            rows = ",".join(f"({i},'{body}','[{i},0]',{i % 2})" for i in range(start, start + batch))
            db.ok(f"INSERT INTO t VALUES {rows}", batch)
        db.quit()
    log_size = (root / "minivectordb.log").stat().st_size
    assert log_size > 2 * limit, (log_size, limit)
    with Session(binary, root, limit) as db:
        assert db.query("SELECT id FROM t WHERE tag = 1 ORDER BY L2_DISTANCE(v,'[12,0]') LIMIT 4") == [
            ["11"], ["13"], ["9"], ["15"]]
        db.quit()
    print(f"Disk-backed filtered TopK after restart: {log_size:,} bytes with {limit:,}-byte address limit")


def indexed_ties(binary, root):
    with Session(binary, root) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(1), label VARCHAR(1))")
        db.ok("INSERT INTO t VALUES (1,'[-1]','x'),(2,'[1]','x'),(3,'[0]','y'),(4,'[-4]','x')", 4)
        db.ok("CREATE VECTOR INDEX ix ON t(v)")
        query = "SELECT id FROM t WHERE label='x' ORDER BY L2_DISTANCE(v,'[0]') LIMIT 1"
        assert db.query(query) == [["1"]]
        db.ok("MODE ann")
        assert db.query(query) == [["1"]]
        db.ok("BEGIN")
        db.ok("UPDATE t SET v='[-2]' WHERE id=1", 1)
        assert db.query(query) == [["2"]]
        db.ok("DELETE FROM t WHERE id=2", 1)
        assert db.query(query) == [["1"]]
        db.ok("ROLLBACK")
        assert db.query(query) == [["1"]]
        db.quit()
    with Session(binary, root) as db:
        db.ok("MODE ann")
        assert db.query(query) == [["1"]]
        db.quit()
    print("Indexed search tie order, filtering, mutation and rollback/restart: passed")


def benchmark(binary, root):
    rng = random.Random(382901)
    dimension, size, k = 128, 10_000, 10

    def vector():
        return [rng.randint(-256, 256) / 256 for _ in range(dimension)]

    def square(data, query):
        return sum((a - b) ** 2 for a, b in zip(data, query))

    data = [vector() for _ in range(size)]
    writes = []
    for batch in range(0, size, 100):
        values = ",".join(f"({i},{i % 10},'{data[i]}')" for i in range(batch, batch + 100))
        writes.append(f"INSERT INTO t VALUES {values}")
    queries = []
    for i in range(1000):
        v = vector()
        tag = rng.randrange(10) if i % 2 else None
        where = "" if tag is None else f" WHERE tag={tag}"
        sql = f"SELECT id,L2_DISTANCE(v,'{v}') AS d FROM t{where} ORDER BY d LIMIT {k}"
        queries.append((v, tag, sql))

    with Session(binary, root) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, tag BIGINT, v VECTOR(128))")
        start = perf_counter()
        for insert in writes:
            db.ok(insert, 100)
        write_time = perf_counter() - start
        db.quit()

    with Session(binary, root) as db:
        db.query(queries[0][2])
        start = perf_counter()
        results = [db.query(sql) for _, _, sql in queries]
        query_time = perf_counter() - start
        db.quit()

    checked = [0, 0]
    for i, ((query, tag, _), result) in enumerate(zip(queries, results)):
        keys = [int(key) for key, _ in result]
        assert len(keys) == len(set(keys)) == k
        for key, (_, distance) in zip(keys, result):
            assert 0 <= key < size and (tag is None or key % 10 == tag)
            assert math.isclose(float(distance), math.sqrt(square(data[key], query)), rel_tol=1e-12)
        if i % 50 in (0, 1):
            candidates = range(size) if tag is None else range(tag, size, 10)
            expected = sorted(candidates, key=lambda key: (square(data[key], query), key))[:k]
            assert keys == expected, (i, keys, expected)
            checked[tag is not None] += 1

    log_bytes = (root / "minivectordb.log").stat().st_size
    print(f"Local benchmark: 100 committed batches {write_time:.3f}s, "
          f"1000 exact queries {query_time:.3f}s, log {log_bytes:,} bytes")
    print(f"All returned distances checked; independent exact ranking matched "
          f"{checked[0]} unfiltered and {checked[1]} filtered queries")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--benchmark", action="store_true")
    options = parser.parse_args()
    binary = options.binary.resolve(strict=True)
    with TemporaryDirectory(prefix=".verify-", dir=Path(__file__).resolve().parent) as name:
        root = Path(name)
        functional(binary, root / "sql")
        atomicity(binary, root / "atomic")
        partial_log(binary, root / "partial")
        indexed_ties(binary, root / "index")
        bounded_memory(binary, root / "large")
        if options.benchmark:
            benchmark(binary, root / "bench")


if __name__ == "__main__":
    main()
