"""changelog/ helper for CI. changelog/<X.Y.Z>/en.md holds the notes of one version.

  release_notes.py check <version>   fails when changelog/<version>/en.md is missing
  release_notes.py notes <version>   prints the notes of every version since the previous X.Y.0 up to this one
"""
import re
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parent.parent / "changelog"
LANG = "en"


def parse_version(text):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        sys.exit(f"not a plain X.Y.Z version: {text}")
    return tuple(map(int, match.groups()))


def name(version):
    return ".".join(map(str, version))


def versions():
    for folder in CHANGELOG.iterdir():
        if re.fullmatch(r"\d+\.\d+\.\d+", folder.name) and (folder / f"{LANG}.md").is_file():
            yield parse_version(folder.name)


def release_versions(version):
    previous_release = max((v for v in versions() if v[2] == 0 and v < version), default=None)
    picked = [v for v in versions() if v <= version and (previous_release is None or v > previous_release)]
    return sorted(picked, reverse=True)


def notes(version):
    text = (CHANGELOG / name(version) / f"{LANG}.md").read_text(encoding="utf-8").strip()
    return f"## {name(version)}\n\n{text}"


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("check", "notes"):
        sys.exit(__doc__)
    version = parse_version(sys.argv[2])
    if version not in set(versions()):
        sys.exit(f"changelog/{sys.argv[2]}/{LANG}.md is missing")
    if sys.argv[1] == "notes":
        print("\n\n".join(notes(v) for v in release_versions(version)))


if __name__ == "__main__":
    main()
