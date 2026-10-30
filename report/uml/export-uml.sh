#!/usr/bin/env bash
# Renders the six Mermaid diagrams in UML-diagrams.md to vector PDFs used by
# report/XX_CodeAgent.tex (report/screenshots/uml-*.pdf). The Mermaid source
# is the only input; nothing is redrawn by hand.
#
# Needs Node.js. Uses Mermaid CLI (mmdc) from PATH if installed, otherwise
# npx downloads @mermaid-js/mermaid-cli and Puppeteer's Chromium.
# Optional: PUPPETEER_CONFIG=/path/to/puppeteer.json (e.g. a custom
# executablePath or "--no-sandbox"), passed to mmdc -p.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$root/report/screenshots"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if command -v mmdc >/dev/null 2>&1; then
  mmdc=(mmdc)
else
  mmdc=(npx -y -p @mermaid-js/mermaid-cli mmdc)
fi
extra=()
[[ -n "${PUPPETEER_CONFIG:-}" ]] && extra=(-p "$PUPPETEER_CONFIG")

# Markdown input: one PDF per mermaid block, numbered in order of appearance
# (d-1.pdf ... d-6.pdf). Each PDF page is sized to its diagram.
"${mmdc[@]}" "${extra[@]}" \
  -c "$here/mermaid-config.json" -C "$here/mermaid-print.css" \
  -i "$root/UML-diagrams.md" -o "$work/d.md" -e pdf -a "$work" -q

# Block order in UML-diagrams.md.
names=(uml-class uml-errors uml-seq-cli-generate uml-seq-cli-refactor
       uml-seq-gui-refactor uml-state)
count=$(ls "$work"/d-*.pdf | wc -l)
if [[ "$count" -ne ${#names[@]} ]]; then
  echo "expected ${#names[@]} diagrams in UML-diagrams.md, rendered $count" >&2
  exit 1
fi
for i in "${!names[@]}"; do
  cp "$work/d-$((i + 1)).pdf" "$out/${names[$i]}.pdf"
  echo "wrote report/screenshots/${names[$i]}.pdf"
done
