#!/bin/sh
# overrides command for the git example: read NUL-separated paths on
# stdin, emit each path's declared "area=" attribute from .gitattributes
# as "path<TAB>area=VALUE". A path with no area= attribute, or the value
# "unspecified", yields no line. This is the declarative override source:
# git check-attr reads the .gitattributes the maintainer wrote, and the
# adapter treats the value as the highest-precedence area label.
# Run from the git worktree root.

git check-attr area --stdin -z |
	tr '\0' '\n' |
	while IFS= read -r path && IFS= read -r _attr && IFS= read -r value; do
		[ -n "$path" ] || continue
		case "$value" in
		"" | unspecified) continue ;;
		esac
		printf '%s\tarea=%s\n' "$path" "$value"
	done
