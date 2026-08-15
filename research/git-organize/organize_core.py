"""organize core: records, seams, orchestration, registry.

Enforce a declared layout through three seams: a Signal labels files,
a Policy maps a label to a target, an Transformer owns membership and the
move cascade. This module orchestrates records and formats output.

Contract of this module (the purity invariant):
- It runs no subprocess and imports no build or version-control tool.
- It names no build system, language, or attribute file. Every such
  string lives in an adapter.
- FileId and DirId are opaque handles. This module never takes a
  destination, a parent directory, or an existence check off one. Any
  such question routes through the Transformer.
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

    kind is move, edit, gate, or stage. Only the Transformer interprets
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


class Transformer(Protocol):
    def scope(self):
        """The set[FileId] this adapter governs (build membership)."""

    def target_ready(self, target):
        """Whether target exists and can receive files."""

    def already_at(self, f, target):
        """Whether file f already sits in target. The core must not
        answer this itself; it is path arithmetic on a handle."""

    def is_source(self, f):
        """Whether f is a primary placement unit, not a rider. The core
        counts and cross-checks sources; a rider moves with its source.
        The adapter owns which files are sources; the core names no
        language."""

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


def register(name, make):
    """Register a factory returning (Signal, Policy, Transformer)."""
    _TRIPLES[name] = make


def get_triple(name):
    make = _TRIPLES.get(name)
    if not make:
        sys.exit(f"organize: no such triple '{name}'")
    return make()


def registered(name):
    """Whether a triple named name is registered. The launcher asks this
    to decide whether a second signal is available; the core names no
    triple itself."""
    return name in _TRIPLES


def _place_all(signal, policy, transformer):
    """scope through the three seams to {FileId: Placement}.

    Keeps every file the Signal or an override gave a label, so the
    labelled count is stable even when a label maps to no target. A
    caller drops the target-less placements."""
    scope = transformer.scope()
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


def _files_for(signal, policy, transformer, target):
    """The set of scope files the Policy placed into target. The
    Transformer decides which of these actually move (pairing,
    relocatability, already-there)."""
    placed = _place_all(signal, policy, transformer)
    return {f for f, p in placed.items() if p.target == target}


def _resolve_target(policy, area):
    """The target directory for an area argument, which may be the
    directory name that status prints (index) or an area label token
    (read). Returns the directory or None."""
    if area in policy.ordered_targets():
        return area
    return policy.target_of(area)


def _override_notices(policy, transformer):
    """Adapter-supplied lint lines for the declared overrides, with a
    header, or an empty list. The core prints the strings verbatim; the
    adapter owns detecting an unresolved value or a duplicate line and
    naming the valid areas."""
    if not hasattr(policy, "override_notices"):
        return []
    notices = policy.override_notices(transformer.scope())
    if not notices:
        return []
    return ["", "override lint:"] + notices


def cmd_apply(signal, policy, transformer, mapref, area, commit):
    if area is None:
        sys.exit("organize apply: --area X is required")
    policy.load(mapref)
    target = _resolve_target(policy, area)
    if not target:
        sys.exit(f"organize apply: unknown area '{area}'\n"
                 "known areas: " + ", ".join(policy.ordered_targets()))
    blockers = transformer.preflight()
    if blockers:
        sys.exit("organize apply: " + "; ".join(blockers))
    if not transformer.target_ready(target):
        sys.exit(f"organize apply: target dir {target}/ is absent")
    plan = transformer.plan(target, _files_for(signal, policy,
                                            transformer, target))
    if not plan.moves:
        print("organize apply: nothing to do")
        return
    result = transformer.apply(plan, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def cmd_status(signal, policy, transformer, mapref, area=None,
               by_area=False, conflicts=False, exit_code=False,
               group_signal=None, group_label="a second signal"):
    """Scope the organize against the declared layout.

    The default groups the drift by state, the way git status groups by
    change type, and every count comes from the Transformer's plans, not
    from the second signal. renamed is what apply --unconflicted moves
    (the plan over the non-conflicted files); conflict is placement (a
    second signal disagrees with the rule) plus requirement (the domain
    cannot move it); untracked is the sources no rule places; organized
    is the files the plan keeps in place. With --by-area, print the same
    counts per subsystem; with --conflicts, print the conflict worklist;
    with --exit-code, exit 0 when nothing renames or conflicts and 1
    otherwise, for CI. With an area, show that subsystem's exact move
    set, flagging the conflicts. Without a second signal there are no
    placement conflicts and renamed is the whole plan."""
    if conflicts:
        _status_conflicts(signal, policy, transformer, mapref,
                          group_signal, group_label)
        return
    policy.load(mapref)
    have_group = group_signal is not None
    if not have_group:
        print("organize status: no second signal is configured, so a\n"
              "rule-vs-evidence conflict is not detected; a file either\n"
              "moves or is held.\n")
    contested, implied = set(), {}
    agree, unver, clist = {}, {}, []
    if have_group:
        contested, agree, unver, clist = _agreement(
            signal, policy, transformer, group_signal, mapref)
        implied = {f: imp for f, _x, imp, _c in clist}
    if area is not None:
        _status_area(signal, policy, transformer, area, contested, implied,
                     group_label)
        return
    cont_by = {}
    for _f, X, _imp, _c in clist:
        cont_by[X] = cont_by.get(X, 0) + 1
    rows = []
    tot = {"renamed": 0, "placement": 0, "requirement": 0,
           "organized": 0}
    for t in policy.ordered_targets():
        files = _files_for(signal, policy, transformer, t)
        if not files:
            continue
        plan = transformer.plan(t, files)
        # renamed is what apply --unconflicted moves: the plan over the
        # non-conflicted files (the Transformer drops a held source's
        # riders too). The plan, not the second signal, is the source of
        # truth for what moves, so a domain with no second signal reads
        # correctly.
        safe = files - contested
        renamed = (len(plan.moves) if safe == files
                   else len(transformer.plan(t, safe).moves))
        placement = cont_by.get(t, 0)
        requirement = len(plan.skipped)
        organized = len(plan.kept_public)
        if not (renamed or placement or requirement or organized):
            continue
        state = "exists" if transformer.target_ready(t) else "new"
        rows.append((t, state, renamed, placement, requirement,
                     organized))
        tot["renamed"] += renamed
        tot["placement"] += placement
        tot["requirement"] += requirement
        tot["organized"] += organized
    if by_area:
        _status_by_area(policy, rows)
        for line in _override_notices(policy, transformer):
            print(line)                # first notice prints its header
        return
    place_all = _place_all(signal, policy, transformer)
    scope_src = {f for f in transformer.scope() if transformer.is_source(f)}
    placed_src = {f for f in place_all if transformer.is_source(f)}
    untracked = len(scope_src - placed_src)
    renamed = tot["renamed"]
    conflict = tot["placement"] + tot["requirement"]
    is_organized = renamed == 0 and conflict == 0
    banner = "(organized)" if is_organized else "(not organized)"
    print(f"organize status against {policy.name()}   {banner}\n")
    print(f"renamed    {renamed:<5} will move on apply")
    print(f"conflict   {conflict:<5} cannot auto-apply, needs a "
          "decision")
    print(f"untracked  {untracked:<5} no rule places these")
    print(f"organized  {tot['organized']:<5} already in place")
    print(f"\napply --unconflicted moves the {renamed} and holds the "
          f"{conflict} conflicts.\norganize status --conflicts lists "
          "them; --by-area splits per subsystem;\nstatus <area> shows "
          "one.")
    for line in _override_notices(policy, transformer):
        # first notice line prints its own header
        print(line)
    if exit_code:
        sys.exit(0 if is_organized else 1)


def _status_by_area(policy, rows):
    """Print the per-subsystem drift table: the same quantities the
    default groups by state, laid out by area."""
    print(f"organize status against {policy.name()}\n")
    print(f"  {'subsystem':<11}{'dir':<7}{'renamed':>8}"
          f"{'conflict':>9}{'organized':>10}")
    for t, state, renamed, placement, requirement, organized in rows:
        conflict = placement + requirement
        print(f"  {t + '/':<11}{state:<7}{renamed:>8}{conflict:>9}"
              f"{organized:>10}")


def _status_area(signal, policy, transformer, area, contested, implied,
                 group_label="a second signal"):
    """Show one subsystem's exact move set, the dry run plan used to
    print, now flagging which files apply --unconflicted would hold."""
    target = _resolve_target(policy, area)
    if not target:
        sys.exit(f"organize status: unknown area '{area}'\n"
                 "known areas: " + ", ".join(policy.ordered_targets()))
    plan = transformer.plan(target, _files_for(signal, policy, transformer,
                                            target))
    nc = sum(1 for s, _ in plan.moves if s in contested)
    na = len(plan.moves) - nc
    print(f"organize status for {target}/  ({na} will move, {nc} in "
          f"conflict)\n")
    print(f"moves: {len(plan.moves)} files")
    for src, dst in plan.moves:
        tag = (f"   * conflict: {group_label} say "
               f"{implied.get(src, '?')}/") if src in contested else ""
        print(f"  {src:<22}  ->  {dst}{tag}")
    for line in plan.notes:
        print(line)
    if plan.skipped:
        print("\ncannot move (held at root):")
        for f, reason in plan.skipped:
            print(f"  {f}  ({reason})")
    if nc:
        print(f"\napply --unconflicted moves the {na} and holds the "
              f"{nc} in conflict;\napply --area {area} would move all "
              f"{len(plan.moves)}. See organize status\n--conflicts for "
              "the decisions.")


def _agreement(signal, policy, transformer, group_signal, mapref):
    """Cross-check the rule placement against a second signal that groups
    files.

    The three-way merge of the layout: base is the current tree, the
    rule's placement is one side, the second signal's grouping is the
    other. group_signal labels each file with a group; a file's neighbors
    are the other files sharing its group. Returns (conflict-set,
    agreed-by-area, declared-only-by-area, conflict-list). A source is a
    placement conflict only when one different rule-area is the strict
    majority of its group neighbors and outweighs its own; agreed when
    its own area leads or ties; declared-only when the group has no
    opinion, or when the group is too scattered for any area to hold a
    majority. That majority test drops co-membership noise: a loose group
    whose members spread across many unrelated areas names no majority,
    so it does not conflict. Ties among the majority areas break by name,
    so verdicts are reproducible. Computed on sources; a paired file
    rides with its source. Absent a second signal, returns empties."""
    if group_signal is None:
        return set(), {}, {}, []
    policy.load(mapref)
    placed = _place_all(signal, policy, transformer)
    rule_area = {f: p.target for f, p in placed.items() if p.target}
    remaining = {f: t for f, t in rule_area.items()
                 if transformer.is_source(f)
                 and not transformer.already_at(f, t)}
    group = {f: v.primary
             for f, v in group_signal.label(transformer.scope()).items()
             if v.primary}
    members = {}
    for f, c in group.items():
        members.setdefault(c, set()).add(f)
    agree, unver, contested = {}, {}, []
    for f in sorted(remaining):
        X = remaining[f]
        neigh = {}
        for g in members.get(group.get(f), set()):
            ga = rule_area.get(g)
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
            contested.append((f, X, implied, group.get(f)))
    return {f for f, _, _, _ in contested}, agree, unver, contested


def cmd_apply_auto(signal, policy, transformer, mapref, commit,
                   group_signal=None):
    """Converge every unconflicted subsystem in one gated pass.

    The organize's terraform-apply. Unconflicted means the rule and the
    second signal do not disagree (agreed or the second signal is
    silent); conflict sources are held at the root as a todo for a human
    or an LM. All moves run as one operation, then the Transformer gates
    them: the pure-rename check (a git primitive, domain-agnostic) and
    the validator (a domain plugin, the build for a C tree). The batch is
    staged, or committed with --commit."""
    contested = _agreement(signal, policy, transformer,
                           group_signal, mapref)[0]
    policy.load(mapref)
    blockers = transformer.preflight()
    if blockers:
        sys.exit("organize apply --unconflicted: " + "; ".join(blockers))
    plans = []
    for t in policy.ordered_targets():
        files = {f for f in _files_for(signal, policy, transformer, t)
                 if f not in contested}
        if not files:
            continue
        plan = transformer.plan(t, files)
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
    result = transformer.apply_auto(plans, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def _status_conflicts(signal, policy, transformer, mapref,
                      group_signal=None, group_label="a second signal"):
    """Emit the worklist of conflicts that need a human or an LM.

    This is the flag-for-intervention artifact, the piece a converging
    apply cannot do on its own. Each entry carries both positions and
    the exact rule edit that records a decision, so the resolver has all
    the context and the loop stays declarative.

    Two reasons. A conflict (placement) entry is a source the rule and
    the second signal place in different subsystems; the resolver keeps
    it where the rule put it, or records an override that moves it to
    where the second signal points. A conflict (requirement) entry is a
    source the rule would move but the Transformer cannot place mechanically
    (a program, a per-object rule); it is normally left at the root."""
    _cset, _a, _u, contested = _agreement(
        signal, policy, transformer, group_signal, mapref)
    policy.load(mapref)
    skips = []
    for t in policy.ordered_targets():
        files = _files_for(signal, policy, transformer, t)
        if not files:
            continue
        for f, reason in transformer.plan(t, files).skipped:
            skips.append((f, t, reason))
    print(f"organize status --conflicts: {len(contested) + len(skips)} "
          f"conflicts\n({len(contested)} rule vs evidence, {len(skips)} "
          "cannot move)\n")
    for f, X, implied, c in sorted(contested):
        tok = policy.token_for(implied)
        equiv = "" if tok == implied else f" (the token for {implied}/)"
        header = transformer.paired_internal_header(f)
        print(f"[conflict] {f}")
        print(f"  rule says:      {X}/  (commit-subject label)")
        print(f"  {group_label}:   {implied}/  (group "
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
        print(f"[conflict] {f}")
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


def main(default_triple, default_mapref, group_signal=None,
         group_label="a second signal"):
    """Parse args, select a triple, dispatch. default_mapref is an
    adapter string the core passes through without interpreting.
    group_signal is the optional second signal for the cross-check, and
    group_label is the launcher-supplied word the summary uses to name
    it; the core names no signal itself."""
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
    signal, policy, transformer = get_triple(triple or default_triple)
    cmd = args[0] if args else "status"
    if area is None and cmd == "status" and len(args) > 1:
        area = args[1]                 # organize status <area>
    if cmd == "status":
        cmd_status(signal, policy, transformer, mapref, area, by_area,
                   conflicts, exit_code, group_signal, group_label)
    elif cmd == "apply":
        if auto:
            cmd_apply_auto(signal, policy, transformer, mapref, commit,
                           group_signal)
        else:
            cmd_apply(signal, policy, transformer, mapref, area, commit)
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
