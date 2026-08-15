#!/bin/sh
# tag command: read NUL-separated paths on stdin, emit the area each
# page declares in its "area:" frontmatter line, as "path<TAB>area=NAME".
# A page with no such line yields no output, which the tool reads as no
# tag. Run from the docs tree root; paths are tree-relative.

# Read the whole NUL-separated stream, split on NUL.
paths=$(tr '\0' '\n')

printf '%s\n' "$paths" | while IFS= read -r path; do
	[ -n "$path" ] || continue
	[ -f "$path" ] || continue
	# The first "area: NAME" line names the page area.
	area=$(sed -n 's/^area:[[:space:]]*//p' "$path" | head -n 1)
	[ -n "$area" ] || continue
	printf '%s\t%s\n' "$path" "area=$area"
done
