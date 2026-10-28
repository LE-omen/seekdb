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


def get_local_ips():
    ips = {"127.0.0.1", "localhost", "::1", "0.0.0.0", ""}
    try:
        hostname = socket.gethostname()
        ips.add(hostname)
        for info in socket.getaddrinfo(hostname, None):
            ips.add(info[4][0])
    except Exception:
        pass
    try:
        _, _, addrs = socket.gethostbyname_ex(socket.gethostname())
        ips.update(addrs)
    except Exception:
        pass
    return ips


def is_local(host, data_dir):
    if host in get_local_ips():
        return True
    parent = os.path.dirname(os.path.abspath(data_dir))
    if os.path.exists(parent):
        return True
    return False


def setup_fixtures(host, data_dir):
    data_dir = os.path.abspath(data_dir)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    src_dir = os.path.abspath(os.path.join(script_dir, "..", "data"))

    # Try local setup first if host is local or parent dir exists
    if is_local(host, data_dir):
        try:
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
            return 0
        except Exception as err:
            if host in get_local_ips():
                sys.stderr.write(f"Local fixture setup error: {err}\n")
                return 1

    # Remote fallback via SSH/SCP
    cmd = (
        f"ssh {host} 'rm -rf \"{data_dir}\" && mkdir -p \"{data_dir}\"' && "
        f"scp -q {src_dir}/* {host}:\"{data_dir}/\" && "
        f"ssh {host} 'base64 -d \"{data_dir}/simple.parquet.b64\" > \"{data_dir}/simple.parquet\" && rm -f \"{data_dir}/simple.parquet.b64\"'"
    )
    res = subprocess.run(cmd, shell=True)
    if res.returncode != 0:
        # Fallback to local if SSH failed (e.g. container without sshd)
        try:
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
            return 0
        except Exception as fallback_err:
            sys.stderr.write(f"SSH and local fallback both failed: {fallback_err}\n")
            return res.returncode
    return 0


def cleanup_fixtures(host, data_dir):
    data_dir = os.path.abspath(data_dir)
    if is_local(host, data_dir) or os.path.exists(data_dir):
        try:
            if os.path.exists(data_dir):
                shutil.rmtree(data_dir)
            return 0
        except Exception as err:
            if host in get_local_ips():
                sys.stderr.write(f"Local cleanup error: {err}\n")
                return 1

    cmd = f"ssh {host} 'rm -rf \"{data_dir}\"'"
    res = subprocess.run(cmd, shell=True)
    if res.returncode != 0 and os.path.exists(data_dir):
        try:
            shutil.rmtree(data_dir)
            return 0
        except Exception:
            pass
    return res.returncode


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
