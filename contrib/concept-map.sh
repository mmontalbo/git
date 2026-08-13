#!/bin/sh
#
# concept-map.sh: which of git's root .c files look like they belong together?
# A throwaway baseline. It groups them by the headers they share.
#
# A sample run, git master at 5b2471720c ("The 10th batch"). Each group is
# named by the headers its members most share; the members are the .c files.
# You can read subsystems straight off it: the wire, the index, commit and
# revision, diff, packs.
#
#   oid-array pkt-line remote (11)
#       connect.c fetch-pack.c http.c object-name.c remote-curl.c remote.c
#       send-pack.c shallow.c transport-helper.c transport.c upload-pack.c
#
#   sparse-index symlinks advice (9)
#       diff-lib.c dir.c entry.c merge-ort.c preload-index.c read-cache.c
#       sparse-index.c submodule.c unpack-trees.c
#
#   commit-slab commit-reach tag (7)
#       blame.c commit.c log-tree.c ref-filter.c revision.c sequencer.c
#       wt-status.c
#
#   diffcore oid-array quote (4)
#       apply.c combine-diff.c diff.c diffcore-rename.c
#
#   pack pack-bitmap pack-objects (3)
#       delta-islands.c pack-bitmap-write.c pack-bitmap.c
#
#   advice branch color (2)
#       config.c environment.c
#
#   chunk-format hash-lookup midx (2)
#       midx-write.c midx.c
#
# How it works:
#   1. For each top-level .c file, read its  #include "..."  lines.
#   2. Drop headers that most files include (git-compat-util.h, strbuf.h, ...).
#      They are shared plumbing and say nothing about which concept a file is.
#   3. Join two files when they share at least N of the remaining headers, then
#      let those links settle into groups. Each group is named by the headers
#      its members have most in common.
# No commit history is used; it just reads the files. Needs only git and perl.
#
# Run it:  ./contrib/concept-map.sh [N]     (headers two files must share to be
#                                            joined; default 6, higher = tighter)

set -eu
min="${1:-6}"
root=$(git rev-parse --show-toplevel)

git -C "$root" grep -E '^[[:space:]]*#[[:space:]]*include[[:space:]]+"[^"]+"' -- ':(glob)*.c' |
MIN="$min" perl -e '
    my (%inc, %hf);                           # file->headers, header->files
    while (<STDIN>) {
        chomp;
        my ($f, $rest) = split /:/, $_, 2;
        next unless defined $rest;
        my ($h) = $rest =~ /"([^"]+)"/;
        next unless defined $h;
        $h =~ s{.*/}{};                       # odb/source.h -> source
        $h =~ s{\.h$}{};
        $inc{$f}{$h} = 1;
        $hf{$h}{$f} = 1;
    }

    my $files = keys %inc;
    my $cap = $files * 0.12;                   # header in >12% of files = plumbing
    my %dist;
    for my $h (keys %hf) { $dist{$h} = 1 if keys %{$hf{$h}} <= $cap; }

    # How many distinctive headers each file pair shares.
    my %shared;
    for my $h (grep { $dist{$_} } keys %hf) {
        my @fs = sort keys %{$hf{$h}};
        for my $i (0 .. $#fs) {
            for my $j ($i + 1 .. $#fs) { $shared{"$fs[$i]\t$fs[$j]"}++; }
        }
    }

    # Join files that share at least MIN headers.
    my %adj;
    for (keys %shared) {
        next if $shared{$_} < $ENV{MIN};
        my ($a, $b) = split /\t/;
        $adj{$a}{$b} = $shared{$_};
        $adj{$b}{$a} = $shared{$_};
    }

    # Let the links settle into groups (label propagation). Deterministic:
    # fixed order, ties broken by lowest id. No chaining two groups through one
    # shared file, unlike simply following every link.
    my @nodes = sort keys %adj;
    my %lab; my $i = 0; $lab{$_} = $i++ for @nodes;
    for (1 .. 100) {
        my $ch = 0;
        for my $n (@nodes) {
            my %w;
            $w{$lab{$_}} += $adj{$n}{$_} for keys %{$adj{$n}};
            my $best = (sort { $w{$b} <=> $w{$a} or $a <=> $b } keys %w)[0];
            if ($best != $lab{$n}) { $lab{$n} = $best; $ch++; }
        }
        last unless $ch;
    }
    my %grp;
    push @{$grp{$lab{$_}}}, $_ for @nodes;

    my @groups = sort { @$b <=> @$a } grep { @$_ >= 2 } values %grp;
    print "groups of root .c files sharing >= $ENV{MIN} headers, named by what\n";
    print "their members most have in common:\n\n";
    for my $g (@groups) {
        my @m = sort @$g;
        my %c;                                 # signature = commonest headers
        for my $f (@m) { $c{$_}++ for grep { $dist{$_} } keys %{$inc{$f}}; }
        my @sig = grep { defined }
            (sort { $c{$b} <=> $c{$a} or $a cmp $b } grep { $c{$_} >= 2 } keys %c)[0 .. 2];
        printf "== %s (%d) ==\n   %s\n\n", join(" ", @sig), scalar @m, join(" ", @m);
    }
'
