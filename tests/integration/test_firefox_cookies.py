"""Exercise real SQLite/WAL cookie capture without Firefox or network access."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


def database(path, rows):
    path.parent.mkdir(parents=True)
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute("PRAGMA wal_autocheckpoint=0")
    connection.execute("CREATE TABLE moz_cookies (host, path, name, value, expiry, isSecure, isHttpOnly)")
    connection.executemany("INSERT INTO moz_cookies VALUES (?, ?, ?, ?, ?, ?, ?)", rows)
    connection.commit()
    return connection


def main():
    script = Path(sys.argv[1]) / "scripts/grab-firefox-cookies.sh"
    with tempfile.TemporaryDirectory(prefix="firefox-cookies-", dir=os.getcwd()) as temporary:
        root = Path(temporary)
        profiles = root / "profiles with spaces"
        inactive = profiles / "inactive/cookies.sqlite"
        active = profiles / "active/cookies.sqlite"
        old = database(inactive, [("admin.booking.com", "/", "session", "stale-private", 0, 1, 1)])
        old.close()
        live = database(active, [
            (".booking.com", "/", "session", "fresh-private", 0, 1, 1),
            ("admin.booking.com", "/hotel", "scoped", "scoped-private", 4102444800, 0, 1),
            ("admin.booking.com", "/", "expired", "expired-private", 1, 1, 1),
            ("evilbooking.com", "/", "foreign", "foreign-private", 0, 0, 0),
        ])
        try:
            # Firefox is still open: its latest cookies are exclusively in WAL.
            os.utime(active, (100, 100))
            os.utime(inactive, (200, 200))
            os.utime(Path(str(active) + "-wal"), (300, 300))
            output = root / "exports/cookies.json"
            env = dict(os.environ, FIREFOX_PROFILE_ROOT=str(profiles))
            env.pop("FIREFOX_PROFILE_DIR", None)
            result = subprocess.run(["bash", str(script), str(output), "--verbose"],
                                    env=env, capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result.stderr
            cookies = json.loads(output.read_text())
            assert [row["name"] for row in cookies] == ["session", "scoped"], cookies
            assert cookies[0]["value"] == "fresh-private", cookies
            assert cookies[0]["secure"] is True and cookies[0]["httpOnly"] is True
            assert cookies[1]["secure"] is False and cookies[1]["httpOnly"] is True
            assert output.stat().st_mode & 0o077 == 0
            assert "exported 2" in result.stderr
            assert "active/cookies.sqlite" in result.stderr
            for private in ("fresh-private", "scoped-private", "expired-private", "foreign-private", "stale-private"):
                assert private not in result.stdout + result.stderr

            # Explicit profile selection wins even with another active WAL.
            result = subprocess.run(["bash", str(script), str(output)],
                                    env=dict(env, FIREFOX_PROFILE_DIR=str(inactive.parent)),
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result.stderr
            assert json.loads(output.read_text())[0]["value"] == "stale-private"

            # Empty/unavailable/broken snapshots cannot replace a prior export.
            empty = database(profiles / "empty/cookies.sqlite", [])
            empty.close()
            broken = profiles / "broken"
            broken.mkdir()
            (broken / "cookies.sqlite").write_text("private-invalid-database")
            prior = output.read_bytes()
            for profile in (profiles / "empty", profiles / "missing", broken):
                result = subprocess.run(["bash", str(script), str(output), "-v"],
                                        env=dict(env, FIREFOX_PROFILE_DIR=str(profile)),
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode != 0
                assert output.read_bytes() == prior
                assert "existing export preserved" in result.stderr
                assert "private-invalid-database" not in result.stderr
            assert not list(output.parent.glob(".firefox-cookies-*"))
        finally:
            live.close()
    print("PASS: Firefox WAL/profile selection, booleans, expiry/domain filtering, atomic export and secret-free diagnostics")


if __name__ == "__main__":
    main()
