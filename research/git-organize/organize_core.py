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
    lines the core prints verbatim (counts, stale references).
    paired_headers is how many moves are headers riding with their
    source; the adapter counts it so the core does no path math."""
    target: DirId
    moves: tuple
    skipped: tuple
    steps: tuple
    notes: tuple = ()
    kept_public: tuple = ()
    paired_headers: int = 0


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

    def paired_internal_header(self, f):
        """The same-stem header that would co-move with source f, or
        None when there is none or it is a public header kept at root.
        The core prints this handle; the adapter owns the path check."""

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

# The launcher records why the optional cohesion import failed, so the
# status banner can name the cause. The core prints this string
# verbatim; it never composes an install path or module name itself.
_COHESION_ABSENT_REASON = ""


def note_cohesion_absent(reason):
    """The launcher calls this with an adapter string naming why the
    cohesion triple is unavailable, for the status banner."""
    global _COHESION_ABSENT_REASON
    _COHESION_ABSENT_REASON = reason


def _cohesion_absent_reason():
    """The launcher-supplied cause string, or a generic fallback."""
    return _COHESION_ABSENT_REASON or "the triple did not load."


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
    labelled count is stable even when a label maps to no target. A
    caller drops the target-less placements."""
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


def _resolve_target(policy, area):
    """The target directory for an area argument, which may be the
    directory name that status prints (index) or an area label token
    (read). Returns the directory or None."""
    if area in policy.ordered_targets():
        return area
    return policy.target_of(area)


def _override_notices(policy, enforcer):
    """Adapter-supplied lint lines for the declared overrides, with a
    header, or an empty list. The core prints the strings verbatim; the
    adapter owns detecting an unresolved value or a duplicate line and
    naming the valid areas."""
    if not hasattr(policy, "override_notices"):
        return []
    notices = policy.override_notices(enforcer.scope())
    if not notices:
        return []
    return ["", "override lint:"] + notices


def cmd_apply(signal, policy, enforcer, mapref, area, commit):
    if area is None:
        sys.exit("organize apply: --area X is required")
    policy.load(mapref)
    target = _resolve_target(policy, area)
    if not target:
        sys.exit(f"organize apply: unknown area '{area}'\n"
                 "known areas: " + ", ".join(policy.ordered_targets()))
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


def cmd_status(signal, policy, enforcer, mapref, area=None,
               by_area=False, conflicts=False, exit_code=False):
    """Scope the organize against the declared layout.

    The default groups the drift by state, the way git status groups by
    change type. renamed sums the sources apply would move (agreed plus
    declared-only); conflict sums the sources it cannot auto-apply
    (placement, the rule and the includes disagree, plus requirement,
    the move would break the build); untracked counts root .c that no
    rule places; clean counts interface headers the carve keeps at root.
    The renamed count reconciles with the include cross-check, whose
    sources total is agreed + declared-only + placement. With --by-area,
    print the same numbers as a per-subsystem table; with --conflicts,
    print the conflict worklist; with --exit-code, exit 0 on a clean
    layout and 1 otherwise, for CI. With an area (a directory name or an
    area token), show that subsystem's exact move set, flagging the
    conflicts; this is the dry run that plan used to print. The placement
    split needs the include triple; without it placement reads 0 and
    every source reads as declared-only."""
    if conflicts:
        _status_conflicts(signal, policy, enforcer, mapref)
        return
    policy.load(mapref)
    have_cohesion = "cohesion" in _TRIPLES
    if not have_cohesion:
        why = _cohesion_absent_reason()
        print("organize status: the include triple is not registered, "
              "so the\nagreed/placement split is unavailable; every "
              "source reads\nas declared-only. " + why + "\n")
    contested, implied = set(), {}
    agree, unver, clist = {}, {}, []
    if have_cohesion:
        _gp, contested, agree, unver, clist = _agree_verdicts(mapref)
        implied = {f: imp for f, _x, imp, _c in clist}
    if area is not None:
        _status_area(signal, policy, enforcer, area, contested, implied)
        return
    cont_by = {}
    for _f, X, _imp, _c in clist:
        cont_by[X] = cont_by.get(X, 0) + 1
    rows = []
    tot = {"agreed": 0, "declared": 0, "placement": 0, "requirement": 0,
           "clean": 0, "moves": 0, "headers": 0}
    for t in policy.ordered_targets():
        files = _files_for(signal, policy, enforcer, t)
        if not files:
            continue
        plan = enforcer.plan(t, files)
        agreed = agree.get(t, 0)
        declared = unver.get(t, 0)
        placement = cont_by.get(t, 0)
        requirement = len(plan.skipped)
        clean = len(plan.kept_public)
        headers = plan.paired_headers
        if not (agreed or declared or placement or requirement
                or plan.moves):
            continue
        state = "exists" if enforcer.target_ready(t) else "new"
        rows.append((t, state, agreed, declared, placement,
                     requirement, clean))
        tot["agreed"] += agreed
        tot["declared"] += declared
        tot["placement"] += placement
        tot["requirement"] += requirement
        tot["clean"] += clean
        tot["moves"] += len(plan.moves)
        tot["headers"] += headers
    if by_area:
        _status_by_area(policy, rows)
        for line in _override_notices(policy, enforcer):
            print(line)                # first notice prints its header
        return
    place_all = _place_all(signal, policy, enforcer)
    scope_c = {f for f in enforcer.scope() if f.endswith(".c")}
    placed_c = {f for f in place_all if f.endswith(".c")}
    untracked = len(scope_c - placed_c)
    renamed = tot["agreed"] + tot["declared"]
    conflict = tot["placement"] + tot["requirement"]
    clean_layout = renamed == 0 and conflict == 0
    banner = "(layout clean)" if clean_layout else "(layout not clean)"
    print(f"organize status against {policy.name()}   {banner}\n")
    print(f"renamed    {renamed:<5} will move on apply  (agreed "
          f"{tot['agreed']}, declared-only {tot['declared']})")
    print(f"conflict   {conflict:<5} cannot auto-apply:")
    print(f"             placement    {tot['placement']:<5} rule vs "
          "the includes disagree on the area")
    print(f"             requirement  {tot['requirement']:<5} move "
          "would break the build (program, per-object)")
    print(f"untracked  {untracked:<5} no rule places these")
    print(f"clean      {tot['clean']:<5} interface headers kept at "
          "root")
    print(f"\n{renamed} renamed = {tot['agreed']} agreed + "
          f"{tot['declared']} declared-only; apply --unconflicted\n"
          f"moves those plus {tot['headers']} internal headers and "
          f"holds the {conflict} conflicts.")
    print("\norganize status --conflicts lists each conflict; "
          "--by-area splits\nthis per subsystem; status <area> shows "
          "one subsystem's moves.")
    for line in _override_notices(policy, enforcer):
        # first notice line prints its own header
        print(line)
    if exit_code:
        sys.exit(0 if clean_layout else 1)


def _status_by_area(policy, rows):
    """Print the per-subsystem drift table: the same quantities the
    default groups by state, laid out by area."""
    print(f"organize status against {policy.name()}\n")
    print(f"  {'subsystem':<11}{'dir':<7}{'agreed':>7}"
          f"{'declared-only':>14}{'placement':>10}"
          f"{'requirement':>12}{'clean':>7}")
    for t, state, agreed, declared, placement, requirement, clean \
            in rows:
        print(f"  {t + '/':<11}{state:<7}{agreed:>7}{declared:>14}"
              f"{placement:>10}{requirement:>12}{clean:>7}")


def _status_area(signal, policy, enforcer, area, contested, implied):
    """Show one subsystem's exact move set, the dry run plan used to
    print, now flagging which files apply --unconflicted would hold."""
    target = _resolve_target(policy, area)
    if not target:
        sys.exit(f"organize status: unknown area '{area}'\n"
                 "known areas: " + ", ".join(policy.ordered_targets()))
    plan = enforcer.plan(target, _files_for(signal, policy, enforcer,
                                            target))
    nc = sum(1 for s, _ in plan.moves if s in contested)
    na = len(plan.moves) - nc
    print(f"organize status for {target}/  ({na} auto-ready, {nc} "
          f"conflict (placement))\n")
    print(f"moves: {len(plan.moves)} files")
    for src, dst in plan.moves:
        tag = ("   * conflict (placement): the includes say "
               f"{implied.get(src, '?')}/") if src in contested else ""
        print(f"  {src:<22}  ->  {dst}{tag}")
    for line in plan.notes:
        print(line)
    if plan.skipped:
        print("\nconflict (requirement):")
        for f, reason in plan.skipped:
            print(f"  {f}  ({reason})")
    if nc:
        print(f"\napply --unconflicted moves the {na} auto-ready and "
              f"holds the {nc} conflict (placement);\napply --area "
              f"{area} would move all {len(plan.moves)}. See organize "
              "status\n--conflicts for the conflict decisions.")


def _agree_verdicts(mapref):
    """Cross-check the commit-label rule against the include signal.

    The three-way merge of the layout: base is the current tree, the
    rule is one side, the include evidence is the other. Returns
    (git-c policy, conflict-set, agreed-by-area, declared-only-by-area,
    conflict-list). A source is a placement conflict only when one
    different rule-area is the strict majority of its include-cluster
    neighbors and outweighs its own; agreed when its own area leads or
    ties; declared-only when the includes have no opinion, or when the
    cluster is too scattered for any area to hold a majority. That
    majority test drops co-consumption noise: a loose cluster whose
    members spread across many unrelated areas names no majority, so it
    does not conflict. Ties among the majority areas break by name, so
    verdicts are reproducible. Computed on sources; headers ride with
    their source. Needs the include triple."""
    gs, gp, ge = get_triple("git-c")
    if "cohesion" not in _TRIPLES:
        sys.exit("organize: the include triple is not registered "
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


def cmd_apply_auto(signal, policy, enforcer, mapref, commit):
    """Converge every unconflicted subsystem in one gated pass.

    The organize's terraform-apply. Unconflicted means the rule and the
    include evidence do not disagree (agreed or include-silent);
    conflict sources are held at the root as a todo for a human or an
    LM. All moves run as one operation, then the Enforcer gates them:
    the pure-rename check (a git primitive, domain-agnostic) and the
    validator (a domain plugin, the build for git's C sources). The
    batch is staged, or committed with --commit."""
    _gp, contested, _a, _u, _c = _agree_verdicts(mapref)
    policy.load(mapref)
    blockers = enforcer.preflight()
    if blockers:
        sys.exit("organize apply --unconflicted: " + "; ".join(blockers))
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
        print("organize apply --unconflicted: nothing unconflicted "
              "to carve")
        return
    n = sum(len(p.moves) for p in plans)
    print(f"converging {len(plans)} subsystems, {n} unconflicted "
          f"files; holding {len(contested)} conflict sources at "
          f"root\n")
    result = enforcer.apply_auto(plans, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def _status_conflicts(signal, policy, enforcer, mapref):
    """Emit the worklist of conflicts that need a human or an LM.

    This is the flag-for-intervention artifact, the piece a converging
    apply cannot do on its own. Each entry carries both positions and
    the exact rule edit that records a decision, so the resolver has all
    the context and the loop stays declarative.

    Two reasons. A conflict (placement) entry is a source the
    commit-label rule and the include evidence place in different
    subsystems; the resolver keeps it where the rule put it, or records
    an override that moves it to where the evidence points. A conflict
    (requirement) entry is a source the rule would move but the Enforcer
    cannot place mechanically (a program, a per-object rule); it is
    normally left at the root."""
    gp, _cset, _a, _u, contested = _agree_verdicts(mapref)
    policy.load(mapref)
    skips = []
    for t in policy.ordered_targets():
        files = _files_for(signal, policy, enforcer, t)
        if not files:
            continue
        for f, reason in enforcer.plan(t, files).skipped:
            skips.append((f, t, reason))
    print(f"organize status --conflicts: {len(contested)} conflict "
          f"(placement), {len(skips)} conflict (requirement)\n")
    for f, X, implied, c in sorted(contested):
        tok = gp.token_for(implied)
        equiv = "" if tok == implied else f" (the token for {implied}/)"
        header = enforcer.paired_internal_header(f)
        print(f"[conflict (placement)] {f}")
        print(f"  rule says:      {X}/  (commit-subject label)")
        print(f"  includes say:   {implied}/  (include cluster "
              f"'{c}', majority)")
        print(f"  to decide:      is {f} a member of {X}/, or only "
              f"coupled to {implied}/?")
        print(f"  keep:           no action")
        print(f"  move:           add to .gitattributes: "
              f"/{f}  area={tok}{equiv}")
        if header:
            print(f"                  and its internal header: "
                  f"/{header}  area={tok}")
        print()
    for f, X, reason in sorted(skips):
        print(f"[conflict (requirement)] {f}")
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
  organize status [AREA] [--by-area] [--conflicts] [--exit-code]
                  [--map FILE]
                                  the orientation view, and the default
                                  with no command. With no area, group
                                  the drift by state: renamed, conflict
                                  (placement and requirement), untracked,
                                  and clean; --by-area lays the same
                                  numbers out per subsystem; --conflicts
                                  lists each conflict with the rule edit
                                  to record a decision; --exit-code exits
                                  0 on a clean layout and 1 otherwise, for
                                  CI; with an area (a directory name or an
                                  area token), that subsystem's exact move
                                  set with conflicts flagged (the dry run
                                  plan used to give)
  organize apply --area X [--commit]
                                  perform one area's carve, stop before
                                  commit unless --commit is given
  organize apply --unconflicted [--commit]
                                  converge every unconflicted subsystem
                                  in one pure-rename-and-validator-gated
                                  pass; hold conflict sources at root
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
    auto = "--unconflicted" in args
    by_area = "--by-area" in args
    conflicts = "--conflicts" in args
    exit_code = "--exit-code" in args
    args = [a for a in args
            if a not in ("--commit", "--unconflicted", "--by-area",
                         "--conflicts", "--exit-code")]
    if args and args[0] in ("--help", "-h", "help"):
        print(DOC)                     # explicit help exits 0
        return
    signal, policy, enforcer = get_triple(triple or default_triple)
    cmd = args[0] if args else "status"
    if area is None and cmd == "status" and len(args) > 1:
        area = args[1]                 # organize status <area>
    if cmd == "status":
        cmd_status(signal, policy, enforcer, mapref, area, by_area,
                   conflicts, exit_code)
    elif cmd == "apply":
        if auto:
            cmd_apply_auto(signal, policy, enforcer, mapref, commit)
        else:
            cmd_apply(signal, policy, enforcer, mapref, area, commit)
    elif cmd == "check":
        sys.exit("organize check is now organize status --exit-code")
    elif cmd == "agree":
        sys.exit("organize agree is now organize status --by-area and "
                 "organize status --conflicts")
    elif cmd == "todo":
        sys.exit("organize todo is now organize status --conflicts")
    elif cmd == "plan":
        sys.exit("organize plan is now organize status <area>")
    else:
        sys.exit(DOC)                  # unknown command: nonzero + DOC
