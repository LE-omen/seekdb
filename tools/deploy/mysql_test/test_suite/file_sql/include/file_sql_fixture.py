#!/usr/bin/env python3
"""Fixture helper for file_sql mysqltest cases.

Copies test fixtures (basic.csv, basic.jsonl, simple.parquet, invalid.txt)
into the observer's data directory, supporting local container execution
as well as SSH/SCP fallback for remote nodes.
"""

import base64
import os
import shutil
import socket
import subprocess
import sys


def _populate_files(src_dir, data_dir):
    if os.path.exists(data_dir):
        shutil.rmtree(data_dir)
    os.makedirs(data_dir, exist_ok=True)
    for item in os.listdir(src_dir):
        src_path = os.path.join(src_dir, item)
        if not os.path.isfile(src_path):
            continue
        if item == "simple.parquet.b64":
            with open(src_path, "rb") as fin:
                decoded = base64.b64decode(fin.read())
            with open(os.path.join(data_dir, "simple.parquet"), "wb") as fout:
                fout.write(decoded)
        else:
            shutil.copy2(src_path, os.path.join(data_dir, item))


def setup_fixtures(host, data_dir):
    data_dir = os.path.abspath(data_dir)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    src_dir = os.path.abspath(os.path.join(script_dir, "..", "data"))

    # 1. Primary path: local container or same-node filesystem
    try:
        _populate_files(src_dir, data_dir)
        return 0
    except Exception:
        pass

    # 2. Remote fallback via SSH/SCP if local path couldn't be prepared
    cmd = (
        f"ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 {host} "
        f"'rm -rf \"{data_dir}\" && mkdir -p \"{data_dir}\"' && "
        f"scp -q -o StrictHostKeyChecking=no -o ConnectTimeout=5 {src_dir}/* {host}:\"{data_dir}/\" && "
        f"ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 {host} "
        f"'base64 -d \"{data_dir}/simple.parquet.b64\" > \"{data_dir}/simple.parquet\" && rm -f \"{data_dir}/simple.parquet.b64\"'"
    )
    res = subprocess.run(
        cmd,
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if res.returncode == 0:
        return 0

    # 3. Final local attempt
    try:
        _populate_files(src_dir, data_dir)
        return 0
    except Exception as err:
        sys.stderr.write(f"Fixture setup failed: {err}\n")
        return 1


def cleanup_fixtures(host, data_dir):
    data_dir = os.path.abspath(data_dir)
    try:
        if os.path.exists(data_dir):
            shutil.rmtree(data_dir)
        return 0
    except Exception:
        pass

    cmd = f"ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 {host} 'rm -rf \"{data_dir}\"'"
    subprocess.run(
        cmd,
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if os.path.exists(data_dir):
        try:
            shutil.rmtree(data_dir)
        except Exception:
            pass
    return 0


def main():
    if len(sys.argv) < 4:
        sys.stderr.write("Usage: file_sql_fixture.py <setup|cleanup> <host> <data_dir>\n")
        return 1
    action = sys.argv[1]
    host = sys.argv[2]
    data_dir = sys.argv[3]

    if action == "setup":
        return setup_fixtures(host, data_dir)
    elif action == "cleanup":
        return cleanup_fixtures(host, data_dir)
    else:
        sys.stderr.write(f"Unknown action: {action}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
