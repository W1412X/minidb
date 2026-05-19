#!/usr/bin/env python3
"""Shared helpers for deterministic MiniDB integration tests."""

from __future__ import annotations

import argparse
import os
import random
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple


DEFAULT_SEED = 12648430


def add_seed_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("bin", nargs="?", default="./build/minidb")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--stress", action="store_true")
    parser.add_argument("--cases", type=int, default=None)


def seeded_rng(seed: int) -> random.Random:
    return random.Random(seed)


def temp_db(prefix: str) -> str:
    return tempfile.mkdtemp(prefix=prefix)


def cleanup(path: str) -> None:
    shutil.rmtree(path, ignore_errors=True)


def run_minidb(bin_path: str, db_dir: str, sql: Sequence[str] | str,
               timeout: int = 30) -> str:
    if isinstance(sql, str):
        statements = [sql]
    else:
        statements = list(sql)
    if not statements or statements[-1].strip().lower() != "exit":
        statements.append("exit")
    proc = subprocess.run(
        [bin_path, "--dir", db_dir],
        input="\n".join(statements) + "\n",
        text=True,
        capture_output=True,
        timeout=timeout,
    )
    return proc.stdout + proc.stderr


def assert_no_error(output: str, seed: int, context: str) -> None:
    bad = (
        "Error:",
        "failed to build plan",
        "failed to create executor",
        "statement timeout",
        "unexpected token",
    )
    if any(token in output for token in bad):
        raise AssertionError(f"{context} failed seed={seed}\n{output}")


def _clean_minidb_lines(output: str) -> List[str]:
    lines: List[str] = []
    for raw in output.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("Data directory:"):
            continue
        if line.startswith("[DB] destructor:"):
            continue
        if line.startswith("MiniADB"):
            continue
        if line.startswith("Type 'exit'"):
            continue
        if line.startswith("minidb>"):
            line = line[len("minidb>"):].strip()
            if not line:
                continue
        if line in {
            "Transaction started.",
            "Transaction committed.",
            "Transaction rolled back.",
        }:
            continue
        if line.startswith("Table ") or line.startswith("Index "):
            continue
        if line in {"Bye.", "Goodbye."}:
            continue
        lines.append(line)
    return lines


def select_rows_from_output(output: str) -> List[Tuple[str, ...]]:
    lines = _clean_minidb_lines(output)
    if not lines:
        return []
    if lines[0].startswith("affected_rows"):
        return []
    data = lines[1:] if "|" in lines[0] or lines[0].isidentifier() or lines[0].startswith("agg_") else lines
    rows: List[Tuple[str, ...]] = []
    for line in data:
        if line.startswith("affected_rows"):
            break
        rows.append(tuple(part.strip() for part in line.split("|")))
    return rows


def minidb_query(bin_path: str, db_dir: str, sql: str, seed: int,
                 timeout: int = 30) -> List[Tuple[str, ...]]:
    output = run_minidb(bin_path, db_dir, [sql], timeout=timeout)
    assert_no_error(output, seed, sql)
    return select_rows_from_output(output)


def sqlite_rows(conn: sqlite3.Connection, sql: str) -> List[Tuple[str, ...]]:
    cur = conn.execute(sql)
    out: List[Tuple[str, ...]] = []
    for row in cur.fetchall():
        converted = []
        for value in row:
            converted.append("NULL" if value is None else str(value))
        out.append(tuple(converted))
    return out


def assert_rows_equal(actual: List[Tuple[str, ...]], expected: List[Tuple[str, ...]],
                      seed: int, sql: str) -> None:
    if actual != expected:
        raise AssertionError(
            f"result mismatch seed={seed}\nSQL: {sql}\nMiniDB: {actual}\nReference: {expected}"
        )


@dataclass
class TcpMiniDB:
    bin_path: str
    db_dir: str
    port: int
    proc: subprocess.Popen | None = None

    def start(self) -> None:
        self.proc = subprocess.Popen(
            [self.bin_path, "--dir", self.db_dir, "--server", "--port", str(self.port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2) as sock:
                    sock.recv(4096)
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError(f"server did not start on port {self.port}")

    def stop(self, kill: bool = False) -> None:
        if not self.proc:
            return
        if kill:
            self.proc.kill()
        else:
            self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    def execute(self, statements: Iterable[str], read_timeout: float = 0.5) -> str:
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
            sock.settimeout(5)
            sock.recv(4096)
            for sql in statements:
                if not sql.rstrip().endswith(";"):
                    sql += ";"
                sock.sendall((sql + "\n").encode())
            result = b""
            sock.settimeout(read_timeout)
            while True:
                try:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    result += chunk
                except socket.timeout:
                    break
            return result.decode(errors="replace")
