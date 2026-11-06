#!/usr/bin/env bash
# Install Microsoft's java-debug plugin (the jdtls bundle that serves DAP) so
# kajo-ide can debug Java. The jar comes from Maven Central and is checked
# against the published SHA-1 before it is installed.
#
#   scripts/install-java-debug.sh [version]      # default 0.53.1
#
# Installed to ${XDG_DATA_HOME:-~/.local/share}/kajo/java-debug/. Restart
# kajo-ide afterwards: jdtls loads the bundle at startup. Override the location
# with KAJO_JAVA_DEBUG_JAR=/path/to/com.microsoft.java.debug.plugin-*.jar.
set -euo pipefail

version="${1:-0.53.1}"
artifact="com.microsoft.java.debug.plugin"
base="https://repo1.maven.org/maven2/com/microsoft/java/${artifact}/${version}/${artifact}-${version}"
dest="${XDG_DATA_HOME:-$HOME/.local/share}/kajo/java-debug"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading ${artifact}-${version}.jar from Maven Central..."
curl -fsSL --max-time 120 -o "$tmp/plugin.jar" "${base}.jar"
curl -fsSL --max-time 30 -o "$tmp/plugin.jar.sha1" "${base}.jar.sha1"

expected="$(tr -d ' \n\r' < "$tmp/plugin.jar.sha1" | cut -c1-40)"
actual="$(sha1sum "$tmp/plugin.jar" | cut -d' ' -f1)"
if [ "$expected" != "$actual" ]; then
    echo "checksum mismatch: expected $expected, got $actual" >&2
    exit 1
fi

mkdir -p "$dest"
rm -f "$dest"/${artifact}-*.jar
install -m 0644 "$tmp/plugin.jar" "$dest/${artifact}-${version}.jar"
echo "Installed $dest/${artifact}-${version}.jar"
echo "Restart kajo-ide so jdtls loads the bundle."
