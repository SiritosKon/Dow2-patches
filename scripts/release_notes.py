"""CHANGELOG.md helper for CI.

  release_notes.py check <version>   fails when CHANGELOG.md has no entry for the version
  release_notes.py notes <version>   prints the entries since the previous X.Y.0 up to the version (release notes)
"""
import re
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parent.parent / "CHANGELOG.md"
HEADING = re.compile(r"^## (\d+)\.(\d+)\.(\d+)\b.*$", re.M)


def parse_version(text):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        sys.exit(f"not a plain X.Y.Z version: {text}")
    return tuple(map(int, match.groups()))


def entries():
    source = CHANGELOG.read_text(encoding="utf-8")
    headings = list(HEADING.finditer(source))
    for i, heading in enumerate(headings):
        end = headings[i + 1].start() if i + 1 < len(headings) else len(source)
        yield tuple(map(int, heading.groups())), source[heading.start():end].strip()


def release_entries(version):
    previous_release = max(
        (v for v, _ in entries() if v[2] == 0 and v < version), default=None
    )
    picked = [(v, text) for v, text in entries() if v <= version and (previous_release is None or v > previous_release)]
    return [text for _, text in sorted(picked, reverse=True)]


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("check", "notes"):
        sys.exit(__doc__)
    version = parse_version(sys.argv[2])
    if version not in {v for v, _ in entries()}:
        sys.exit(f"CHANGELOG.md has no entry for {sys.argv[2]}")
    if sys.argv[1] == "notes":
        print("\n\n".join(release_entries(version)))


if __name__ == "__main__":
    main()
