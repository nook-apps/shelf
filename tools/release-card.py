#!/usr/bin/env python3
"""Render a Shelf release card from the self-contained HTML template."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from urllib.parse import urlencode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Version shown on the card, for example 0.5.2")
    parser.add_argument("description", help="Short English release summary (up to 105 characters)")
    parser.add_argument("--type", choices=("release", "update", "bugfix", "milestone", "announce"), default="update")
    parser.add_argument("--secondary", choices=("release", "update", "bugfix", "milestone", "announce"), default="bugfix")
    parser.add_argument("--output", type=Path, help="PNG path; defaults to docs/release-cards/shelf-VERSION.png")
    args = parser.parse_args()
    if len(args.description) > 105:
        parser.error("description must fit the template's 105-character limit")
    if args.type == args.secondary:
        parser.error("primary and secondary labels must differ")

    browser = next((path for name in ("chromium-browser", "chromium", "google-chrome")
                    if (path := shutil.which(name))), None)
    if not browser:
        parser.error("Chromium or Google Chrome is required to render the card")
    template = Path(__file__).with_name("release-card.html")
    output = args.output or template.parent.parent / "docs" / "release-cards" / f"shelf-{args.version}.png"
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    query = urlencode({"preview": "", "lang": "en", "product": "shelf", "title": "Shelf",
                       "version": args.version, "description": args.description,
                       "type": args.type, "secondary": args.secondary})
    url = template.resolve().as_uri() + "?" + query
    with tempfile.TemporaryDirectory(prefix="shelf-release-card-") as profile:
        subprocess.run([browser, "--headless=new", "--disable-gpu",
                        f"--user-data-dir={profile}", "--hide-scrollbars",
                        "--force-device-scale-factor=1", "--window-size=1200,630",
                        "--virtual-time-budget=2000", f"--screenshot={output}", url], check=True)
    print(output)


if __name__ == "__main__":
    main()
