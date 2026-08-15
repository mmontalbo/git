"""organize core: records, seams, orchestration, registry.

Enforce a declared layout through three seams: a Signal labels files,
a Policy maps a label to a target, an Enforcer owns membership and the
move cascade. This module orchestrates records and formats output.

Contract of this module (the purity invariant):
- It runs no subprocess and imports no build or version-control tool.
- It names no build system, language, or attribute file. Every such
  string lives in an adapter.
- FileId and DirId are opaque handles. This module never takes a
  destination, a parent directory, or an existence check off one. Any
  such question routes through the Enforcer.
"""
import sys
from dataclasses import dataclass, field
from typing import NewType, Optional, Protocol

# Opaque handles. This module never interprets their text.
FileId = NewType("FileId", str)
DirId = NewType("DirId", str)
Label = str


@dataclass(frozen=True)
class Vote:
    """A Signal's opinion on one file, as a distribution over labels.

    dist is nonempty when the file is labelled. primary is the argmax
    label or None for no opinion. confidence is dist[primary] or 0.0.
    status is a short adapter word (labelled, thin, ambiguous)."""
    dist: dict
    primary: Optional[Label]
    confidence: float
    status: str


@dataclass(frozen=True)
class Placement:
    """A Policy's decision for one file. target None means no opinion,
    so the core leaves the file in place. reason is UX prose naming
    which input won (override, label, name)."""
    target: Optional[DirId]
    label: Optional[Label]
    reason: str


@dataclass(frozen=True)
class Verdict:
    """A yes or no with adapter prose when no."""
    ok: bool
    reason: str


@dataclass(frozen=True)
class Step:
    """One cascade action as generic metadata plus an opaque payload.

    kind is move, edit, gate, or stage. Only the Enforcer interprets
    payload; the core prints kind, summary, reads, writes, preview."""
    kind: str
    summary: str
    reads: tuple
    writes: tuple
    preview: Optional[str]
    payload: object


@dataclass(frozen=True)
class Plan:
    """One area's carve. moves are (src, dst) handle pairs. skipped are
    (file, reason). steps are the opaque cascade. notes are display
    lines the core prints verbatim (counts, stale references)."""
    target: DirId
    moves: tuple
    skipped: tuple
    steps: tuple
    notes: tuple = ()
    kept_public: tuple = ()


@dataclass
class ApplyResult:
    """The outcome of an apply: ok plus lines the core prints."""
    ok: bool
    lines: list = field(default_factory=list)


class Signal(Protocol):
    def label(self, scope):
        """scope: set[FileId] -> dict[FileId, Vote]. Files with no
        evidence are absent."""


class Policy(Protocol):
    def load(self, mapref):
        """Load the area map named by mapref, an adapter string."""

    def name(self):
        """A short label for the loaded map, for output only."""

    def overrides(self, scope):
        """scope: set[FileId] -> dict[FileId, Label] declared labels."""

    def place(self, f, vote, override):
        """(FileId, Vote|None, Label|None) -> Placement. A declared
        override beats the inferred vote."""

    def target_of(self, label):
        """Label -> DirId or None."""

    def ordered_targets(self):
        """The map's targets in declared order, for stable output."""


class Enforcer(Protocol):
    def scope(self):
        """The set[FileId] this adapter governs (build membership)."""

    def target_ready(self, target):
        """Whether target exists and can receive files."""

    def already_placed(self, f, target):
        """Whether file f already sits in target. The core must not
        answer this itself; it is path arithmetic on a handle."""

    def preflight(self):
        """Blocking reasons (list[str]); empty means clear to apply."""

    def plan(self, target, files):
        """(DirId, set[FileId]) -> Plan. files are the scope handles
        the Policy placed into target. Folds pairing, relocatability,
        and the include and build edits into one Plan. Drops files
        already in target."""

    def apply(self, plan, commit):
        """Run the cascade, gate on the build, roll back on failure.
        Returns ApplyResult. Commits only when commit is true."""

    def recover_hint(self):
        """The one string that restores the pre-apply tree."""


# Registry of named triples. An adapter registers itself on import.
_TRIPLES = {}


def register(name, make):
    """Register a factory returning (Signal, Policy, Enforcer)."""
    _TRIPLES[name] = make


def get_triple(name):
    make = _TRIPLES.get(name)
    if not make:
        sys.exit(f"organize: no such triple '{name}'")
    return make()


def _place_all(signal, policy, enforcer):
    """scope through the three seams to {FileId: Placement}.

    Keeps every file the Signal or an override gave a label, so the
    labelled count is stable even when a label maps to no target.
    classify later drops the target-less placements."""
    scope = enforcer.scope()
    votes = signal.label(scope)
    over = policy.overrides(scope)
    placed = {}
    for f in scope:
        vote = votes.get(f)
        ov = over.get(f)
        if (vote is None or vote.primary is None) and ov is None:
            continue
        placed[f] = policy.place(f, vote, ov)
    return placed


def _files_for(signal, policy, enforcer, target):
    """The set of scope files the Policy placed into target. The
    Enforcer decides which of these actually move (pairing,
    relocatability, already-there)."""
    placed = _place_all(signal, policy, enforcer)
    return {f for f, p in placed.items() if p.target == target}


def classify(policy, enforcer, placed):
    """Split placed files into (misfiled, tocreate).

    misfiled: [(file, label, target)] whose target is ready and that
    are not already there. tocreate: {target: [files]} whose target is
    not ready. A target-less placement (no map opinion) is dropped.
    Both the readiness and the already-there test route through the
    Enforcer, so the core does no path arithmetic."""
    misfiled, tocreate = [], {}
    for f in sorted(placed):
        p = placed[f]
        target = p.target
        if target is None or enforcer.already_placed(f, target):
            continue
        if enforcer.target_ready(target):
            misfiled.append((f, p.label, target))
        else:
            tocreate.setdefault(target, []).append(f)
    return misfiled, tocreate


def cmd_check(signal, policy, enforcer, mapref):
    policy.load(mapref)
    placed = _place_all(signal, policy, enforcer)
    misfiled, tocreate = classify(policy, enforcer, placed)
    print(f"checked {len(placed)} labelled root files against "
          f"{policy.name()}\n")
    if misfiled:
        print(f"misfiled: {len(misfiled)} files whose area directory "
              "already exists:")
        for f, label, target in misfiled:
            print(f"  {f}  ->  {target}/   (labelled '{label}:')")
        print()
    if tocreate:
        n = sum(len(v) for v in tocreate.values())
        print(f"would create: {n} files map to {len(tocreate)} "
              "directories that do not exist yet:")
        for target in policy.ordered_targets():
            if target in tocreate:
                print(f"  {target}/   {len(tocreate[target])} files")
    if not misfiled and not tocreate:
        print("layout is clean: every labelled file is in its "
              "area's dir.")


def cmd_plan(signal, policy, enforcer, mapref, area):
    policy.load(mapref)
    if not area:
        placed = _place_all(signal, policy, enforcer)
        misfiled, tocreate = classify(policy, enforcer, placed)
        by_dir = {}
        for _, _, t in misfiled:
            by_dir[t] = by_dir.get(t, 0) + 1
        for t, fs in tocreate.items():
            by_dir[t] = by_dir.get(t, 0) + len(fs)
        print("plan summary (candidate root files before pairing; "
              "pass --area X\nfor the exact move set):\n")
        for t in policy.ordered_targets():
            if by_dir.get(t):
                print(f"  {t}/   {by_dir[t]} files")
        return
    target = policy.target_of(area)
    if not target:
        sys.exit(f"organize plan: unknown area '{area}'")
    plan = enforcer.plan(target, _files_for(signal, policy,
                                            enforcer, target))
    print(f"plan for area {area}  (dir {plan.target}/)\n")
    print(f"moves: {len(plan.moves)} files")
    for step in plan.steps:
        if step.kind == "move":
            print(step.summary)
    for line in plan.notes:
        print(line)
    if plan.skipped:
        print("\nskipped:")
        for f, reason in plan.skipped:
            print(f"  {f}  ({reason})")


def cmd_apply(signal, policy, enforcer, mapref, area, commit):
    if not area:
        sys.exit("organize apply: --area X is required")
    policy.load(mapref)
    target = policy.target_of(area)
    if not target:
        sys.exit(f"organize apply: unknown area '{area}'")
    blockers = enforcer.preflight()
    if blockers:
        sys.exit("organize apply: " + "; ".join(blockers))
    if not enforcer.target_ready(target):
        sys.exit(f"organize apply: target dir {target}/ is absent")
    plan = enforcer.plan(target, _files_for(signal, policy,
                                            enforcer, target))
    if not plan.moves:
        print("organize apply: nothing to do")
        return
    result = enforcer.apply(plan, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def cmd_status(signal, policy, enforcer, mapref):
    """Scope the organize against the declared rules.

    Per subsystem, split the remaining candidate files into auto-ready,
    contested, and marked. Auto-ready means the rule would move the file
    as a pure rename and the cohesion evidence does not object, so it is
    trivially correct to apply; the pure-rename and validator gates
    confirm it. Contested means the rule and the cohesion evidence place
    the file in different subsystems, so apply --auto holds it for a
    human or an LM (organize markers gives each one's positions and the
    rule edit that resolves it). Marked means the rule cannot place the
    file mechanically (a program, or a per-object rule the move would
    strand). The contested split needs the cohesion triple; without it,
    contested is 0 and every movable file reads as auto-ready."""
    policy.load(mapref)
    contested = _agree_verdicts(mapref)[1] if "cohesion" in _TRIPLES \
        else set()
    rows, skips = [], []
    tot_auto = tot_cont = tot_marked = 0
    for t in policy.ordered_targets():
        files = _files_for(signal, policy, enforcer, t)
        if not files:
            continue
        plan = enforcer.plan(t, files)
        cont = sum(1 for s, _ in plan.moves if s in contested)
        auto, marked = len(plan.moves) - cont, len(plan.skipped)
        if not (auto or cont or marked):
            continue
        state = "exists" if enforcer.target_ready(t) else "new"
        rows.append((t, state, auto, cont, marked,
                     len(plan.kept_public)))
        for f, reason in plan.skipped:
            skips.append((t, f, reason))
        tot_auto += auto
        tot_cont += cont
        tot_marked += marked
    print(f"organize status against {policy.name()}\n")
    print(f"  {'subsystem':<11}{'dir':<7}{'auto':>5}{'contested':>10}"
          f"{'marked':>8}{'public':>8}")
    for t, state, auto, cont, marked, public in rows:
        print(f"  {t + '/':<11}{state:<7}{auto:>5}{cont:>10}{marked:>8}"
              f"{public:>8}")
    print(f"\nauto-ready (pure rename, cohesion does not object): "
          f"{tot_auto} files in {len(rows)} subsystems")
    print(f"contested (rule vs cohesion disagree, held for a decision): "
          f"{tot_cont} files")
    print(f"marked (cannot place mechanically): {tot_marked} files")
    if skips:
        print("\nmarked (non-relocatable):")
        for t, f, reason in skips:
            print(f"  {f}  ->  {t}/   blocked: {reason}")
    if "cohesion" not in _TRIPLES:
        print("\nnote: contested is 0 because the cohesion triple is "
              "not\navailable, so movable files all read as auto-ready.")
    else:
        print("\napply --auto carves the auto-ready and holds the "
              "contested;\nrun organize markers for each contested "
              "file's positions and\nthe rule edit that resolves it.")


def _agree_verdicts(mapref):
    """Cross-check the commit-label rule against the cohesion signal.

    The three-way merge of the layout: base is the current tree, the
    rule is one side, the cohesion evidence is the other. Returns
    (git-c policy, contested-set, agree-by-area, unverified-by-area,
    contested-list). A source is contested only when one different
    rule-area is the strict majority of its cohesion-cluster neighbors
    and outweighs its own; agreed when its own area leads or ties;
    unverified when cohesion has no opinion, or when the cluster is too
    scattered for any area to hold a majority. That majority test drops
    co-consumption noise: a loose cluster whose members spread across
    many unrelated areas names no majority, so it does not contest.
    Ties among the majority areas break by name, so verdicts are
    reproducible. Computed on sources; headers ride with their source.
    Needs the cohesion triple."""
    gs, gp, ge = get_triple("git-c")
    if "cohesion" not in _TRIPLES:
        sys.exit("organize: the cohesion triple is not registered "
                 "(needs research/lib-reorg on the path)")
    cs = get_triple("cohesion")[0]
    gp.load(mapref)
    placed = _place_all(gs, gp, ge)
    gc_area = {f: p.target for f, p in placed.items() if p.target}
    remaining = {f: t for f, t in gc_area.items()
                 if f.endswith(".c") and not ge.already_placed(f, t)}
    cluster = {f: v.primary for f, v in cs.label(ge.scope()).items()
               if v.primary}
    members = {}
    for f, c in cluster.items():
        members.setdefault(c, set()).add(f)
    agree, unver, contested = {}, {}, []
    for f in sorted(remaining):
        X = remaining[f]
        neigh = {}
        for g in members.get(cluster.get(f), set()):
            ga = gc_area.get(g)
            if g != f and ga:
                neigh[ga] = neigh.get(ga, 0) + 1
        if not neigh:
            unver[X] = unver.get(X, 0) + 1
            continue
        best = max(neigh.values())
        total = sum(neigh.values())
        if neigh.get(X, 0) == best:
            agree[X] = agree.get(X, 0) + 1
        elif best * 2 <= total:
            unver[X] = unver.get(X, 0) + 1        # no majority: scattered
        else:
            implied = min(a for a in neigh if neigh[a] == best)
            contested.append((f, X, implied, cluster.get(f)))
    return gp, {f for f, _, _, _ in contested}, agree, unver, contested


def cmd_agree(mapref):
    """Corroborate the commit-label rule with the cohesion signal:
    agree (safe to automate), contested (needs a decision), unverified
    (cohesion silent). See _agree_verdicts for the rule."""
    gp, _cset, agree, unver, contested = _agree_verdicts(mapref)
    print(f"organize agree: cohesion corroboration of {gp.name()}\n")
    print(f"  {'subsystem':<12}{'agree':>6}{'contested':>10}"
          f"{'unverified':>12}")
    for t in gp.ordered_targets():
        nc = sum(1 for _, x, _, _ in contested if x == t)
        if agree.get(t) or unver.get(t) or nc:
            print(f"  {t + '/':<12}{agree.get(t, 0):>6}{nc:>10}"
                  f"{unver.get(t, 0):>12}")
    print(f"\ncorroborated (both signals agree, safe to automate): "
          f"{sum(agree.values())}")
    print(f"contested (signals disagree, needs a decision): "
          f"{len(contested)}")
    print(f"unverified (cohesion silent, rule stands alone): "
          f"{sum(unver.values())}")
    if contested:
        print("\ncontested sources (rule -> cohesion suggests):")
        for f, X, implied, c in sorted(contested):
            print(f"  {f:<22} {X}/ -> groups with {implied}/ "
                  f"(cohesion cluster '{c}')")


def cmd_apply_auto(signal, policy, enforcer, mapref, commit):
    """Converge every uncontested subsystem in one gated pass.

    The organize's terraform-apply. Uncontested means the rule and the
    cohesion evidence do not disagree (agreed or cohesion-silent);
    contested sources are held at the root as markers for a human or an
    LM. All moves run as one operation, then the Enforcer gates them:
    the pure-rename check (a git primitive, domain-agnostic) and the
    validator (a domain plugin, the build for git's C sources). The
    batch is staged, or committed with --commit."""
    _gp, contested, _a, _u, _c = _agree_verdicts(mapref)
    policy.load(mapref)
    blockers = enforcer.preflight()
    if blockers:
        sys.exit("organize apply --auto: " + "; ".join(blockers))
    plans = []
    for t in policy.ordered_targets():
        files = {f for f in _files_for(signal, policy, enforcer, t)
                 if f not in contested}
        if not files:
            continue
        plan = enforcer.plan(t, files)
        if plan.moves:
            plans.append(plan)
    if not plans:
        print("organize apply --auto: nothing uncontested to carve")
        return
    n = sum(len(p.moves) for p in plans)
    print(f"converging {len(plans)} subsystems, {n} uncontested "
          f"files; holding {len(contested)} contested sources at "
          f"root\n")
    result = enforcer.apply_auto(plans, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def cmd_markers(signal, policy, enforcer, mapref):
    """Emit the worklist of files that need a human or an LM to decide.

    This is the flag-for-intervention artifact, the piece a converging
    apply cannot do on its own. Each marker carries both positions and
    the exact rule edit that records a decision, so the resolver has all
    the context and the loop stays declarative.

    Two kinds. A signal-conflict marker is a source the commit-label
    rule and the cohesion evidence place in different subsystems; the
    resolver keeps it where the rule put it, or records an override that
    moves it to where the evidence points. A non-relocatable marker is a
    source the rule would move but the Enforcer cannot place
    mechanically (a program, a per-object rule); it is normally left at
    the root."""
    gp, _cset, _a, _u, contested = _agree_verdicts(mapref)
    policy.load(mapref)
    skips = []
    for t in policy.ordered_targets():
        files = _files_for(signal, policy, enforcer, t)
        if not files:
            continue
        for f, reason in enforcer.plan(t, files).skipped:
            skips.append((f, t, reason))
    print(f"organize markers: {len(contested)} signal-conflict, "
          f"{len(skips)} non-relocatable\n")
    for f, X, implied, c in sorted(contested):
        tok = gp.token_for(implied)
        print(f"[signal-conflict] {f}")
        print(f"  rule says:      {X}/  (commit-subject label)")
        print(f"  evidence says:  {implied}/  (cohesion cluster "
              f"'{c}', majority)")
        print(f"  to decide:      is {f} a member of {X}/, or only "
              f"coupled to {implied}/?")
        print(f"  keep:           no action")
        print(f"  move:           add to .gitattributes: "
              f"/{f}  area={tok}")
        print()
    for f, X, reason in sorted(skips):
        print(f"[non-relocatable] {f}")
        print(f"  rule says:      {X}/")
        print(f"  blocker:        {reason}")
        print(f"  to decide:      leave at root, or restructure the "
              f"build so it can move")
        print(f"  leave:          no action (expected for this kind)")
        print()


def take_opt(args, name):
    """Pop '--name VALUE' from args, returning (value, args)."""
    if name not in args:
        return None, args
    i = args.index(name)
    return args[i + 1], args[:i] + args[i + 2:]


DOC = """organize: enforce a declared source layout.

Usage:
  organize check [--map FILE]        report files whose area does not
                                  match the directory its area maps to
  organize status [--map FILE]       scope the organize: per subsystem, how
                                  many remaining files are auto-ready
                                  versus marked for judgment
  organize agree [--map FILE]        corroborate the rule with the cohesion
                                  signal: agree (safe to automate),
                                  contested (needs a decision), unverified
  organize markers [--map FILE]      emit the worklist of files needing a
                                  human or LM decision, each with both
                                  positions and the rule edit to record it
  organize plan [--area X] [--map FILE]
                                  dry run: show one area's moves and its
                                  include, build, and pairing effects
  organize apply --area X [--commit]
                                  perform one area's carve, stop before
                                  commit unless --commit is given
  organize apply --auto [--commit]   converge every uncontested subsystem
                                  in one pure-rename-and-validator-gated
                                  pass; hold contested sources at root
"""


def main(default_triple, default_mapref):
    """Parse args, select a triple, dispatch. default_mapref is an
    adapter string the core passes through without interpreting."""
    args = sys.argv[1:]
    triple, args = take_opt(args, "--triple")
    mapref, args = take_opt(args, "--map")
    if mapref is None:
        mapref = default_mapref
    area, args = take_opt(args, "--area")
    commit = "--commit" in args
    auto = "--auto" in args
    args = [a for a in args if a not in ("--commit", "--auto")]
    signal, policy, enforcer = get_triple(triple or default_triple)
    cmd = args[0] if args else "check"
    if cmd == "check":
        cmd_check(signal, policy, enforcer, mapref)
    elif cmd == "status":
        cmd_status(signal, policy, enforcer, mapref)
    elif cmd == "agree":
        cmd_agree(mapref)
    elif cmd == "markers":
        cmd_markers(signal, policy, enforcer, mapref)
    elif cmd == "plan":
        cmd_plan(signal, policy, enforcer, mapref, area)
    elif cmd == "apply":
        if auto:
            cmd_apply_auto(signal, policy, enforcer, mapref, commit)
        else:
            cmd_apply(signal, policy, enforcer, mapref, area, commit)
    else:
        sys.exit(DOC)
