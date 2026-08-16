"""organize core: records, seams, orchestration, registry.

Enforce a declared layout through two seams cast as a git tree diff. An
Interpreter names each file's TARGET directory (where the rules say the
blob should live); a Transformer diffs the current tree against that
target tree. The diff is a set of RENAMES (a blob moves to a new path,
content identical, R100) plus PATCHES (a referrer blob whose content
changes only to repoint at moved blobs, same path). A diff entry that
cannot be applied cleanly is a CONFLICT. This module orchestrates the
records and formats output.

Contract of this module (the purity invariant):
- It runs no subprocess and imports no build or version-control tool.
- It names no build system, language, or attribute file. Every such
  string lives in an adapter.
- FileId and DirId are opaque handles. This module never interprets
  their text, joins a path, or takes a parent directory off one. The
  Transformer computes target paths; the core reads a Rename's src and
  dst. Any path or existence question routes through the Transformer.
"""
import sys
from dataclasses import dataclass, field
from typing import NewType, Optional, Protocol

# Opaque handles. This module never interprets their text.
FileId = NewType("FileId", str)
DirId = NewType("DirId", str)
Label = str

# The repository root as a target directory. A file whose target dir is
# ROOT stays at the top (a public interface header kept at root); its
# target path equals its basename. The core passes this handle through
# to the Transformer, which owns the path arithmetic.
ROOT = DirId("")


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
    target directory."""
    target: Optional[DirId]
    label: Optional[Label]
    reason: str


@dataclass(frozen=True)
class Rename:
    """A blob moves to a new path, content identical (R100). ok is
    False when the transformer cannot perform the move (a conflict);
    reason says why."""
    src: FileId
    dst: FileId
    ok: bool
    reason: str


@dataclass(frozen=True)
class Patch:
    """A referrer blob whose content changes only to repoint at moved
    blobs (same path, new content) -- a patched-blob future computed
    before it is written. ok is False when the pointer update cannot
    be computed unambiguously (a conflict); reason says why. summary
    is a short human line; payload is opaque, the transformer's apply
    consumes it."""
    path: FileId
    ok: bool
    reason: str
    summary: str
    payload: object


@dataclass(frozen=True)
class Diff:
    """The tree diff from current to target: renames plus the pointer
    patches they entail."""
    renames: tuple      # of Rename
    patches: tuple      # of Patch


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


@dataclass
class ApplyResult:
    """The outcome of an apply: ok plus lines the core prints."""
    ok: bool
    lines: list = field(default_factory=list)


class Interpreter(Protocol):
    def targets(self, scope):
        """scope: set[FileId] -> dict[FileId, DirId]. The target
        directory for each file the rules address. A file no rule
        addresses is ABSENT (untracked). A file that should stay at the
        root (a public interface header) maps to ROOT. Placement
        (subsystem from label or override) and role (public header ->
        ROOT, internal header -> its source's dir) and pairing live
        here. A program pin does NOT live here: a program gets its
        subsystem dir like any file; the transformer's diff() decides it
        cannot move."""

    def load(self, mapref):
        """Load the area map named by mapref, an adapter string."""

    def name(self):
        """A short label for the loaded map, for output only."""

    def target_of(self, label):
        """Label -> DirId or None."""

    def ordered_targets(self):
        """The map's targets in declared order, for stable output."""

    # optional: override_notices(scope) -> list[str] (hasattr-gated).
    # optional: declared(scope) -> {FileId: DirId} (hasattr-gated).
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

    def preflight(self):
        """Blocking reasons (list[str]); empty means clear to apply."""

    def diff(self, targets):
        """{FileId: DirId} -> Diff. Compute the tree diff from the
        current tree to the target tree. For each file whose target PATH
        (target_dir joined with basename, or basename when the dir is
        ROOT) differs from its current path, emit a Rename(src, dst, ok,
        reason): ok False when the move cannot be performed. Then compute
        the PATCHES the ok renames entail: each referrer blob that names
        a moved file, as a Patch with ok True when the pointer update is
        unambiguous. The core does no path arithmetic; it reads the
        Rename src and dst."""

    def apply(self, diff, commit):
        """Perform the ok renames and ok patches, run validate, roll
        back on failure. Ignore the not-ok entries (they are held
        conflicts). Returns ApplyResult. Commits only when commit is
        true."""

    def recover_hint(self):
        """The one string that restores the pre-apply tree."""


# Registry of named pairs. An adapter registers itself on import.
_PAIRS = {}


def register(name, make):
    """Register a factory returning (Interpreter, Transformer)."""
    _PAIRS[name] = make


def get_pair(name):
    make = _PAIRS.get(name)
    if not make:
        sys.exit(f"organize: no such project '{name}'")
    return make()


def registered(name):
    """Whether a pair named name is registered."""
    return name in _PAIRS


def _targets(interp, transformer):
    """The interpreter's target directory per addressed scope file:
    {FileId: DirId}. A file no rule addresses is absent (untracked)."""
    return interp.targets(transformer.scope())


def _diff(interp, transformer):
    """The tree diff from the current tree to the interpreter's target
    tree: a Diff of renames plus the pointer patches they entail."""
    return transformer.diff(_targets(interp, transformer))


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


def _area_diff(interp, transformer, target):
    """The tree diff scoped to one target directory: the renames whose
    destination lands in target, plus the patches those renames entail.

    The interpreter names the whole tree's targets; restricting to the
    files whose target is this area yields that area's slice of the
    diff, so a per-area apply moves only that subsystem."""
    all_targets = _targets(interp, transformer)
    scoped = {f: t for f, t in all_targets.items() if t == target}
    return transformer.diff(scoped)


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
    diff = _area_diff(interp, transformer, target)
    ok_renames = [r for r in diff.renames if r.ok]
    if not ok_renames:
        print("organize apply: nothing to do")
        return
    result = transformer.apply(diff, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def _area_of(interp, transformer, targets, rename):
    """The target directory a Rename lands in: the target the
    interpreter named for the moved file. The core does no path
    arithmetic, so it reads the interpreter's target map by src."""
    return targets.get(rename.src)


def cmd_status(interp, transformer, mapref, area=None,
               by_area=False, conflicts=False, exit_code=False,
               plan=False):
    """Scope the organize as a git tree diff against the target layout.

    The default groups the diff by state, the way git status groups by
    change type. Every count derives from the Diff and the interpreter's
    targets:
    - organized: files the interpreter addresses whose target path
      equals the current path (not in the renames) -- the public
      headers the rules keep at root.
    - renamed: the ok renames (what apply moves).
    - conflict: the not-ok renames plus not-ok patches (the entries the
      transformer cannot apply cleanly, e.g. a program built from a
      path-derived name).
    - untracked: sources the interpreter does NOT address.
    With --by-area, print the same counts per subsystem; with
    --conflicts, list each not-ok entry with its reason; with --plan,
    the incremental refactoring view; with --exit-code, exit 0 when
    nothing renames and no conflict and 1 otherwise, for CI. With an
    area, show that subsystem's renames."""
    if conflicts:
        _status_conflicts(interp, transformer, mapref)
        return
    interp.load(mapref)
    if plan:
        _status_plan(interp, transformer)
        return
    if area is not None:
        _status_area(interp, transformer, area)
        return
    targets = _targets(interp, transformer)
    diff = transformer.diff(targets)
    ok_renames = [r for r in diff.renames if r.ok]
    conflict_entries = ([r for r in diff.renames if not r.ok]
                        + [p for p in diff.patches if not p.ok])
    ok_patches = [p for p in diff.patches if p.ok]

    if by_area:
        _status_by_area(interp, transformer, targets, diff)
        for line in _override_notices(interp, transformer):
            print(line)                # first notice prints its header
        return

    renamed_srcs = {r.src for r in diff.renames}
    organized = sum(1 for f in targets if f not in renamed_srcs)
    renamed = len(ok_renames)
    conflict = len(conflict_entries)
    scope_src = {f for f in transformer.scope() if transformer.is_source(f)}
    untracked = len(scope_src - set(targets))

    clean = renamed == 0 and conflict == 0
    banner = "(organized)" if clean else "(not organized)"
    print(f"organize status against {interp.name()}   {banner}\n")
    print(f"renamed    {renamed:<5} will move on apply")
    print(f"conflict   {conflict:<5} cannot auto-apply, needs a "
          "decision")
    print(f"untracked  {untracked:<5} no rule places these")
    print(f"organized  {organized:<5} already in place")
    print(f"\napply moves the {renamed} and patches {len(ok_patches)} "
          f"referrer files; {conflict} renames are held (programs).")
    print("organize status --by-area splits per subsystem; "
          "status <area> shows one.")
    for line in _override_notices(interp, transformer):
        # first notice line prints its own header
        print(line)
    if exit_code:
        sys.exit(0 if clean else 1)


def _area_rows(interp, transformer, targets, diff):
    """Per-subsystem (target, state, renamed, conflict, organized) rows
    from the diff, one per target the interpreter declares that has any
    addressed file. Every count derives from the Diff and the targets,
    the same quantities the default groups by state."""
    renamed_srcs = {r.src for r in diff.renames}
    ok_by_area, conflict_by_area, organized_by_area = {}, {}, {}
    for r in diff.renames:
        area = _area_of(interp, transformer, targets, r)
        bucket = ok_by_area if r.ok else conflict_by_area
        bucket[area] = bucket.get(area, 0) + 1
    for f, t in targets.items():
        if f not in renamed_srcs:
            organized_by_area[t] = organized_by_area.get(t, 0) + 1
    rows = []
    for t in interp.ordered_targets():
        renamed = ok_by_area.get(t, 0)
        conflict = conflict_by_area.get(t, 0)
        organized = organized_by_area.get(t, 0)
        if not (renamed or conflict or organized):
            continue
        state = "exists" if transformer.target_ready(t) else "new"
        rows.append((t, state, renamed, conflict, organized))
    return rows


def _status_by_area(interp, transformer, targets, diff):
    """Print the per-subsystem drift table: the same quantities the
    default groups by state, laid out by area."""
    print(f"organize status against {interp.name()}\n")
    print(f"  {'subsystem':<11}{'dir':<7}{'renamed':>8}"
          f"{'conflict':>9}{'organized':>10}")
    for t, state, renamed, conflict, organized in _area_rows(
            interp, transformer, targets, diff):
        print(f"  {t + '/':<11}{state:<7}{renamed:>8}{conflict:>9}"
              f"{organized:>10}")


def _status_area(interp, transformer, area):
    """Show one subsystem's renames, the dry run diff used to print."""
    target = _resolve_target(interp, area)
    if not target:
        sys.exit(f"organize status: unknown area '{area}'\n"
                 "known areas: " + ", ".join(interp.ordered_targets()))
    diff = _area_diff(interp, transformer, target)
    ok_renames = [r for r in diff.renames if r.ok]
    print(f"organize status for {target}/  ({len(ok_renames)} will "
          f"move)\n")
    print(f"moves: {len(ok_renames)} files")
    for r in ok_renames:
        print(f"  {r.src:<22}  ->  {r.dst}")
    held = [r for r in diff.renames if not r.ok]
    if held:
        print(f"\nheld ({len(held)}):")
        for r in held:
            print(f"  {r.src:<22}  {r.reason}")


def cmd_apply_auto(interp, transformer, mapref, commit):
    """Converge every subsystem in one gated pass.

    The organize's terraform-apply. The Diff carries every ok rename and
    the pointer patches they entail plus the not-ok entries the
    transformer cannot apply (the held conflicts); apply performs only
    the ok entries as one operation, then gates on the validator (a
    domain plugin, the build for a C tree). The batch is staged, or
    committed with --commit."""
    interp.load(mapref)
    blockers = transformer.preflight()
    if blockers:
        sys.exit("organize apply --unconflicted: " + "; ".join(blockers))
    diff = _diff(interp, transformer)
    ok_renames = [r for r in diff.renames if r.ok]
    held = ([r for r in diff.renames if not r.ok]
            + [p for p in diff.patches if not p.ok])
    if not ok_renames:
        print("organize apply --unconflicted: nothing unconflicted "
              "to carve")
        return
    ok_patches = [p for p in diff.patches if p.ok]
    print(f"converging {len(ok_renames)} unconflicted files, "
          f"patching {len(ok_patches)} referrer files; holding "
          f"{len(held)} conflict entries at root\n")
    result = transformer.apply(diff, commit)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def _status_conflicts(interp, transformer, mapref):
    """Emit the worklist of conflicts that need a human or an LM.

    A conflict is a diff entry the transformer cannot apply cleanly: a
    not-ok Rename (a program built from a path-derived name, so the move
    would break the build) or a not-ok Patch (a pointer update it cannot
    compute unambiguously). Each prints with its reason."""
    interp.load(mapref)
    diff = _diff(interp, transformer)
    held_renames = [r for r in diff.renames if not r.ok]
    held_patches = [p for p in diff.patches if not p.ok]
    n = len(held_renames) + len(held_patches)
    if n == 0:
        print("organize status --conflicts: 0 conflicts")
        return
    print(f"organize status --conflicts: {n} conflicts\n")
    for r in held_renames:
        print(f"  {r.src:<22}  {r.reason}")
    for p in held_patches:
        print(f"  {p.path:<22}  {p.reason}")


def _status_plan(interp, transformer):
    """The incremental refactoring view: the diff sorted into four bands.

    cleaned up      the files already at their target path (organized)
    auto-cleanable  the ok renames, grouped by target subsystem
    partial         the not-ok renames and patches, grouped, with reason
    needs rules     the sources no rule addresses (untracked)"""
    targets = _targets(interp, transformer)
    diff = transformer.diff(targets)
    renamed_srcs = {r.src for r in diff.renames}
    cleaned = sum(1 for f in targets if f not in renamed_srcs)

    auto = {}
    for r in diff.renames:
        if r.ok:
            area = _area_of(interp, transformer, targets, r)
            auto.setdefault(area, []).append(r)

    partial = {}
    for r in diff.renames:
        if not r.ok:
            area = _area_of(interp, transformer, targets, r)
            partial.setdefault(area, []).append((r.src, r.reason))
    for p in diff.patches:
        if not p.ok:
            partial.setdefault(None, []).append((p.path, p.reason))

    scope_src = {f for f in transformer.scope() if transformer.is_source(f)}
    needs = sorted(scope_src - set(targets))

    n_auto = sum(len(v) for v in auto.values())
    n_partial = sum(len(v) for v in partial.values())
    print(f"organize plan against {interp.name()}\n")
    print(f"cleaned up      {cleaned:<5} already at their target path")
    print(f"auto-cleanable  {n_auto:<5} move on apply --unconflicted")
    print(f"partial         {n_partial:<5} held, need a decision")
    print(f"needs rules     {len(needs):<5} no rule places these\n")
    if auto:
        print("auto-cleanable by subsystem:")
        for t in interp.ordered_targets():
            if auto.get(t):
                print(f"  {t + '/':<12} {len(auto[t])}")
    if partial:
        print("\npartial:")
        for t in interp.ordered_targets():
            for f, reason in partial.get(t, []):
                print(f"  {(t + '/'):<12} {f:<22} {reason}")
        for f, reason in partial.get(None, []):
            print(f"  {'(referrer)':<12} {f:<22} {reason}")
    if needs:
        print("\nneeds rules:")
        for f in needs:
            print(f"  {f}")


def take_opt(args, name):
    """Pop '--name VALUE' from args, returning (value, args)."""
    if name not in args:
        return None, args
    i = args.index(name)
    return args[i + 1], args[:i] + args[i + 2:]


DOC = """organize: enforce a declared source layout.

Usage:
  organize status [AREA] [--by-area] [--conflicts] [--plan]
                  [--exit-code] [--map FILE]
                                  the orientation view, and the default
                                  with no command. status is the tree
                                  diff from the current tree to the
                                  target tree. With no area, group the
                                  diff by state: renamed, conflict,
                                  untracked, and organized; --by-area
                                  lays the same numbers out per
                                  subsystem; --conflicts lists each
                                  not-ok diff entry with its reason;
                                  --plan shows the incremental
                                  refactoring bands (cleaned up,
                                  auto-cleanable, partial, needs rules);
                                  --exit-code exits 0 on an organized
                                  tree and 1 otherwise, for CI; with an
                                  area (a directory name or an area
                                  token), that subsystem's renames
  organize apply --area X [--commit]
                                  perform one area's renames and patches,
                                  stop before commit unless --commit
  organize apply --unconflicted [--commit]
                                  perform every ok rename and the patches
                                  they entail in one validator-gated
                                  pass; hold conflict entries at root
  organize attributes [--map FILE]
                                  print the declared placement as one
                                  attribute line per placed file, the set
                                  git check-attr reads to reproduce the
                                  layout
"""


def cmd_attributes(interp, transformer, mapref):
    """Print the declared placement as attribute lines, one per placed
    file, sorted. The interpreter owns the attribute name and returns the
    line through resolution; the core prints it verbatim and names none.
    The output is what git check-attr reads back to reproduce the carve,
    so a maintainer can record the layout with it.

    The lines record each file's OWN declared placement, not the role
    and pairing that targets() folds in for status; an interpreter
    exposes that set through declared(), falling back to targets() with
    the files kept at ROOT dropped (a root file has no subsystem to
    record)."""
    interp.load(mapref)
    if hasattr(interp, "declared"):
        placed = interp.declared(transformer.scope())
    else:
        placed = {f: t for f, t in
                  _targets(interp, transformer).items()
                  if t != ROOT}
    for f in sorted(placed):
        for line in interp.resolution(f, placed[f]):
            print(line)


def main(default_pair, default_mapref):
    """Parse args, select a pair, dispatch. default_mapref is an
    adapter string the core passes through without interpreting."""
    args = sys.argv[1:]
    mapref, args = take_opt(args, "--map")
    if mapref is None:
        mapref = default_mapref
    area, args = take_opt(args, "--area")
    commit = "--commit" in args
    auto = "--unconflicted" in args
    by_area = "--by-area" in args
    conflicts = "--conflicts" in args
    plan = "--plan" in args
    exit_code = "--exit-code" in args
    args = [a for a in args
            if a not in ("--commit", "--unconflicted", "--by-area",
                         "--conflicts", "--plan", "--exit-code")]
    if args and args[0] in ("--help", "-h", "help"):
        print(DOC)                     # explicit help exits 0
        return
    interp, transformer = get_pair(default_pair)
    cmd = args[0] if args else "status"
    if area is None and cmd == "status" and len(args) > 1:
        area = args[1]                 # organize status <area>
    if cmd == "status":
        cmd_status(interp, transformer, mapref, area, by_area,
                   conflicts, exit_code, plan)
    elif cmd == "apply":
        if auto:
            cmd_apply_auto(interp, transformer, mapref, commit)
        else:
            cmd_apply(interp, transformer, mapref, area, commit)
    elif cmd == "attributes":
        cmd_attributes(interp, transformer, mapref)
    elif cmd == "check":
        sys.exit("organize check is now organize status --exit-code")
    elif cmd == "agree":
        sys.exit("organize agree is now organize status --by-area and "
                 "organize status --conflicts")
    elif cmd == "todo":
        sys.exit("organize todo is now organize status --conflicts")
    elif cmd == "plan":
        sys.exit("organize plan is now organize status --plan")
    else:
        sys.exit(DOC)                  # unknown command: nonzero + DOC
