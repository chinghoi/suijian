#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 <app bundle> <volume name> <output.dmg>" >&2
    exit 2
fi

app_bundle=$1
volume_name=$2
output_image=$3

if [[ ! -d "$app_bundle/Contents" ]]; then
    echo "App bundle not found: $app_bundle" >&2
    exit 1
fi

staging=$(mktemp -d "${TMPDIR:-/tmp}/suijian-dmg.XXXXXX")
trap 'rm -r "$staging"' EXIT

ditto "$app_bundle" "$staging/随剪.app"
ln -s /Applications "$staging/Applications"

hdiutil create -volname "$volume_name" -srcfolder "$staging" -ov -format UDZO "$output_image"
