"""organize cohesion adapter: a Signal and Policy from include cohesion.

A second component that reuses the git Enforcer unchanged. The Signal
labels each root .c file by the include-cohesion cluster it lands in;
the Policy treats a cluster name as its own target directory. This
proves the Signal and Policy seams accept a different label vocabulary
(cluster names, not git area tokens) over the same enforcer cascade.

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
from organize_core import Vote, Placement
from organize_git import MakeMesonEnforcer

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


class CohesionSignal:
    """Label each root .c file by the include-cohesion cluster it lands
    in. Headers get no vote; the enforcer pairs a header when its
    source moves, exactly as with the git signal. The label vocabulary
    is cluster names, a file stem, not the git area tokens."""

    def label(self, scope):
        inc = cohlib.distinctive(cohlib.read_includes())
        votes = {}
        for name, members, coh in _cluster(inc):
            conf = min(1.0, coh / BAND)
            for f in members:
                if f in scope:
                    votes[f] = Vote(dist={name: 1.0}, primary=name,
                                    confidence=conf,
                                    status="labelled")
        return votes


class ClusterPolicy:
    """The discovered cluster name is the proposed directory, so this
    policy is close to identity. It declares no map and no overrides;
    the clusters are the targets."""

    def __init__(self):
        self._targets = []           # cluster names seen in place()

    def load(self, mapref):
        return None                  # the clusters are the map

    def name(self):
        return "cohesion clusters"

    def overrides(self, scope):
        return {}

    def place(self, f, vote, override):
        if vote is None or vote.primary is None:
            return Placement(target=None, label=None, reason="none")
        if vote.primary not in self._targets:
            self._targets.append(vote.primary)
        return Placement(target=vote.primary, label=vote.primary,
                         reason="cohesion")

    def target_of(self, label):
        return label                 # the cluster name is the directory

    def ordered_targets(self):
        """Cluster names for stable output. place() sees files in an
        unordered scope, so sort by name for a deterministic order."""
        return sorted(set(self._targets))


def _make_cohesion():
    return (CohesionSignal(), ClusterPolicy(), MakeMesonEnforcer())


organize_core.register("cohesion", _make_cohesion)
