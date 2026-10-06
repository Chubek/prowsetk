"""Booking assistant scripts: option forwarding, validation and value-free help.

Covers `run-opencode-marionette.sh` and `browser-run-assist.exp` without an
OpenCode server, a Cloudflare account, or a display: the fetch step is replaced
by a stub and the marionette runner by an argv echo, so the assertions are about
the exact command line these scripts build.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = sys.argv[1]
del sys.argv[1:]
EXAMPLE = Path(SOURCE) / "examples/booking-dotcom-admin-api"
MARIONETTE = EXAMPLE / "run-opencode-marionette.sh"
FETCH = EXAMPLE / "fetch-browser-run-snapshot.sh"
ASSIST = EXAMPLE / "browser-run-assist.exp"
BEACON = "//h1[contains(., 'two words') and @id='*']"


def sh(script, *args, **kwargs):
    return subprocess.run([str(script), *args], capture_output=True, text=True,
                          timeout=20, **kwargs)


class RunnerWrapper(unittest.TestCase):
    """The wrapper must build an exact argv and reject unusable values."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pm-", dir=os.getcwd())
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)
        (self.root / "snap.html").write_text("<p>x</p>")
        (self.root / "cfg.toml").write_text('[project]\nname = "x"\n')
        self.echo = self.root / "echo.sh"
        self.echo.write_text('#!/bin/sh\nfor a in "$@"; do printf "[%s]" "$a"; done\nprintf "\\n"\n')
        self.echo.chmod(0o755)

    def runner(self, *args):
        return sh(MARIONETTE, "--snapshot", str(self.root / "snap.html"),
                  "--marionette-bin", str(self.echo), *args)

    def test_help_is_complete_and_mentions_each_forwarded_option(self):
        text = sh(MARIONETTE, "--help").stdout
        # The usage block is extracted by marker; a truncated block would lose
        # its last line, which is the assistant-browser-force option.
        self.assertIn("--assistant-browser-force BOOL", text)
        self.assertIn("--opencode-api-prefix /api", text)
        for option in ("--verbose", "--no-xcors", "--max-steps", "--max-page-requests",
                       "--opencode-max-requests", "--opencode-wait-ms", "--max-get-probes",
                       "--booking-config"):
            self.assertIn(option, text)
        self.assertIn("booking-dotcom-admin", text)
        # The fetch helper prints its own marker block; a fixed line range would
        # drift as soon as the header changes.
        fetch_help = sh(FETCH, "--help").stdout
        self.assertIn("--browser-run-bin BIN", fetch_help)
        self.assertIn("refuses existing ones", fetch_help)

    def test_every_option_reaches_the_runner_as_one_argument(self):
        result = self.runner("--verbose", "--no-xcors", "false", "--max-steps", "24",
                             "--max-page-requests", "256", "--opencode-max-requests", "128",
                             "--opencode-wait-ms", "30000", "--opencode-api-prefix", "/api")
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = result.stdout.strip()
        for token in ("[--no-xcors][false]", "[--max-steps][24]", "[--max-page-requests][256]",
                      "[--opencode-max-requests][128]", "[--opencode-wait-ms][30000]",
                      "[--opencode-api-prefix][/api]", "[--verbose]"):
            self.assertIn(token, argv)
        # Five positional arguments, six valued options, one valueless flag.
        self.assertEqual(argv.count("["), 5 + 6 * 2 + 1)

    def test_a_value_with_spaces_and_globs_stays_a_single_argument(self):
        result = self.runner("--booking-config", str(self.root / "cfg.toml"),
                             "--success-beacon", BEACON, "--dotenv", "a b.env")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[" + BEACON + "]", result.stdout)
        self.assertIn("[a b.env]", result.stdout)

    def test_underscore_aliases_match_the_runner_spelling(self):
        result = self.runner("--booking-config", str(self.root / "cfg.toml"),
                             "--success_beacon", "//h2", "--success_beacon_type", "css",
                             "--assistant_browser_force", "true", "--no_xcors", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        for token in ("[--success-beacon][//h2]", "[--success-beacon-type][css]",
                      "[--assistant-browser-force][true]", "[--no-xcors][true]"):
            self.assertIn(token, result.stdout)

    def test_login_options_require_a_booking_config(self):
        # Each value is individually valid, so the coupling check is what rejects it.
        valid = {"--cookies-json": "cookies.json", "--dotenv": ".env",
                 "--success-beacon": "//h1", "--success-beacon-type": "xpath",
                 "--assistant-browser-force": "true", "--max-get-probes": "4"}
        for option, value in valid.items():
            with self.subTest(option=option):
                result = self.runner(option, value)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("login options also require --booking-config", result.stderr)
                self.assertNotIn("echo.sh", result.stdout)

    def test_malformed_values_are_usage_errors(self):
        cases = {
            "--max-steps": "abc", "--max-steps": "-1", "--max-steps": "1.5",
            "--max-page-requests": "x", "--opencode-max-requests": "",
            "--opencode-wait-ms": "1e3", "--no-xcors": "maybe",
            "--assistant-browser-force": "1", "--opencode-api-prefix": "/v2",
            "--max-get-probes": "0x1",
        }
        for option, value in cases.items():
            with self.subTest(option=option, value=value):
                result = self.runner(option, value)
                self.assertEqual(result.returncode, 2, (result.stdout, result.stderr))
                # The runner stub echoes its argv, so it must never be spawned.
                self.assertNotIn("echo.sh", result.stdout)
                self.assertIn(option, result.stderr)

    def test_unknown_and_incomplete_options_are_usage_errors(self):
        self.assertEqual(self.runner("--nope", "x").returncode, 2)
        incomplete = sh(MARIONETTE, "--snapshot", str(self.root / "snap.html"),
                        "--marionette-bin", str(self.echo), "--max-steps")
        self.assertEqual(incomplete.returncode, 2)
        self.assertIn("needs a value", incomplete.stderr)
        missing = sh(MARIONETTE, "--marionette-bin", str(self.echo))
        self.assertEqual(missing.returncode, 2)
        self.assertIn("--snapshot", missing.stderr)

    def test_an_unreadable_snapshot_fails_before_the_runner_is_reached(self):
        result = sh(MARIONETTE, "--snapshot", str(self.root / "absent.html"),
                    "--marionette-bin", str(self.echo))
        self.assertEqual(result.returncode, 1)
        self.assertIn("snapshot file unreadable", result.stderr)


class AssistExpect(unittest.TestCase):
    """The Expect launcher forwards marionette tuning and validates it first."""

    @classmethod
    def setUpClass(cls):
        cls.expect = shutil.which("expect")
        if not cls.expect:
            raise unittest.SkipTest("Expect unavailable")

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="br-", dir=os.getcwd())
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)
        self.fetch = self.root / "fetch.sh"
        self.fetch.write_text(
            '#!/bin/sh\nout=""\nwhile [ $# -gt 0 ]; do case "$1" in --output) out="$2"; shift 2 ;;'
            ' *) shift ;; esac; done\nmkdir -p "$(dirname "$out")"\nprintf "<p>x</p>" > "$out"\n'
            'echo "Browser Run operation completed"\n')
        self.fetch.chmod(0o755)
        self.echo = self.root / "echo.sh"
        self.echo.write_text('#!/bin/sh\nfor a in "$@"; do printf "[%s]" "$a"; done\nprintf "\\n"\n')
        self.echo.chmod(0o755)

    def assist(self, *args, **environment):
        env = dict(os.environ, **environment)
        return subprocess.run([self.expect, str(ASSIST), *args], capture_output=True,
                              text=True, timeout=30, env=env, stdin=subprocess.DEVNULL)

    def test_help_lists_the_marionette_options_and_their_scope(self):
        text = self.assist("--help").stdout
        self.assertIn("--verbose", text)
        self.assertIn("--opencode-api-prefix /api", text)
        self.assertIn("(marionette only)", text)

    def test_tuning_options_reach_the_runner_through_the_wrapper(self):
        result = self.assist("--fetch-bin", str(self.fetch), "--to", "marionette",
                             "--marionette-bin", str(MARIONETTE),
                             "--output", str(self.root / "snap.html"),
                             "--openapi", str(self.root / "o.yaml"),
                             "--postman", str(self.root / "p.json"),
                             "--verbose", "--no-xcors", "true", "--max-steps", "9",
                             "--max-page-requests", "64", "--opencode-max-requests", "128",
                             "--opencode-wait-ms", "30000", "--opencode-api-prefix", "/api",
                             PROWSETK_MARIONETTE_BIN=str(self.echo),
                             CLOUDFLARE_ACCOUNT_ID="0123456789abcdef0123456789abcdef",
                             CLOUDFLARE_API_TOKEN="token-must-not-reach-argv")
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
        for token in ("[--no-xcors][true]", "[--max-steps][9]", "[--max-page-requests][64]",
                      "[--opencode-max-requests][128]", "[--opencode-wait-ms][30000]",
                      "[--opencode-api-prefix][/api]", "[--verbose]"):
            self.assertIn(token, result.stdout)
        self.assertNotIn("token-must-not-reach-argv", result.stdout + result.stderr)

    def test_malformed_values_never_reach_the_downstream(self):
        cases = {
            "--no-xcors": "maybe", "--max-steps": "abc", "--max-steps": "-1",
            "--max-page-requests": "1.5", "--opencode-max-requests": "",
            "--opencode-wait-ms": "1e3", "--opencode-api-prefix": "/v2",
        }
        for option, value in cases.items():
            with self.subTest(option=option, value=value):
                result = self.assist(option, value)
                self.assertEqual(result.returncode, 2, (result.stdout, result.stderr))
                self.assertNotIn("snapshot saved", result.stdout)

    def test_driver_mode_says_marionette_options_are_ignored(self):
        result = self.assist("--to", "driver", "--max-steps", "5",
                             "--fetch-bin", str(self.fetch))
        self.assertIn("marionette options are ignored by --to driver", result.stdout)


if __name__ == "__main__":
    unittest.main()
