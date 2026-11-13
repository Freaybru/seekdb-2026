"""Black-box SQL, durability, bounded-memory and optional timing checks."""

import argparse
from array import array
from contextlib import ExitStack
import fcntl
import json
import math
from pathlib import Path
import random
import re
import resource
import select
import shutil
import signal
import struct
import subprocess
from tempfile import TemporaryDirectory
import termios
from time import perf_counter, sleep


class Session:
    def __init__(self, binary, directory, address_space=None, *, arguments=None, ready="READY"):
        def limit_memory():
            resource.setrlimit(resource.RLIMIT_AS, (address_space, address_space))

        self.process = subprocess.Popen(
            [str(binary), *(arguments if arguments is not None
                            else ("--data-dir", str(directory), "--input=line"))],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            preexec_fn=limit_memory if address_space else None,
        )
        try:
            assert self.read() == ready
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
        assert result.startswith(f"ERROR {code} "), (sql[:120], code, result)

    def query(self, sql, expected_columns=None):
        columns, rows = self.read_result(self.send(sql))
        if expected_columns is not None:
            assert columns == expected_columns, columns
        return rows

    def read_result(self, status):
        header = status.split()
        assert len(header) == 3 and header[0] == "RESULT", status
        width, count = map(int, header[1:])
        assert width > 0 and count >= 0, status
        columns = self.read().split("\t")
        assert len(columns) == width
        rows = [self.read().split("\t") for _ in range(count)]
        assert all(len(row) == width for row in rows), rows
        assert self.read() == "END"
        return columns, rows

    def quit(self):
        assert self.send("QUIT") == "BYE"
        assert self.process.wait(timeout=5) == 0

    def kill(self):
        self.process.kill()
        assert self.process.wait(timeout=5) == -signal.SIGKILL


def unescape_cell(text):
    escapes = {"\\": "\\", "t": "\t", "n": "\n", "r": "\r", "0": "\0"}
    return re.sub(r"\\([\\tnr0])", lambda match: escapes[match[1]], text)


def matches_row(actual, expected):
    if len(actual) != len(expected):
        return False
    for cell, value in zip(actual, expected):
        if isinstance(value, float):
            try:
                if not math.isclose(float(cell), value, abs_tol=1e-5, rel_tol=1e-5):
                    return False
            except ValueError:
                return False
        elif cell != str(value):
            return False
    return True


def check_public_rows(actual, expected, unordered):
    assert len(actual) == len(expected), (actual, expected)
    if not unordered:
        assert all(matches_row(row, wanted) for row, wanted in zip(actual, expected)), (
            actual, expected)
        return

    # Match each expected row once. Reassign a previous match if overlapping
    # distance tolerances give one row more than one possible partner.
    owners = {}

    def assign(wanted, visited):
        for index, row in enumerate(actual):
            if index in visited or not matches_row(row, expected[wanted]):
                continue
            visited.add(index)
            if index not in owners or assign(owners[index], visited):
                owners[index] = wanted
                return True
        return False

    assert all(assign(i, set()) for i in range(len(expected))), (actual, expected)


def check_public_response(db, sql, check):
    if "error" in check:
        db.error(sql, check["error"])
        return
    status = db.send(sql)
    if status.startswith("RESULT "):
        assert "affected_rows" not in check, status
        columns, raw_rows = db.read_result(status)
        if "columns" in check:
            columns = [unescape_cell(column) for column in columns]
            assert columns == check["columns"], (columns, check["columns"])
        if "rows" in check:
            rows = [[unescape_cell(cell) for cell in row] for row in raw_rows]
            check_public_rows(rows, check["rows"], check.get("unordered", False))
    else:
        assert re.fullmatch(r"OK [0-9]+", status), status
        assert "rows" not in check and "columns" not in check, status
        if "affected_rows" in check:
            assert int(status[3:]) == check["affected_rows"], (status, check)


def public_case(binary, root, suite, case):
    name = case["id"]
    sql_file = suite / case.get("test", f"cases/{name}.test")
    result_file = suite / case.get("result", f"cases/{name}.result")
    expected = json.loads(result_file.read_text(encoding="utf-8"))
    lines = [line.strip(" \t\r") for line in sql_file.read_text(encoding="utf-8").split("\n")]
    count = sum(bool(line) and not line.startswith("--") for line in lines)
    assert count == len(expected) == case["statements"], "SQL and assertion counts differ"
    checks = iter(expected)
    mode = "exact"
    with ExitStack() as active:
        db = active.enter_context(Session(binary, root))
        for line_number, line in enumerate(lines, 1):
            if not line:
                continue
            try:
                if line == "--restart":
                    db.quit()
                    active.close()
                    db = active.enter_context(Session(binary, root))
                    if mode != "exact":
                        db.ok(f"MODE {mode}")
                elif line in ("--mode ann", "--mode exact"):
                    mode = line.split()[1]
                    db.ok(f"MODE {mode}")
                else:
                    assert not line.startswith("--"), "unknown driver directive"
                    check_public_response(db, line, next(checks))
            except AssertionError as error:
                raise AssertionError(f"line {line_number}: {line}: {error}") from error
        db.quit()
    return count


def public_cases(binary, root, suite):
    manifest = json.loads((suite / "manifest.json").read_text(encoding="utf-8"))
    total = 0
    failures = []
    for case in manifest["cases"]:
        name = case["id"]
        assert re.fullmatch(r"[A-Za-z0-9_-]+", name), name
        try:
            count = public_case(binary, root / name, suite, case)
        except AssertionError as error:
            failures.append(name)
            print(f"Public sample {name}: FAILED ({error})")
        else:
            total += count
            print(f"Public sample {name}: {count}/{count}")
    assert not failures, f"{len(failures)} public samples failed: {', '.join(failures)}"
    print(f"Official public samples: {len(manifest['cases'])} cases, {total} statements passed")


def functional(binary, root):
    with Session(binary, root) as db:
        db.ok("CREATE TABLE Docs (label VARCHAR(3), v VECTOR(2), id BIGINT, "
              "score BIGINT, PRIMARY KEY (id));")
        db.ok("INSERT INTO docs (id, score, label, v) VALUES "
              "(4,5,'猫🦊é','[3,4]'),(2,5,'猫🦊é','[0,5]'),(5,3,'z','[0,0]'),"
              "(-9223372036854775808,2,'z','[1,1]'),(9223372036854775807,4,'a','[3,0]')", 5)
        log = root / "minivectordb.log"
        before = log.stat().st_size
        db.ok("UPDATE docs SET v='[3.0,4e0]',score=5 WHERE id=4", 1)
        assert log.stat().st_size == before
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
        for value in ("'[]'", "'[1]'", "'[1,0,2]'"):
            db.error(f"INSERT INTO docs VALUES ('a',{value},8,1)", "dimension_mismatch")
        for value in ("'[1e39,0]'", "'[NaN,0]'", "'[Infinity,0]'", "'[1,0,]'", "'[1,0]x'"):
            db.error(f"INSERT INTO docs VALUES ('a',{value},8,1)", "VALUE")
        db.error("INSERT INTO docs VALUES ('四字符数','[1,0]',8,1)", "VALUE")
        db.error("INSERT INTO docs VALUES ('a','[1,0]',9223372036854775808,1)", "VALUE")
        db.error("UPDATE docs SET id=0 WHERE id=4", "unsupported_sql")
        db.error("SELECT id FROM docs; DELETE FROM docs", "SYNTAX")
        db.error("DROP TABLE docs", "unsupported_sql")
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
        db.error("UPDATE docs SET score=6,v='[0]' WHERE id=4", "dimension_mismatch")
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


def json_wire(binary, root):
    arguments = ("--data-dir", str(root))
    ready = '{"type":"ready"}'

    def request(db, ident, sql=None, *, op="execute", mode=None):
        message = {"id": ident, "op": op}
        if sql is not None:
            message["sql"] = sql
        if mode is not None:
            message["search_mode"] = mode
        response = json.loads(db.send(json.dumps(message, allow_nan=False)))
        assert type(response["id"]) is type(ident) and response["id"] == ident, response
        assert type(response["ok"]) is bool, response
        return response

    def shutdown(db, ident):
        assert request(db, ident, op="shutdown") == {"id": ident, "ok": True}
        assert db.process.wait(timeout=5) == 0
        assert db.process.stdout.read() == ""

    text = '甲🦊; "quote" \'single\' \\tab\tline\nreturn\rNUL\0'
    literal = "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"
    largest = 2**63 - 1
    with Session(binary, root, arguments=arguments, ready=ready) as db:
        assert request(db, 2**100, "CREATE TABLE docs (\n  id BIGINT PRIMARY KEY,\n"
                       "  body VARCHAR(64),\n  v VECTOR(2)\n);")["ok"]
        write = request(db, "写入🦊", f"INSERT INTO docs VALUES ({largest},{literal},'[1.25,0]')")
        assert write == {"id": "写入🦊", "ok": True, "affected_rows": 1}
        assert type(write["affected_rows"]) is int
        ident = {"name": "\t#行", "values": [1, False, None, {"value": 2**103}]}
        found = request(db, ident, "SELECT id,body,v FROM docs ORDER BY id;", mode="exact")
        assert found == {"id": ident, "ok": True, "columns": ["id", "body", "v"],
                         "rows": [[largest, text, [1.25, 0]]]}
        assert type(found["rows"][0][0]) is int
        assert request(db, False, "SELECT id FROM docs WHERE id<0", mode="ann")["rows"] == []
        assert request(db, None, "INSERT INTO docs VALUES "
                       f"({largest},'duplicate','[1,0]')")["error"]["code"] == "duplicate_key"
        for malformed in ('{"id":1,"id":2,"op":"shutdown"}',
                          '{"id":NaN,"op":"shutdown"}',
                          r'{"id":"\ud800","op":"shutdown"}',
                          '{"id":2,"op":"execute","sql":"' + 'a' * 42 + '\x1f"}',
                          '{"id":2,"op":"execute","sql":"' + 'a' * 42 + '\\q"}'):
            response = json.loads(db.send(malformed))
            assert response["ok"] is False and response["error"]["code"] == "invalid_request"
        assert request(db, 1.25, op="other")["ok"] is False
        assert request(db, 1, "BEGIN")["ok"]
        assert request(db, 2, "DELETE FROM docs")["affected_rows"] == 1
        shutdown(db, "结束")
    with Session(binary, root, arguments=arguments, ready=ready) as db:
        assert request(db, 0, "SELECT body FROM docs")["rows"] == [[text]]
        shutdown(db, ["done", 7])
    print("Default JSONL: typed IDs and values, multiline SQL, Unicode, errors and shutdown passed")


def vector_syntax(binary, root):
    with Session(binary, root) as db:
        db.ok("CREATE TABLE `Vec``Test` (id BIGINT PRIMARY KEY, v VECTOR(3), txt VARCHAR(96))")
        db.ok("INSERT INTO `Vec``Test` VALUES (1,'[+.5,1.e+0,-1e-500]','a''b\\t甲')", 1)
        assert db.query("SELECT txt FROM `vec``test`") == [["a'b\\t甲"]]
        distance = db.query("SELECT L2_DISTANCE(v,'[.5,1,0]') FROM `vec``test`")
        assert float(distance[0][0]) == 0
        for value in ("[1e+,2,3]", "[+-1,2,3]", "[1e999,2,3]", "[0x1p0,2,3]",
                      "[NaN,2,3]", "[1,2,3,]", "[1,2,3] trailing"):
            db.error(f"UPDATE `vec``test` SET v='{value}'", "VALUE")
        db.error("UPDATE `vec``test` SET v='[1,2]'", "dimension_mismatch")
        assert float(db.query("SELECT L2_DISTANCE(v,'[.5,1,0]') FROM `vec``test`")[0][0]) == 0
        db.quit()
    print("Quoted SQL, decimal and exponent syntax, and invalid float32 writes: passed")


def packed_storage(binary, root):
    # 129 coordinates exercise the partial SIMD block. Include sign bits,
    # exponent escapes, and values in the same row that must remain raw.
    size = 129
    short = [0.0, -0.0, 1.5, -0.25, 5.0] * 25 + [0.0, -0.0, 0.25, 1.0]
    clustered = list(array('f', (0.91 + 0.00037 * i for i in range(size))))
    clustered[0], clustered[1], clustered[-1] = 2.0**-149, -0.0, -2.0**-90
    raw = list(array('f', ((-1)**i * (1.0001 + i / 10000) * 2**(-100 if i % 2 else 100)
                            for i in range(size))))
    values = {1: short, 2: clustered, 3: raw}
    labels = {1: '甲', 2: '乙', 3: '丙'}

    def binary32(value):
        return array('f', value).tobytes()

    def check_rows(db):
        result = db.query('SELECT id,v,body,tag FROM t ORDER BY body, id')
        assert [int(row[0]) for row in result] == sorted(values, key=lambda key: labels[key])
        for key, vector, body, tag in result:
            key = int(key)
            assert binary32(json.loads(vector, parse_int=float)) == binary32(values[key]), key
            assert body == labels[key] and int(tag) == key % 2

    for legacy in (True, False):
        folder = root / ('legacy' if legacy else 'current')
        folder.mkdir(parents=True)
        log = folder / 'minivectordb.log'
        if legacy:
            log.write_bytes(b'MINIVDB1')
        with Session(binary, folder) as db:
            db.ok(f'CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR({size}), body VARCHAR(8), tag BIGINT)')
            db.ok('INSERT INTO t VALUES '+','.join(f"({key},'{json.dumps(v)}','{labels[key]}',{key%2})"
                                                  for key,v in values.items()),3)
            check_rows(db)
            before = log.stat().st_size
            db.ok('UPDATE t SET body=\'乙\' WHERE id=2',1)
            assert before == log.stat().st_size
            db.ok('CREATE VECTOR INDEX ix ON t(v)')
            query = f"SELECT id FROM t WHERE tag=0 ORDER BY L2_DISTANCE(v,'{json.dumps(clustered)}') LIMIT 1"
            assert db.query(query)==[['2']]
            db.ok('MODE ann')
            assert db.query(query)==[['2']]
            db.ok('BEGIN')
            db.ok(f"UPDATE t SET v='{json.dumps(raw)}' WHERE id=1",1)
            db.ok('DELETE FROM t WHERE id=2',1)
            db.ok('ROLLBACK')
            check_rows(db)
            db.ok('BEGIN')
            db.ok(f"INSERT INTO t VALUES (4,'{json.dumps(clustered)}','未提交',0)",1)
            db.kill()
        with Session(binary, folder) as db:
            check_rows(db)
            db.ok('MODE ann')
            assert db.query(query)==[['2']]
            db.quit()
        data = log.read_bytes()
        assert data[:8] == (b'MINIVDB1' if legacy else b'MINIVDB2')
        records = {}
        cursor = 8
        while cursor < len(data):
            length, op, _, key, _ = struct.unpack_from('<IIIqQ',data,cursor)
            if op in (3,6):
                records[key] = (op, length, data[cursor + 28:cursor + 30])
            cursor += length + 32
        assert cursor == len(data)
        assert set(records) == {1,2,3}
        if legacy:
            assert all(op==3 for op,_,_ in records.values())
        else:
            assert records[1][0]==6 and int.from_bytes(records[1][2],'little') & 0x8000
            assert records[2][0]==6 and not int.from_bytes(records[2][2],'little') & 0x8000
            assert records[3][0]==3
    print('Lossless float32 storage: two encodings, raw and v1 fallback, indexed filters, rollback and crash recovery passed')


def script_input(binary, root):
    arguments = (f"--data-dir={root}", "--mode=ann", "--input=sql")
    with Session(binary, root, arguments=arguments) as db:
        # Keep the input open and send no line ending after the semicolon.
        db.process.stdin.write(
            "\ufeff-- initial comment with ;\r\nCREATE TABLE docs (\r\n"
            "  id BIGINT PRIMARY KEY,\r\n  text VARCHAR(30),\r\n  v VECTOR(2)\r\n);")
        db.process.stdin.flush()
        assert select.select([db.process.stdout], [], [], 5)[0], "missing response to semicolon"
        assert db.read() == "OK 0"
        db.process.stdin.write(
            "INSERT INTO docs VALUES\n"
            " (1,'a;it''s # text',[3,4]), /* ignore ; */ (2,'甲;乙',[0,0]);"
            "SELECT id,text FROM docs\n ORDER BY id DESC;")
        db.process.stdin.flush()
        assert db.read() == "OK 2"
        assert db.read_result(db.read()) == (
            ["id", "text"], [["2", "甲;乙"], ["1", "a;it's # text"]])
        db.ok("mode\texact;")
        db.error("INSERT INTO docs VALUES (3,'bad',[0;0]);", "VALUE")
        db.ok("BEGIN;")
        db.ok("DELETE FROM docs WHERE id=2;", 1)
        db.process.stdin.write("DELETE FROM docs WHERE id=1")
        db.process.stdin.flush()
        db.process.terminate()
        assert db.process.wait(timeout=5) == 0
    with Session(binary, root) as db:
        assert db.query("\ufeffSELECT id FROM docs ORDER BY id") == [["1"], ["2"]]
        db.quit()
    with Session(binary, root, arguments=arguments) as db:
        db.process.stdin.write("INSERT INTO docs VALUES (4,'unfinished")
        db.process.stdin.close()
        assert db.read().startswith("ERROR SYNTAX ")
        assert db.process.wait(timeout=5) == 0
    print("Script delimiters, quoting, comments, equals-form options, and line-mode restart: passed")


def paged_mutations(binary, root):
    keys = [-(2**63), 2**63 - 1] + [key * 4099 for key in range(-2600, 2600)]
    random.Random(1271).shuffle(keys)
    expected = {key: key % 13 for key in keys}

    def check(db, state):
        assert db.query("SELECT id,tag FROM t ORDER BY id DESC") == [
            [str(key), str(state[key])] for key in sorted(state, reverse=True)]

    with Session(binary, root) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, tag BIGINT)")
        for start in range(0, len(keys), 311):
            chunk = keys[start:start+311]
            rows = ",".join(f"({key},{expected[key]})" for key in chunk)
            db.ok("INSERT INTO t VALUES " + rows, len(chunk))
        check(db, expected)
        changed = [key for key, tag in expected.items() if -100_000 <= key <= 200_000 and tag < 8]
        db.ok("UPDATE t SET tag=99 WHERE id>=-100000 AND tag<8 AND id<=200000", len(changed))
        expected.update((key, 99) for key in changed)
        erased = [key for key, tag in expected.items() if key < -200_000 and tag <= 9]
        db.ok("DELETE FROM t WHERE id < -200000 AND tag <= 9", len(erased))
        for key in erased:
            del expected[key]
        check(db, expected)
        db.ok("BEGIN")
        db.ok("DELETE FROM t", len(expected))
        assert db.query("SELECT * FROM t ORDER BY id DESC") == []
        db.ok("INSERT INTO t VALUES (-9223372036854775808,1),(9223372036854775807,2)", 2)
        rows = ",".join(f"({key},0)" for key in range(1, 600))
        db.error(f"INSERT INTO t VALUES {rows},(9223372036854775807,3)", "duplicate_key")
        check(db, {-(2**63): 1, 2**63 - 1: 2})
        db.ok("ROLLBACK")
        check(db, expected)
        db.kill()
    with Session(binary, root) as db:
        check(db, expected)
        db.ok("DELETE FROM t", len(expected))
        db.ok("INSERT INTO t VALUES (-9223372036854775808,7),(9223372036854775807,8)", 2)
        db.quit()
    with Session(binary, root) as db:
        check(db, {-(2**63): 7, 2**63 - 1: 8})
        db.quit()
    print("5,202 shuffled keys: page splits, filtered range edits, merges, and restart passed")


def atomicity(binary, root):
    with Session(binary, root) as db:
        db.ok("CREATE TABLE docs (id BIGINT PRIMARY KEY, body VARCHAR(4096))")
        db.ok("INSERT INTO docs VALUES (1,'original')", 1)
        db.ok("BEGIN")
        db.ok("INSERT INTO docs VALUES (2,'earlier successful statement')", 1)
        large = "x" * 1024
        rows = ",".join(f"({i},'{large}')" for i in range(3, 350))
        db.error(f"INSERT INTO docs VALUES {rows},(1,'duplicate')", "duplicate_key")
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
        db.ok("BEGIN")
        db.ok("DELETE FROM docs WHERE id=2", 1)
        db.process.terminate()
        assert db.process.wait(timeout=5) == 0
    with Session(binary, root) as db:
        assert db.query("SELECT body FROM docs WHERE id=2") == [["acknowledged"]]
        db.quit()
    print("Failed 348-row INSERT, two SIGKILL/restarts and graceful SIGTERM rollback: passed")


def shutdown_backpressure(binary, root):
    body = "x" * (256 * 1024)
    with Session(binary, root) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, body VARCHAR(262144))")
        db.ok(f"INSERT INTO t VALUES (1,'{body}'),(2,'committed')", 2)
        db.quit()
    log = root / "minivectordb.log"
    committed_size = log.stat().st_size
    for disconnect in (False, True):
        with Session(binary, root) as db:
            db.ok("BEGIN")
            db.ok(f"UPDATE t SET body='{body}' WHERE id=2", 1)
            assert log.stat().st_size > committed_size
            pipe = db.process.stdout.fileno()
            if disconnect:
                db.process.stdout.close()
            else:
                capacity = fcntl.fcntl(pipe, fcntl.F_SETPIPE_SZ, 4096)
            db.process.stdin.write("SELECT body FROM t WHERE id=1\n")
            db.process.stdin.flush()
            if not disconnect:
                queued = array("i", [0])
                deadline = perf_counter() + 5
                while queued[0] < capacity and perf_counter() < deadline:
                    fcntl.ioctl(pipe, termios.FIONREAD, queued, True)
                    sleep(0.01)
                assert queued[0] == capacity, (queued[0], capacity)
                db.process.terminate()
            assert db.process.wait(timeout=5) == 0
        assert log.stat().st_size == committed_size
    with Session(binary, root) as db:
        assert db.query("SELECT body FROM t WHERE id=2") == [["committed"]]
        db.quit()
    print("Full stdout pipe and disconnected reader both shut down and rolled back")


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


def process_read_chars(pid):
    counters = Path(f"/proc/{pid}/io").read_text(encoding="ascii").splitlines()
    return int(next(line.split()[1] for line in counters if line.startswith("rchar:")))


def bounded_memory(binary, root):
    limit = 18 * 1024 * 1024
    size, batch = 1600, 10
    prefix = "x" * 23999
    suffixes = ("甲", "A", "乙")
    keys = list(range(size))
    random.Random(48012).shuffle(keys)
    with Session(binary, root, limit) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, body VARCHAR(24000), v VECTOR(2), tag BIGINT)")
        for start in range(0, size, batch):
            rows = ",".join(f"({i},'{prefix}{suffixes[i % 3]}','[{i},0]',{i % 2})"
                            for i in keys[start:start + batch])
            db.ok(f"INSERT INTO t VALUES {rows}", batch)
        db.quit()
    log_size = (root / "minivectordb.log").stat().st_size
    assert log_size > 2 * limit, (log_size, limit)
    with Session(binary, root, limit) as db:
        before = process_read_chars(db.process.pid)
        assert db.query("SELECT id,L2_DISTANCE(v,'[12,0]') AS d FROM t ORDER BY d LIMIT 4") == [
            ["12", "0"], ["11", "1"], ["13", "1"], ["10", "2"]]
        read_chars = process_read_chars(db.process.pid) - before
        assert read_chars < log_size // 2, (read_chars, log_size)
        before = process_read_chars(db.process.pid)
        assert db.query("SELECT id FROM t WHERE tag = 1 ORDER BY L2_DISTANCE(v,'[12,0]') LIMIT 4") == [
            ["11"], ["13"], ["9"], ["15"]]
        filtered_chars = process_read_chars(db.process.pid) - before
        assert filtered_chars < 3 * log_size // 4, (filtered_chars, log_size)
        assert db.query("SELECT id FROM t WHERE body > 'x' AND tag = 1 AND body < 'y' "
                        "AND id >= 1590 ORDER BY id") == [
                            [str(key)] for key in range(1591, 1600, 2)]
        expected = sorted(range(size), key=lambda key: (suffixes[key % 3], key))
        assert db.query("SELECT id FROM t ORDER BY body") == [[str(key)] for key in expected]
        assert db.query("SELECT id FROM t ORDER BY body DESC,tag ASC LIMIT 5") == [
            ["0"], ["6"], ["12"], ["18"], ["24"]]
        query = "SELECT id FROM t WHERE id < 12 ORDER BY body LIMIT 2"
        db.ok("BEGIN")
        db.ok(f"UPDATE t SET body='{prefix}' WHERE id=0", 1)
        assert db.query(query) == [["0"], ["1"]]
        db.ok("ROLLBACK")
        assert db.query(query) == [["1"], ["4"]]
        db.quit()
    print(f"Disk-backed filtered TopK and VARCHAR ordering: "
          f"{log_size:,} bytes with {limit:,}-byte address limit")
    print(f"Vector-only TopK on shuffled data: {read_chars:,} bytes read by the process")
    print(f"Filtered TopK on shuffled data: {filtered_chars:,} bytes read by the process")


def indexed_memory(binary, root):
    limit = 14 * 1024 * 1024
    with Session(binary, root) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2), tag BIGINT)")
        for start in range(0, 100_000, 1000):
            rows = ",".join(f"({key},'[{key % 193},0]',{key % 2})"
                            for key in range(start, start + 1000))
            db.ok(f"INSERT INTO t VALUES {rows}", 1000)
        db.ok("CREATE VECTOR INDEX ix ON t(v)")
        db.quit()
    with Session(binary, root, limit) as db:
        sql = "SELECT id FROM t WHERE id < 10 AND tag = 1 ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3"
        assert db.query(sql) == [["1"], ["3"], ["5"]]
        db.ok("MODE ann")
        assert db.query(sql) == [["1"], ["3"], ["5"]]
        db.quit()
    print(f"100,000 indexed rows recovered and queried with {limit:,}-byte address limit")


def dense_recovery(binary, root):
    size = 600_000
    limit = 26 * 1024**2
    keys = list(range(-size // 2, size // 2))
    random.Random(317).shuffle(keys)
    with Session(binary, root) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, tag BIGINT, v VECTOR(2))")
        for start in range(0, size, 1000):
            rows = ",".join(f"({key},{key % 7},'[{key % 23},0]')" for key in keys[start:start+1000])
            db.ok("INSERT INTO t VALUES " + rows, 1000)
        db.ok("CREATE VECTOR INDEX ix ON t(v)")
        db.quit()
    log_size = (root / "minivectordb.log").stat().st_size
    assert log_size > limit
    with Session(binary, root, limit) as db:
        assert db.query("SELECT id FROM t WHERE id >= 299996 ORDER BY id DESC") == [
            [str(key)] for key in range(299999, 299995, -1)]
        assert db.query("SELECT tag FROM t WHERE id=-300000") == [["6"]]
        sql = ("SELECT id FROM t WHERE id>=0 AND id<10 AND tag=0 "
               "ORDER BY L2_DISTANCE(v,'[6,0]') LIMIT 9")
        assert db.query(sql) == [["7"], ["0"]]
        db.ok("MODE ann")
        assert db.query(sql) == [["7"], ["0"]]
        db.quit()
    print(f"{size:,} narrow rows recovered from {log_size:,} bytes with a {limit:,}-byte address limit")


def discarded_tail(binary, root):
    limit = 9 * 1024**2
    log = root / "minivectordb.log"
    with Session(binary, root) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, tag BIGINT)")
        db.ok("INSERT INTO t VALUES (1,7),(2,8)", 2)
        committed = log.stat().st_size
        db.ok("BEGIN")
        db.ok("UPDATE t SET tag=99 WHERE id=1", 1)
        db.ok("DELETE FROM t WHERE id=2", 1)
        db.ok("CREATE TABLE staged (id BIGINT PRIMARY KEY, tag BIGINT)")
        for start in range(0, 400_000, 2000):
            rows = ",".join(f"({key},-1)" for key in range(start, start+2000))
            db.ok("INSERT INTO staged VALUES " + rows, 2000)
        db.kill()
    tail = log.stat().st_size - committed
    assert tail > limit
    for _ in range(2):
        with Session(binary, root, limit) as db:
            assert db.query("SELECT id,tag FROM t ORDER BY id") == [["1", "7"], ["2", "8"]]
            db.error("SELECT id FROM staged", "UNKNOWN_TABLE")
            assert log.stat().st_size == committed
            db.quit()
    print(f"Discarded {tail:,} uncommitted bytes twice checked with a {limit:,}-byte address limit")


def indexed_ties(binary, root):
    wide_order = "SELECT id FROM precise ORDER BY L2_DISTANCE(v,'[0,0,0,0]')"
    wide_top = wide_order + " LIMIT 3"
    wide_expected = [["3"], ["1"], ["2"]]
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
        db.ok("CREATE TABLE precise (id BIGINT PRIMARY KEY, v VECTOR(4))")
        db.ok("INSERT INTO precise VALUES (1,'[1,1,1,134217728]'),"
              "(2,'[134217728,1,1,1]'),(3,'[134217728,0,0,0]')", 3)
        db.ok("CREATE VECTOR INDEX precise_ix ON precise(v)")
        for mode in ("exact", "ann"):
            db.ok(f"MODE {mode}")
            rows = db.query(wide_top)
            assert rows == wide_expected, (mode, rows, wide_expected)
            assert db.query(wide_order + " LIMIT 1") == wide_expected[:1]
        assert db.query(wide_order + ", id") == wide_expected
        db.quit()
    with Session(binary, root) as db:
        db.ok("MODE ann")
        assert db.query(query) == [["1"]]
        assert db.query(wide_top) == wide_expected
        db.quit()
    print("Indexed search ties, extreme float32 ranking, filtering and rollback/restart: passed")


def conservative_bounds(binary, root):
    rng = random.Random(625139)

    def integer(value):
        numerator, denominator = value.as_integer_ratio()
        return (numerator << 149) // denominator

    checks = []
    with Session(binary, root) as db:
        for dimension in (31, 32, 33, 63, 64, 65, 127, 128, 129, 256, 4096, 4097):
            name = f"bound_{dimension}"
            size = 48 if dimension > 256 else 96
            scales = (2.0**-149, 1e-20, 1.0, 1e18, 1e37)
            vectors = [list(array("f", (rng.randint(-8, 8) * scales[key % 5]
                                      for _ in range(dimension)))) for key in range(size)]
            # Equal huge components hide the small differences from double
            # sums; exact integer squares must still determine the order.
            for key in range(8):
                vectors[key] = [0.0] * dimension
                vectors[key][0] = array("f", [1e30])[0]
                vectors[key][-1] = float(7 - key)
            vectors[9] = vectors[8].copy()
            units = [[integer(value) for value in vector] for vector in vectors]
            db.ok(f"CREATE TABLE {name} (id BIGINT PRIMARY KEY, tag BIGINT, v VECTOR({dimension}))")
            keys = list(range(size))
            rng.shuffle(keys)
            for start in range(0, size, 16):
                batch = keys[start:start+16]
                values = ",".join(f"({key},{key % 3},'{json.dumps(vectors[key])}')" for key in batch)
                db.ok(f"INSERT INTO {name} VALUES " + values, len(batch))
            db.ok(f"CREATE VECTOR INDEX ix_{dimension} ON {name}(v)")
            queries = [[0.0] * dimension, vectors[8], vectors[23]]
            queries.extend(list(array("f", (rng.randint(-8, 8) * scale
                                             for _ in range(dimension)))) for scale in scales)
            for number, query in enumerate(queries):
                query_units = list(map(integer, query))
                squares = [sum((a-b)**2 for a, b in zip(vector, query_units)) for vector in units]
                tag = number % 3 if number % 2 else None
                eligible = [key for key in range(size) if tag is None or key % 3 == tag]
                order = sorted(eligible, key=lambda key: (squares[key], key))
                where = f" WHERE tag={tag}" if tag is not None else ""
                sql = (f"SELECT id FROM {name}{where} ORDER BY "
                       f"L2_DISTANCE(v,'{json.dumps(query)}') LIMIT 7")
                checks.append((sql, [[str(key)] for key in order[:7]]))
            tied = (f"SELECT id FROM {name} WHERE id<8 ORDER BY "
                    f"L2_DISTANCE(v,'{json.dumps([0.0] * dimension)}') LIMIT 4")
            checks.append((tied, [[str(key)] for key in (7, 6, 5, 4)]))
        for mode in ("exact", "ann"):
            db.ok(f"MODE {mode}")
            for sql, expected in checks:
                assert db.query(sql) == expected, (mode, sql[:100], expected)
        db.quit()
    with Session(binary, root) as db:
        for mode in ("exact", "ann"):
            db.ok(f"MODE {mode}")
            for sql, expected in checks:
                assert db.query(sql) == expected, ("restart", mode, sql[:100], expected)
        db.quit()
    print("432 high-dimensional exact-integer TopK comparisons: ties, underflow, overflow and restart passed")


def packed_bounds(binary, root):
    rng = random.Random(218443)

    def units(vector):
        return [(a << 149) // b for a, b in (value.as_integer_ratio() for value in vector)]

    # Both the fixed 128-byte scan and the padded, variable-stride scan.
    for dimension, scale in ((33, 2.0**-143), (128, 1.0), (129, 1e35)):
        folder = root / str(dimension)
        values = {key: list(array("f", (rng.uniform(-scale, scale) for _ in range(dimension))))
                  for key in range(-320, 323)}
        values[-305] = values[-304].copy()
        pairs = {key: units(vector) for key, vector in values.items()}
        queries = [values[-304], values[10], [0.0] * dimension,
                   [array("f", [scale * 3])[0]] * dimension]

        def check(db, state, retagged=False):
            for i, query in enumerate(queries):
                q = units(query)
                if i == 1:
                    where = " WHERE id>=-310 AND id<=100 AND tag='组0'"
                    eligible = [key for key in state if -310 <= key <= 100 and
                                (key % 4 == 0 or retagged and key == 10)]
                elif i == 2:
                    where = " WHERE id>=-310 AND id<=100"
                    eligible = [key for key in state if -310 <= key <= 100]
                else:
                    where = ""
                    eligible = state
                top = sorted(eligible, key=lambda key:
                             (sum((a-b)**2 for a, b in zip(state[key], q)), key))[:10]
                sql = f"SELECT id FROM t{where} ORDER BY L2_DISTANCE(v,'{json.dumps(query)}') LIMIT 10"
                assert db.query(sql) == [[str(key)] for key in top], (dimension, i, top)
            assert db.query(f"SELECT id FROM t WHERE id=99 AND id=100 ORDER BY "
                            f"L2_DISTANCE(v,'{json.dumps(queries[0])}') LIMIT 5") == []
            assert db.query(f"SELECT id FROM t WHERE tag='不存在' ORDER BY "
                            f"L2_DISTANCE(v,'{json.dumps(queries[0])}') LIMIT 5") == []
            assert db.query(f"SELECT id FROM t WHERE id>10 AND id<12 AND tag='组0' ORDER BY "
                            f"L2_DISTANCE(v,'{json.dumps(queries[0])}') LIMIT 5") == []
            q = units(queries[1])
            expected = min((key for key in state if 10 <= key <= 12),
                           key=lambda key: (sum((a-b)**2 for a, b in zip(state[key], q)), key))
            assert db.query(f"SELECT id FROM t WHERE marker>=11 AND marker<=13 ORDER BY "
                            f"L2_DISTANCE(v,'{json.dumps(queries[1])}') LIMIT 1") == [[str(expected)]]

        with Session(binary, folder) as db:
            db.ok(f"CREATE TABLE t (id BIGINT PRIMARY KEY, tag VARCHAR(8), marker BIGINT, v VECTOR({dimension}))")
            keys = list(values)
            rng.shuffle(keys)
            for start in range(0, len(keys), 32):
                batch = keys[start:start+32]
                tuples = ",".join(f"({key},'组{key % 4}',{key + 1},'{json.dumps(values[key])}')" for key in batch)
                db.ok("INSERT INTO t VALUES " + tuples, len(batch))
            db.ok("CREATE VECTOR INDEX ix ON t(v)")
            check(db, pairs)
            check(db, pairs)
            zero = json.dumps([0.0] * dimension)
            db.error(f"INSERT INTO t VALUES (640,'组0',641,'{zero}'),(1,'组1',2,'{zero}')", "duplicate_key")
            check(db, pairs)
            db.ok("BEGIN")
            db.ok(f"UPDATE t SET v='{zero}' WHERE id=10", 1)
            db.ok("DELETE FROM t WHERE id=-304", 1)
            edited = dict(pairs)
            edited[10] = [0] * dimension
            del edited[-304]
            check(db, edited)
            db.ok("ROLLBACK")
            check(db, pairs)
            db.ok("UPDATE t SET tag='组0' WHERE id=10", 1)
            check(db, pairs, retagged=True)
            db.ok(f"UPDATE t SET v='{zero}' WHERE id=10", 1)
            pairs[10] = [0] * dimension
            check(db, pairs, retagged=True)
            db.ok(f"INSERT INTO t VALUES (640,'组0',641,'{zero}')", 1)
            pairs[640] = [0] * dimension
            check(db, pairs, retagged=True)
            db.quit()
        with Session(binary, folder) as db:
            # An equality on a high-cardinality column must fall back safely.
            assert db.query(f"SELECT id FROM t WHERE marker=11 ORDER BY "
                            f"L2_DISTANCE(v,'{json.dumps(queries[0])}') LIMIT 5") == [["10"]]
            check(db, pairs, retagged=True)
            db.quit()
    print("Packed TopK: exact integer ranks, padding, filter buckets, out-of-range queries and mutation/restart passed")


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
    parser.add_argument("--public-suite", type=Path, help="path to the official mysqltest directory")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--stress", action="store_true", help="also run the large recovery checks")
    options = parser.parse_args()
    binary = options.binary.resolve(strict=True)
    with TemporaryDirectory(prefix=".verify-", dir=Path(__file__).resolve().parent) as name:
        root = Path(name)
        if options.public_suite is not None:
            public_cases(binary, root / "public", options.public_suite)
        json_wire(binary, root / "json")
        vector_syntax(binary, root / "vector-syntax")
        functional(binary, root / "sql")
        packed_storage(binary, root / "packed-storage")
        script_input(binary, root / "framing")
        paged_mutations(binary, root / "directory")
        atomicity(binary, root / "atomic")
        shutdown_backpressure(binary, root / "backpressure")
        partial_log(binary, root / "partial")
        indexed_ties(binary, root / "index")
        conservative_bounds(binary, root / "bounds")
        packed_bounds(binary, root / "packed-bounds")
        bounded_memory(binary, root / "large")
        indexed_memory(binary, root / "index-memory")
        if options.stress:
            dense_recovery(binary, root / "dense")
            discarded_tail(binary, root / "tail")
        if options.benchmark:
            benchmark(binary, root / "bench")


if __name__ == "__main__":
    main()
