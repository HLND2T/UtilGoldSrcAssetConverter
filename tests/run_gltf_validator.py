"""Runs the Khronos glTF-Validator on every .glb/.gltf below a directory.

The validator is the npm package gltf-validator, installed once into the cache directory; validate_gltf.js, next to
this script, drives it through NODE_PATH. Fails when the validator reports any error.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

VALIDATOR_PACKAGE = "gltf-validator@2.0.0-dev.3.10"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, help="directory the npm package is installed into")
    parser.add_argument("directory", help="directory holding the files to validate")
    arguments = parser.parse_args()

    node = shutil.which("node")
    npm = shutil.which("npm")

    if not node or not npm:
        print("node and npm are required to run the glTF-Validator", file=sys.stderr)
        return 1

    cache = pathlib.Path(arguments.cache)
    modules = cache / "node_modules"

    if not (modules / "gltf-validator").is_dir():
        cache.mkdir(parents=True, exist_ok=True)
        subprocess.run([npm, "install", "--prefix", str(cache), "--no-audit", "--no-fund", VALIDATOR_PACKAGE], check=True)

    files = sorted(str(path) for path in pathlib.Path(arguments.directory).rglob("*") if path.suffix in (".glb", ".gltf"))

    if not files:
        print(f"no glTF files below {arguments.directory}", file=sys.stderr)
        return 1

    environment = dict(os.environ, NODE_PATH=str(modules))
    script = pathlib.Path(__file__).with_name("validate_gltf.js")

    return subprocess.run([node, str(script), *files], env=environment).returncode


if __name__ == "__main__":
    sys.exit(main())
