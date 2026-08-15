#!/bin/sh
# validate command: check that every "[text](path)" link in every .md
# page resolves to an existing file, relative to the page it appears in.
# Exit 0 when every link resolves, 1 (naming the broken link) otherwise.
# Run from the docs tree root.

status=0

# Every .md file in the tree.
find . -type f -name '*.md' | while IFS= read -r page; do
	dir=$(dirname "$page")
	# Extract each link target: the text between "](" and the next ")".
	grep -oE '\]\([^)]+\)' "$page" | sed -E 's/^\]\(//; s/\)$//' |
	while IFS= read -r target; do
		[ -n "$target" ] || continue
		# Skip absolute URLs (a scheme like http:); only local links.
		case "$target" in
		*://*) continue ;;
		esac
		if [ ! -f "$dir/$target" ]; then
			printf 'broken link in %s: %s\n' "$page" "$target" >&2
			exit 1
		fi
	done || exit 1
done || status=1

exit $status
