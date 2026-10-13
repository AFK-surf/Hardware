#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Leak check for the exported public tree (internal tool, not exported).

Usage: tools/check_public.py DIR

Scans every file under DIR and prints `path:line: [rule] match` for anything that looks like it came
from our private test environment: Wi-Fi networks and passwords, test-network addresses, MAC
addresses, backend IDs and tokens, personal names and hostnames, local paths, staging hosts, private
repository names, PR numbers and internal step/report references. Exits 1 if anything is found.

Secrets are never written here in clear text: the test Wi-Fi password is matched by its SHA-256, and
string values from a local, git-ignored firmware/main/secrets.h (next to this script) are added to
the deny list at run time. Run `tools/export_public.sh DEST` first; this script does not know which
files are tracked.
"""

import hashlib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (rule id, regex, flags). Each rule's matches are filtered through ALLOW below.
RULES = [
    # Wi-Fi networks seen in this project (office, home, the DevKit's router).
    ("wifi-ssid", r"HUAWEI-[A-Za-z0-9_-]+|ToEverything[A-Za-z0-9_-]*", 0),
    # Addresses on the test networks. 192.168.1.0/24 and 192.168.2.0/24 are allowed below as the
    # project's documentation and host-test example subnets.
    ("private-ip", r"\b192\.168\.\d{1,3}\.\d{1,3}\b", 0),
    ("private-ip", r"\b\d{1,3}[.|]\d{1,3}[.|]168[.|]192[.|]in-addr", re.I),  # reverse-DNS form
    ("mac", r"\b(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b", re.I),
    # BLE names carry the last two MAC bytes.
    ("device-name", r"\b[Cc]omma[-_][0-9A-Fa-f]{4}\b", 0),
    # Backend IDs: a prefix plus a base64url value. Code identifiers (dev_count, ws_client) are
    # single-case, so a value must mix upper and lower case or upper case and digits.
    ("backend-id",
     r"\b(?:dev|ws|grp1|run|vc|conn|usr|req|tun|agt|env)_"
     r"(?=[A-Za-z0-9_-]{4,})(?=[A-Za-z0-9_-]*[A-Z])(?=[A-Za-z0-9_-]*[a-z0-9])[A-Za-z0-9_-]+", 0),
    ("token", r"\bsalix_(?:conn|vk)_[A-Za-z0-9_-]+", 0),
    ("token", r"\b(?:ghp|gho|github_pat)_[A-Za-z0-9_]{20,}|\bsk-[A-Za-z0-9_-]{20,}|AKIA[0-9A-Z]{16}", 0),
    ("token", r"-----BEGIN [A-Z ]*PRIVATE KEY-----", 0),
    ("personal-name", r"\b(?:a{4}-)?(?:ma)?fu?zhen'?s?\b|\bshawn\b|\bzanwei\b|\bjimmfly\b", re.I),
    ("email", r"\bm502002313\b|\bipxine\b|\bsu0k57ef\b|[A-Za-z0-9._%+-]+@(?:gmail|cue|comma)\.[a-z]+", re.I),
    ("hostname", r"\b[A-Za-z0-9-]+-(?:MacBook|iMac|Mac-mini|Mac-Studio)[A-Za-z0-9-]*|\bzhen-core\b|\.ts\.net\b", re.I),
    ("local-path", r"/Users/[A-Za-z0-9._-]+|~/Desktop\b|/home/[a-z][a-z0-9_-]*/|C:\\Users\\", 0),
    ("staging-host", r"[A-Za-z0-9.-]*staging[A-Za-z0-9-]*\.[A-Za-z0-9.-]+|\bcue\.surf\b", re.I),
    ("private-repo",
     r"\bAFK-surf\b|\bsalix-connect\b|\bcomma-android\b|\bvoice-box-agent\b|\bsalix_agent\b|\bSalixAgent\b"
     r"|\bCue\s+(?:`?main`?|PR|repo(?:sitory)?|commit|`[0-9a-f]{7,}`)"
     r"|\bsystems/(?:apps|connector|station)\b|\b(?:salix_web|salix_env|comma_core|comma_web)\b", 0),
    # The pre-release name. Code, files and log tags were renamed to comma; this catches leftovers.
    ("old-name", r"\b(?:cue|CUE)[_-]\w*|\"cue\"|\bcue:", 0),
    ("pr-number", r"\bPRs?\s*#?\d+|/pull/\d+", 0),
    ("internal-doc", r"docs/(?:prompts|reports)\b|\b(?:prompts|reports)/[A-Za-z0-9_-]+\.md|-report\.md\b", 0),
    # Development step names (A1..A5, B1b, H2 ...), e.g. "see the H2 report". ESP32-H2/-C3 are chips.
    ("internal-step",
     r"(?<!ESP32-)(?<![-.\w])(?:A[1-5]|B[1-3][a-e]?|C[1-4][a-d]?|D1|E[1-4][a-c]?|F[12]|G[1-3]|H[1-5]b?|V[12])\b"
     r"(?![-.]\d)", 0),
]

# Secrets that must never appear, as (length, SHA-256): every substring of that length is hashed.
SECRET_SHA256 = [
    (25, "a32d2cbde26622014e407d680a8e6af2cc6e4e54d8725b9aeeed000b9f6f640d"),  # test Wi-Fi password
]

# (rule, path glob or None for any, regex the matched text must fully match, reason)
ALLOW = [
    ("private-ip", None, r"192\.168\.[12]\.\d{1,3}",
     "192.168.1.0/24 and 192.168.2.0/24 are this project's documentation/test example subnets"),
    ("private-ip", None, r"\d{1,3}[.|][12][.|]168[.|]192[.|]in-addr",
     "reverse-DNS form of the example subnets"),
    ("mac", None, r"01:02:03:04:05:06", "documentation example MAC (multicast bit set, never a device)"),
    ("mac", None, r"(?i)[0-9a-f][26ae](?:[:-][0-9a-f]{2}){5}",
     "locally administered MAC: never burned into hardware, used for examples and test fixtures"),
    ("device-name", None, r"[Cc]omma-1[Aa]2[Bb]", "documentation example name (short ID 1A2B)"),
    ("backend-id", None, r"(?:dev|ws|grp1|run|vc|conn|usr|req|tun)_(?:EXAMPLE|Example)[A-Za-z0-9_-]*",
     "documentation placeholders"),
    ("token", None, r"salix_(?:conn|vk)_(?:replace_me|EXAMPLE)?", "placeholder prefixes in docs and secrets.h.example"),
    ("token", None, r"salix_conn_(?:dev_override|x)", "dummy tokens: the dev URL override and a malformed-write test"),
    ("old-name", "firmware/main/device_config.c",
     r"CUE_(?:VOICE_SERVER|VOICE_GROUP|VOICE_KEY|WORKSPACE_ID|CONNECTOR_URL|CONNECTOR_TOKEN)?",
     "the build error that names old secrets.h macros"),
    ("internal-step", "firmware/components/esp_lcd_gc9d01/*", r".*", "vendored Espressif driver (register names)"),
    ("internal-step", "LICENSE", r".*", "Apache-2.0 text"),
    ("internal-step", "THIRD_PARTY_NOTICES.md", r"V2", "license names (e.g. MIT-0, OFL 1.1)"),
    ("internal-step", "firmware/reference/fonts/*", r".*", "font binary"),
    ("internal-step", "firmware/ui/*/ui_font.c", r".*", "generated glyph tables"),
]

TEXT_SKIP_SUFFIXES = {".png", ".jpg", ".ttf", ".bin"}


def load_local_secrets():
    """String values of local secrets.h files (never tracked), in this checkout and the main one."""
    roots = {ROOT}
    try:
        common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute",
                                 "--git-common-dir"], capture_output=True, text=True, check=True)
        roots.add(pathlib.Path(common.stdout.strip()).parent)
    except (OSError, subprocess.CalledProcessError):
        pass
    values = set()
    for root in roots:
        path = root / "firmware/main/secrets.h"
        if path.is_file():
            values.update(re.findall(r'#\s*define\s+\w+\s+"([^"]{6,})"', path.read_text(errors="replace")))
    return sorted(v for v in values if v != "changeme" and "replace_me" not in v)


def allowed(rule, rel, text):
    for a_rule, glob, pattern, _ in ALLOW:
        if a_rule != rule:
            continue
        if glob and not pathlib.PurePosixPath(rel).match(glob):
            continue
        if re.fullmatch(pattern, text):
            return True
    return False


def scan_text(rel, text, local_secrets):
    hits = []
    for lineno, line in enumerate(text.splitlines(), 1):
        for rule, pattern, flags in RULES:
            for m in re.finditer(pattern, line, flags):
                if not allowed(rule, rel, m.group(0)):
                    hits.append((rel, lineno, rule, m.group(0)))
        for length, digest in SECRET_SHA256:
            for start in range(len(line) - length + 1):
                if hashlib.sha256(line[start:start + length].encode()).hexdigest() == digest:
                    hits.append((rel, lineno, "wifi-password", "<redacted>"))
        for secret in local_secrets:
            if secret in line:
                hits.append((rel, lineno, "local-secret", "<value from secrets.h>"))
    return hits


def scan_binary(rel, data, local_secrets):
    """Binary files (images, fonts): look for embedded text such as paths or names in metadata."""
    hits = []
    text = data.decode("latin-1")
    for rule in ("local-path", "personal-name", "hostname", "email", "staging-host", "private-repo"):
        for r_id, pattern, flags in RULES:
            if r_id != rule:
                continue
            for m in re.finditer(pattern, text, flags):
                if not allowed(rule, rel, m.group(0)):
                    hits.append((rel, f"byte {m.start()}", rule, m.group(0)))
    for secret in local_secrets:
        if secret in text:
            hits.append((rel, "binary", "local-secret", "<value from secrets.h>"))
    return hits


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    base = pathlib.Path(sys.argv[1])
    if not base.is_dir():
        print(f"not a directory: {base}", file=sys.stderr)
        return 2
    local_secrets = load_local_secrets()
    hits = []
    files = sorted(p for p in base.rglob("*") if p.is_file() and ".git" not in p.relative_to(base).parts)
    for path in files:
        rel = path.relative_to(base).as_posix()
        data = path.read_bytes()
        if path.suffix.lower() in TEXT_SKIP_SUFFIXES or b"\0" in data[:4096]:
            hits += scan_binary(rel, data, local_secrets)
        else:
            hits += scan_text(rel, data.decode("utf-8", errors="replace"), local_secrets)
    for rel, line, rule, text in hits:
        print(f"{rel}:{line}: [{rule}] {text}")
    print(f"{len(files)} files checked, {len(hits)} hit(s)", file=sys.stderr)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
