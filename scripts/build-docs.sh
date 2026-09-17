#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
manual_dir="$project_root/manual"
output_dir="${1:-$project_root/build/docs}"

if ! command -v pandoc >/dev/null 2>&1; then
    printf 'build-docs: pandoc is required to build HTML and LaTeX\n' >&2
    exit 1
fi

if [[ ! -f "$manual_dir/README.md" ]]; then
    printf 'build-docs: manual/README.md is missing; the manual is not complete\n' >&2
    exit 1
fi

chapters=()
for chapter_number in $(seq -w 1 30); do
    matches=("$manual_dir"/"$chapter_number"-*.md)
    if [[ ${#matches[@]} != 1 || ! -f ${matches[0]} ]]; then
        printf 'build-docs: expected exactly one chapter %s in manual/\n' "$chapter_number" >&2
        exit 1
    fi
    chapters+=("${matches[0]}")
done

all_chapters=("$manual_dir"/[0-9][0-9]-*.md)
if [[ ${#all_chapters[@]} != 30 ]]; then
    printf 'build-docs: expected 30 chapter files in manual/\n' >&2
    exit 1
fi

mkdir -p -- "$output_dir/html" "$output_dir/latex"
pandoc --standalone --toc --metadata title='ProwseTk Manual' \
    -f markdown -t html5 -o "$output_dir/html/index.html" \
    "$manual_dir/README.md" "${chapters[@]}"
pandoc --standalone --toc --metadata title='ProwseTk Manual' \
    -f markdown -t latex -o "$output_dir/latex/prowsetk.tex" \
    "$manual_dir/README.md" "${chapters[@]}"
printf 'Built %s and %s\n' "$output_dir/html/index.html" \
    "$output_dir/latex/prowsetk.tex"
