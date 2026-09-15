#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

JAR="tools/plantuml.jar"
[ -f "$JAR" ] || { echo "Missing $JAR"; exit 1; }

echo "Rendering all .puml → .svg ..."
find docs/uml -name '*.puml' -exec java -jar "$JAR" -tsvg {} +
echo "Done."
find docs/uml -name '*.svg' | wc -l
