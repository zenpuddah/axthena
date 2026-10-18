#!/usr/bin/env python3
"""Pinned, checkout-local Neo4j service; no global installation or credentials."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import platform
import secrets
import socket
import subprocess
import tarfile
import time
import urllib.error
import urllib.request

HOME = Path(__file__).resolve().parents[1]
ROOT = HOME / "artifacts/neo4j"
VERSION = "5.26.31"
PACKAGES = {
    "neo4j.tar.gz": (
        f"https://dist.neo4j.org/neo4j-community-{VERSION}-unix.tar.gz",
        "f8fc23340561405f1ff10ca6ac2d317d095d3c74509a616883c45d7a61f5cfec",
    ),
    "jdk.tar.gz": (
        "https://github.com/adoptium/temurin21-binaries/releases/download/"
        "jdk-21.0.12.1%2B1/OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.12.1_1.tar.gz",
        "3623232f33a9c3baadf304480b2535f9a3cba8a58d42ecbb438ba267315d9998",
    ),
}
SERVER = ROOT / "packages" / f"neo4j-community-{VERSION}"
JDK = ROOT / "packages" / "jdk-21.0.12.1+1" / "Contents/Home"


def environment():
    env = os.environ.copy()
    env.update(JAVA_HOME=str(JDK), NEO4J_HOME=str(SERVER), NEO4J_CONF=str(ROOT / "conf"))
    return env


def install():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("Pinned runtime targets macOS arm64; select a verified platform package")
    ROOT.mkdir(parents=True, exist_ok=True)
    packages = ROOT / "packages"
    packages.mkdir(exist_ok=True)
    for name, (url, digest) in PACKAGES.items():
        archive = ROOT / name
        if not archive.exists():
            partial = archive.with_suffix(".download")
            subprocess.run(["curl", "--fail", "--location", "--retry", "2", "--output", str(partial), url], check=True)
            if hashlib.file_digest(partial.open("rb"), "sha256").hexdigest() != digest:
                raise RuntimeError(f"Publisher digest mismatch: {name}")
            partial.rename(archive)
        if hashlib.file_digest(archive.open("rb"), "sha256").hexdigest() != digest:
            raise RuntimeError(f"Retained package changed: {name}")
        target = SERVER if name == "neo4j.tar.gz" else JDK
        if not target.exists():
            with tarfile.open(archive) as tar:
                tar.extractall(packages, filter="data")
    if not (JDK / "bin/java").exists():
        raise RuntimeError("Unexpected JDK archive layout")
    conf = ROOT / "conf"
    conf.mkdir(exist_ok=True)
    settings = f"""server.default_listen_address=127.0.0.1
server.default_advertised_address=127.0.0.1
server.http.enabled=true
server.http.listen_address=127.0.0.1:17474
server.http.advertised_address=127.0.0.1:17474
server.bolt.enabled=true
server.bolt.listen_address=127.0.0.1:17687
server.bolt.advertised_address=127.0.0.1:17687
server.https.enabled=false
dbms.security.auth_enabled=true
server.directories.data={ROOT / 'data'}
server.directories.logs={ROOT / 'logs'}
server.directories.run={ROOT / 'run'}
server.directories.import={ROOT / 'import'}
server.memory.heap.initial_size=256m
server.memory.heap.max_size=1g
server.memory.pagecache.size=256m
server.queryapi.transaction_idle_timeout=10s
db.transaction.timeout=120s
"""
    path = conf / "neo4j.conf"
    if path.exists() and path.read_text() != settings:
        raise RuntimeError("Existing runtime configuration differs; review it explicitly")
    if not path.exists():
        path.write_text(settings)
    password = ROOT / "password"
    if not password.exists():
        with password.open("x") as f:
            os.chmod(password, 0o600)
            f.write(secrets.token_urlsafe(32))
    if not (ROOT / "data/dbms/auth.ini").exists() and not (ROOT / "data/databases/system").exists():
        # neo4j-admin receives the password only for local initialization.
        result = subprocess.run([str(SERVER / "bin/neo4j-admin"), "dbms", "set-initial-password", password.read_text()], env=environment(), capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr.replace(password.read_text(), "[redacted]"))
    connection = {"schema_version": "axthena.neo4j-connection.v1", "endpoint": "http://127.0.0.1:17474", "database": "neo4j", "user": "neo4j", "password_file": str(password)}
    config = ROOT / "connection.json"
    if not config.exists():
        config.write_text(json.dumps(connection, indent=2) + "\n")
    (ROOT / "runtime.json").write_text(json.dumps({"neo4j": VERSION, "jdk": "21.0.12.1+1", "packages": PACKAGES}, indent=2) + "\n")


def probe():
    password = (ROOT / "password").read_text()
    request = urllib.request.Request("http://127.0.0.1:17474/db/neo4j/query/v2", data=json.dumps({"statement": "CALL dbms.components() YIELD name, versions RETURN name, versions", "accessMode": "Read"}).encode(), headers={"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(f"neo4j:{password}".encode()).decode()})
    with urllib.request.urlopen(request, timeout=5) as response:
        result = json.load(response)
    if result.get("errors"):
        raise RuntimeError(str(result["errors"]))
    return result["data"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["install", "start", "stop", "status"])
    command = parser.parse_args().command
    if command in ("install", "start"):
        install()
    if command == "start":
        status = subprocess.run([str(SERVER / "bin/neo4j"), "status"], env=environment(), capture_output=True)
        if status.returncode:
            for port in (17474, 17687):
                with socket.socket() as s:
                    if s.connect_ex(("127.0.0.1", port)) == 0:
                        raise RuntimeError(f"Port {port} is occupied by another service")
            subprocess.run([str(SERVER / "bin/neo4j"), "start"], env=environment(), check=True)
        deadline = time.monotonic() + 120
        while True:
            try:
                print(json.dumps(probe()))
                break
            except (OSError, RuntimeError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(1)
        print(f"Browser: http://127.0.0.1:17474/browser/\nUser: neo4j\nPassword file: {ROOT / 'password'}")
    elif command == "stop":
        subprocess.run([str(SERVER / "bin/neo4j"), "stop"], env=environment(), check=True)
    elif command == "status":
        print(json.dumps(probe(), indent=2))


if __name__ == "__main__":
    main()
