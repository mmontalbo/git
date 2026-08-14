"""reorg core: records, seams, orchestration, registry.

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
        sys.exit(f"reorg: no such triple '{name}'")
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
        sys.exit(f"reorg plan: unknown area '{area}'")
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
        sys.exit("reorg apply: --area X is required")
    policy.load(mapref)
    target = policy.target_of(area)
    if not target:
        sys.exit(f"reorg apply: unknown area '{area}'")
    blockers = enforcer.preflight()
    if blockers:
        sys.exit("reorg apply: " + "; ".join(blockers))
    if not enforcer.target_ready(target):
        sys.exit(f"reorg apply: target dir {target}/ is absent")
    plan = enforcer.plan(target, _files_for(signal, policy,
                                            enforcer, target))
    if not plan.moves:
        print("reorg apply: nothing to do")
        return
    result = enforcer.apply(plan, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def take_opt(args, name):
    """Pop '--name VALUE' from args, returning (value, args)."""
    if name not in args:
        return None, args
    i = args.index(name)
    return args[i + 1], args[:i] + args[i + 2:]


DOC = """reorg: enforce a declared source layout.

Usage:
  reorg check [--map FILE]        report files whose area does not
                                  match the directory its area maps to
  reorg plan [--area X] [--map FILE]
                                  dry run: show one area's moves and its
                                  include, build, and pairing effects
  reorg apply --area X [--commit]
                                  perform one area's carve, stop before
                                  commit unless --commit is given
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
    args = [a for a in args if a != "--commit"]
    signal, policy, enforcer = get_triple(triple or default_triple)
    cmd = args[0] if args else "check"
    if cmd == "check":
        cmd_check(signal, policy, enforcer, mapref)
    elif cmd == "plan":
        cmd_plan(signal, policy, enforcer, mapref, area)
    elif cmd == "apply":
        cmd_apply(signal, policy, enforcer, mapref, area, commit)
    else:
        sys.exit(DOC)
