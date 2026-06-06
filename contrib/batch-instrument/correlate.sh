#!/bin/bash
#
# correlate.sh - Map instrument diff offsets to object files
#
# Usage: ./correlate.sh <symmap> <instrument.tsv>
#
# Reads the symmap (object file ranges) and instrument output (changed
# byte offsets), maps each changed range to its owning object file.

SYMMAP="$1"
INST="$2"

if [ ! -f "$SYMMAP" ] || [ ! -f "$INST" ]; then
	echo "usage: $0 <symmap> <instrument.tsv>" >&2
	exit 1
fi

awk -F'\t' '
# Pass 1: Load symmap (object-file ranges)
FNR == NR && /^(data|bss)\t/ && $4 != "" && $4 != "*fill*" && $3 > 0 {
	n = range_count[$1]++
	range_section[n,$1] = $1
	range_offset[n,$1] = $2 + 0
	range_size[n,$1] = $3 + 0
	range_obj[n,$1] = $4
	next
}

# Skip comments and headers
/^#/ { next }

# Pass 2: Map each instrument diff to an object file
FNR != NR {
	section = $3
	offset = $4 + 0
	len = $5 + 0
	cmd = $2
	seq = $1

	obj = "<unmapped>"
	count = range_count[section]
	for (i = 0; i < count; i++) {
		roff = range_offset[i, section]
		rsz = range_size[i, section]
		if (offset >= roff && offset < roff + rsz) {
			obj = range_obj[i, section]
			break
		}
	}

	# Shorten object path
	gsub(/.*\//, "", obj)
	printf "%s\t%s\t%s\t%d\t%d\t%s\n", seq, cmd, section, offset, length, obj
}
' "$SYMMAP" "$INST"
