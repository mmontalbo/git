"""organize core: records, seams, orchestration, registry.

Enforce a declared layout through two seams: an Interpreter states each
file's Desire (a target dir or none), a Transformer owns membership and
the move cascade. This module orchestrates records and formats output.

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
    """A generic helper record for an adapter's opinion on one file, as
    a distribution over labels. Not part of the Interpreter contract;
    the adapter uses it inside its private label logic.

    dist is nonempty when the file is labelled. primary is the argmax
    label or None for no opinion. confidence is dist[primary] or 0.0.
    status is a short adapter word (labelled, thin, ambiguous)."""
    dist: dict
    primary: Optional[Label]
    confidence: float
    status: str


@dataclass(frozen=True)
class Placement:
    """A generic helper record an adapter uses inside its private place
    logic. target None means no opinion, so the file stays in place.
    reason is UX prose naming which input won (override, label, name).
    Not part of the Interpreter contract; the adapter maps it to a
    Desire."""
    target: Optional[DirId]
    label: Optional[Label]
    reason: str


@dataclass(frozen=True)
class Desire:
    """What a file wants, per the interpreter. place is the target dir
    or None (unmanaged). hold, when set, is a project reason the file
    must stay despite place; None means free to move."""
    place: Optional[DirId]
    hold: Optional[str]


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


class Interpreter(Protocol):
    def desires(self, scope):
        """scope: set[FileId] -> dict[FileId, Desire]. A file with no
        opinion (no label and no override) is absent. A file the
        interpreter names but places nowhere is present with
        Desire(place=None)."""

    def load(self, mapref):
        """Load the area map named by mapref, an adapter string."""

    def name(self):
        """A short label for the loaded map, for output only."""

    def target_of(self, label):
        """Label -> DirId or None."""

    def ordered_targets(self):
        """The map's targets in declared order, for stable output."""

    # optional: override_notices(scope) -> list[str] (hasattr-gated).
    # optional: resolution(f, place) -> list[str] (adapter prose).


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


# Registry of named pairs. An adapter registers itself on import.
_TRIPLES = {}


def register(name, make):
    """Register a factory returning (Interpreter, Transformer)."""
    _TRIPLES[name] = make


def get_triple(name):
    make = _TRIPLES.get(name)
    if not make:
        sys.exit(f"organize: no such triple '{name}'")
    return make()


def registered(name):
    """Whether a pair named name is registered."""
    return name in _TRIPLES


def _desires(interp, transformer):
    """The interpreter's Desire per scope file: {FileId: Desire}.

    Files the interpreter has no opinion on are absent; a file it names
    but places nowhere is present with Desire(place=None), so the
    unmanaged count stays stable."""
    return interp.desires(transformer.scope())


def _files_for(interp, transformer, target):
    """The set of scope files the interpreter desires in target. The
    Transformer decides which of these actually move (pairing,
    relocatability, already-there)."""
    placed = _desires(interp, transformer)
    return {f for f, d in placed.items() if d.place == target}


def _resolve_target(interp, area):
    """The target directory for an area argument, which may be the
    directory name that status prints (index) or an area label token
    (read). Returns the directory or None."""
    if area in interp.ordered_targets():
        return area
    return interp.target_of(area)


def _override_notices(interp, transformer):
    """Adapter-supplied lint lines for the declared overrides, with a
    header, or an empty list. The core prints the strings verbatim; the
    adapter owns detecting an unresolved value or a duplicate line and
    naming the valid areas."""
    if not hasattr(interp, "override_notices"):
        return []
    notices = interp.override_notices(transformer.scope())
    if not notices:
        return []
    return ["", "override lint:"] + notices


def cmd_apply(interp, transformer, mapref, area, commit):
    if area is None:
        sys.exit("organize apply: --area X is required")
    interp.load(mapref)
    target = _resolve_target(interp, area)
    if not target:
        sys.exit(f"organize apply: unknown area '{area}'\n"
                 "known areas: " + ", ".join(interp.ordered_targets()))
    blockers = transformer.preflight()
    if blockers:
        sys.exit("organize apply: " + "; ".join(blockers))
    if not transformer.target_ready(target):
        sys.exit(f"organize apply: target dir {target}/ is absent")
    plan = transformer.plan(target, _files_for(interp, transformer,
                                               target))
    if not plan.moves:
        print("organize apply: nothing to do")
        return
    result = transformer.apply(plan, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def cmd_status(interp, transformer, mapref, area=None,
               by_area=False, conflicts=False, exit_code=False):
    """Scope the organize against the declared layout.

    The default groups the drift by state, the way git status groups by
    change type, and every count comes from the Transformer's plans.
    renamed is what apply --unconflicted moves (the plan over each area's
    desired files); conflict is the requirement blocks the Transformer
    reports in plan.skipped (a program, a per-object rule); untracked is
    the sources no rule places; organized is the files the plan keeps in
    place. With --by-area, print the same counts per subsystem; with
    --conflicts, print the conflict worklist; with --exit-code, exit 0
    when nothing renames or conflicts and 1 otherwise, for CI. With an
    area, show that subsystem's exact move set."""
    if conflicts:
        _status_conflicts(interp, transformer, mapref)
        return
    interp.load(mapref)
    if area is not None:
        _status_area(interp, transformer, area)
        return
    rows = []
    tot = {"renamed": 0, "conflict": 0, "organized": 0}
    for t in interp.ordered_targets():
        files = _files_for(interp, transformer, t)
        if not files:
            continue
        plan = transformer.plan(t, files)
        renamed = len(plan.moves)
        conflict = len(plan.skipped)
        organized = len(plan.kept_public)
        if not (renamed or conflict or organized):
            continue
        state = "exists" if transformer.target_ready(t) else "new"
        rows.append((t, state, renamed, conflict, organized))
        tot["renamed"] += renamed
        tot["conflict"] += conflict
        tot["organized"] += organized
    if by_area:
        _status_by_area(interp, rows)
        for line in _override_notices(interp, transformer):
            print(line)                # first notice prints its header
        return
    desired = _desires(interp, transformer)
    scope_src = {f for f in transformer.scope() if transformer.is_source(f)}
    placed_src = {f for f in desired if transformer.is_source(f)}
    untracked = len(scope_src - placed_src)
    renamed = tot["renamed"]
    conflict = tot["conflict"]
    is_organized = renamed == 0 and conflict == 0
    banner = "(organized)" if is_organized else "(not organized)"
    print(f"organize status against {interp.name()}   {banner}\n")
    print(f"renamed    {renamed:<5} will move on apply")
    print(f"conflict   {conflict:<5} cannot auto-apply, needs a "
          "decision")
    print(f"untracked  {untracked:<5} no rule places these")
    print(f"organized  {tot['organized']:<5} already in place")
    print(f"\napply --unconflicted moves the {renamed} and holds the "
          f"{conflict} conflicts.\norganize status --conflicts lists "
          "them; --by-area splits per subsystem;\nstatus <area> shows "
          "one.")
    for line in _override_notices(interp, transformer):
        # first notice line prints its own header
        print(line)
    if exit_code:
        sys.exit(0 if is_organized else 1)


def _status_by_area(interp, rows):
    """Print the per-subsystem drift table: the same quantities the
    default groups by state, laid out by area."""
    print(f"organize status against {interp.name()}\n")
    print(f"  {'subsystem':<11}{'dir':<7}{'renamed':>8}"
          f"{'conflict':>9}{'organized':>10}")
    for t, state, renamed, conflict, organized in rows:
        print(f"  {t + '/':<11}{state:<7}{renamed:>8}{conflict:>9}"
              f"{organized:>10}")


def _status_area(interp, transformer, area):
    """Show one subsystem's exact move set, the dry run plan used to
    print."""
    target = _resolve_target(interp, area)
    if not target:
        sys.exit(f"organize status: unknown area '{area}'\n"
                 "known areas: " + ", ".join(interp.ordered_targets()))
    plan = transformer.plan(target, _files_for(interp, transformer,
                                               target))
    print(f"organize status for {target}/  ({len(plan.moves)} will "
          f"move)\n")
    print(f"moves: {len(plan.moves)} files")
    for src, dst in plan.moves:
        print(f"  {src:<22}  ->  {dst}")
    for line in plan.notes:
        print(line)
    if plan.skipped:
        print("\ncannot move (held at root):")
        for f, reason in plan.skipped:
            print(f"  {f}  ({reason})")
    if plan.skipped:
        print(f"\napply --unconflicted moves the {len(plan.moves)} and "
              f"holds the {len(plan.skipped)} it cannot move. See "
              f"organize status\n--conflicts for the decisions.")


def cmd_apply_auto(interp, transformer, mapref, commit):
    """Converge every subsystem in one gated pass.

    The organize's terraform-apply. Each area's plan already withholds
    the files the Transformer cannot move (plan.skipped, the requirement
    blocks); the rest run as one operation, then the Transformer gates
    them: the pure-rename check (a git primitive, domain-agnostic) and
    the validator (a domain plugin, the build for a C tree). The batch is
    staged, or committed with --commit."""
    interp.load(mapref)
    blockers = transformer.preflight()
    if blockers:
        sys.exit("organize apply --unconflicted: " + "; ".join(blockers))
    plans = []
    held = 0
    for t in interp.ordered_targets():
        files = _files_for(interp, transformer, t)
        if not files:
            continue
        plan = transformer.plan(t, files)
        held += len(plan.skipped)
        if plan.moves:
            plans.append(plan)
    if not plans:
        print("organize apply --unconflicted: nothing unconflicted "
              "to carve")
        return
    n = sum(len(p.moves) for p in plans)
    print(f"converging {len(plans)} subsystems, {n} unconflicted "
          f"files; holding {held} conflict sources at "
          f"root\n")
    result = transformer.apply_auto(plans, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def _status_conflicts(interp, transformer, mapref):
    """Emit the worklist of conflicts that need a human or an LM.

    This is the flag-for-intervention artifact, the piece a converging
    apply cannot do on its own. Each entry carries the rule position, the
    blocker, and the decision, so the resolver has all the context and
    the loop stays declarative.

    Every conflict is a requirement block: a source the rule would move
    but the Transformer cannot place mechanically (a program, a
    per-object rule); it is normally left at the root."""
    interp.load(mapref)
    skips = []
    for t in interp.ordered_targets():
        files = _files_for(interp, transformer, t)
        if not files:
            continue
        for f, reason in transformer.plan(t, files).skipped:
            skips.append((f, t, reason))
    print(f"organize status --conflicts: {len(skips)} conflicts\n"
          "(all cannot move)\n")
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
                                  the drift by state: renamed, conflict,
                                  untracked, and clean; --by-area lays the
                                  same numbers out per subsystem;
                                  --conflicts lists each conflict with the
                                  rule edit to record a decision;
                                  --exit-code exits 0 on a clean layout
                                  and 1 otherwise, for CI; with an area (a
                                  directory name or an area token), that
                                  subsystem's exact move set (the dry run
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
    """Parse args, select a pair, dispatch. default_mapref is an
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
    interp, transformer = get_triple(triple or default_triple)
    cmd = args[0] if args else "status"
    if area is None and cmd == "status" and len(args) > 1:
        area = args[1]                 # organize status <area>
    if cmd == "status":
        cmd_status(interp, transformer, mapref, area, by_area,
                   conflicts, exit_code)
    elif cmd == "apply":
        if auto:
            cmd_apply_auto(interp, transformer, mapref, commit)
        else:
            cmd_apply(interp, transformer, mapref, area, commit)
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
