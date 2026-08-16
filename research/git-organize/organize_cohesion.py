"""organize cohesion adapter: an Interpreter from include cohesion.

A second component that reuses the git Transformer unchanged. The
interpreter labels each root .c file by the include-cohesion cluster it
lands in and treats a cluster name as its own target directory. This
proves the Interpreter seam accepts a different label vocabulary
(cluster names, not git area tokens) over the same transformer cascade.
It runs as a primary interpreter under --triple cohesion; it is no
longer a cross-check.

The cohesion measure is reused from research/lib-reorg/cohlib.py by
import. The agglomerative clustering is ported here as a compact
function because agglom.py is a script. Honest limit (see lib-reorg
FINDINGS.md): cohesion sees include coupling, not purpose, and is
blind to core hubs, so it proposes, it does not decide.
"""
import os
import sys
from collections import defaultdict
from itertools import combinations

import organize_core
from organize_core import Desire
from organize_git import GitTransformer

_LIBREORG = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "lib-reorg")
if _LIBREORG not in sys.path:
    sys.path.insert(0, _LIBREORG)

import cohlib                                  # noqa: E402

# Cut the dendrogram where cohesion first enters the carved band, the
# score the already-carved directories occupy. A cluster keeps at
# least this many files to count as a directory-scale group.
BAND = 3.0
MIN_FILES = 3


def _cluster(inc):
    """Agglomerate root .c files by shared distinctive includes, cut
    at BAND, keep clusters of at least MIN_FILES.

    Ported from research/lib-reorg/agglom.py. Average linkage: at each
    step merge the pair with the greatest summed pair similarity per
    file. Returns a list of (name, members, cohesion) for the maximal
    clusters whose cohesion is at least BAND, name being the cluster's
    most-connected file stem."""
    files = sorted(f for f in inc if "/" not in f and inc[f])

    def sim(a, b):
        return len(inc[a] & inc[b])

    def coh(members):
        return cohlib.cohesion(inc, members) or 0.0

    mem = {i: [f] for i, f in enumerate(files)}
    size = {i: 1 for i in mem}
    kids, cohv = {}, {}
    link = defaultdict(dict)          # link[i][j] = summed pair sims
    for a, b in combinations(range(len(files)), 2):
        s = sim(files[a], files[b])
        if s:
            link[a][b] = s
            link[b][a] = s
    act = set(mem)
    nid = len(files)
    while True:
        best, bv = None, 0.0
        for i in act:
            for j, ss in link[i].items():
                if i < j and ss / (size[i] * size[j]) > bv:
                    bv, best = ss / (size[i] * size[j]), (i, j)
        if not best:
            break
        i, j = best
        c = nid
        nid += 1
        mem[c] = mem[i] + mem[j]
        size[c] = size[i] + size[j]
        kids[c] = (i, j)
        cohv[c] = coh(mem[c])
        for x in (set(link[i]) | set(link[j])) - {i, j}:
            link[c][x] = link[i].get(x, 0) + link[j].get(x, 0)
            link[x][c] = link[c][x]
        for x in list(link[i]):
            link[x].pop(i, None)
        for x in list(link[j]):
            link[x].pop(j, None)
        link.pop(i, None)
        link.pop(j, None)
        act.discard(i)
        act.discard(j)
        act.add(c)

    def cut(node):                    # maximal sub-clusters at BAND
        if size[node] < 2:
            return []
        if cohv.get(node, 0.0) >= BAND:
            return [node]
        if node not in kids:
            return []
        a, b = kids[node]
        return cut(a) + cut(b)

    def name(members):
        return max(members, key=lambda f:
                   sum(sim(f, o) for o in members if o != f))[:-2]

    groups = [g for r in act for g in cut(r) if size[g] >= MIN_FILES]
    groups.sort(key=lambda g: -size[g])
    return [(name(mem[g]), mem[g], cohv.get(g, 0.0)) for g in groups]


class CohesionInterpreter:
    """State each root .c file's Desire from the include-cohesion cluster
    it lands in; the discovered cluster name is the proposed directory,
    so the placement is close to identity.

    Headers get no vote; the transformer pairs a header when its source
    moves, exactly as with the git interpreter. The label vocabulary is
    cluster names (a file stem), not the git area tokens. It declares no
    map and no overrides; the clusters are the targets."""

    def __init__(self):
        self._clusters = None        # cached [(name, members, coh)]

    def _cluster_list(self):
        """The cohesion clusters, computed once and cached. Independent
        of scope, so ordered_targets is stable before desires runs."""
        if self._clusters is None:
            inc = cohlib.distinctive(cohlib.read_includes())
            self._clusters = _cluster(inc)
        return self._clusters

    def desires(self, scope):
        """{FileId: Desire} for each root .c file the cohesion cluster
        labels; place is the cluster name, hold stays None."""
        out = {}
        for name, members, coh in self._cluster_list():
            for f in members:
                if f in scope:
                    out[f] = Desire(place=name, hold=None)
        return out

    def load(self, mapref):
        return None                  # the clusters are the map

    def name(self):
        return "cohesion clusters"

    def target_of(self, label):
        return label                 # the cluster name is the directory

    def ordered_targets(self):
        """Cluster names for stable output, sorted by name for a
        deterministic order. Derived from the cached cluster list, so it
        is populated before desires runs."""
        return sorted({name for name, _m, _c in self._cluster_list()})


def _make_cohesion():
    return (CohesionInterpreter(), GitTransformer())


organize_core.register("cohesion", _make_cohesion)
