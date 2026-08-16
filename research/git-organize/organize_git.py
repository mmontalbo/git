"""organize git adapter: the git and C and Make and meson pair.

Provides GitInterpreter (label from commit subject prefixes plus the
area map with declared overrides, folded into one Desire per file) and
GitTransformer (root scope, the move cascade, the validator).
Importing this module registers the "git-c" pair. Every git, C,
Makefile, meson, include, nix, and .gitattributes string lives here.
"""
import os
import re
import sys
import subprocess
from collections import Counter, defaultdict

import organize_core
from organize_core import (Vote, Placement, Verdict, Step, Plan,
                        ApplyResult, Desire)

TOP = subprocess.check_output(
    ["git", "rev-parse", "--show-toplevel"], text=True).strip()


def git(*a, allow=(0,)):
    """Run git and return stdout. Exit loudly on an exit code not in
    allow, so a broken repo fails instead of reporting an empty
    result. Pass allow=(0, 1) where no-match is expected (grep)."""
    r = subprocess.run(["git", "-C", TOP, *a],
                       capture_output=True, text=True)
    if r.returncode not in allow:
        sys.exit(f"git {' '.join(a)}: {r.stderr.strip()}")
    return r.stdout


def exists(path):
    """Whether path exists on disk under the worktree root."""
    return os.path.isfile(os.path.join(TOP, path))


def stem(path):
    """Filename without its .c or .h suffix."""
    base = os.path.basename(path)
    return base[:-2] if base.endswith((".c", ".h")) else base


def _duplicate_subsystem_paths():
    """{path: count} for each path that carries more than one
    'organize.subsystem=' entry in the root .gitattributes. Counts lines
    whose first token is a path and whose attributes include an
    'organize.subsystem=' assignment; a leading '/' anchors the path,
    which we strip to match scope names. Absent file or no such line
    yields an empty map."""
    p = os.path.join(TOP, ".gitattributes")
    counts = Counter()
    try:
        fh = open(p, encoding="utf-8")
    except FileNotFoundError:
        return counts
    for line in fh:
        line = line.split("#", 1)[0].strip()
        if not line or "organize.subsystem=" not in line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        if any(a.startswith("organize.subsystem=") for a in parts[1:]):
            counts[parts[0].lstrip("/")] += 1
    return counts


PRE = re.compile(r"^([A-Za-z0-9][\w./-]*):")


def norm(p):
    p = p.lower()
    return p[:-2] if p.endswith((".c", ".h")) else p


INCLUDE = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)

# The two project reasons a placed file holds at root instead of moving.
HOLD_PROGRAM = "built as a program, not a libgit object"
HOLD_PUBLIC = "public interface header, kept at root"


class GitCModel:
    """The git/C/Make analysis both seams share: the Makefile
    membership check and the include-graph reader.

    The Interpreter reads this to fold role (public/internal), pin
    (program), and pairing (header rides its source) into each file's
    Desire; the Transformer reads the same helpers so a plan and a
    marker agree. The Makefile text and its LIB_OBJS index are cached on
    first use. No behavior differs from the prior per-Transformer copy;
    this only lifts it to a single owner."""

    def __init__(self):
        self._lib = None       # Counter of LIB_OBJS += <stem>.o lines
        self._make = None      # cached Makefile text

    def _makefile(self):
        if self._make is None:
            self._make = open(os.path.join(TOP, "Makefile"),
                              encoding="utf-8").read()
        return self._make

    def is_lib_object(self, c):
        """Whether the Makefile builds c as exactly one libgit object,
        that is one 'LIB_OBJS += <stem>.o' line."""
        if self._lib is None:
            self._lib = Counter(re.findall(
                r'^[ \t]*LIB_OBJS \+= (\S+)\.o$',
                self._makefile(), re.M))
        return self._lib[stem(c)] == 1

    def has_localized_core(self, c):
        """Whether the Makefile lists the root source c in
        LOCALIZED_C_CORE (feeds po/git-core.pot). Present for only some
        sources, so absence is fine."""
        pat = re.compile(
            r'^[ \t]*LOCALIZED_C_CORE \+= '
            + re.escape(os.path.basename(c)) + r'$', re.M)
        return bool(pat.search(self._makefile()))

    def _binds_to_root(self, f, arg, h):
        """Whether '#include \"arg\"' in file f binds to the repo root
        header h. A quote include resolves against the includer's own
        directory first, then the root via -I. A local shadow such as
        reftable/tree.h does not count."""
        if os.path.basename(arg) != h:
            return False
        local = os.path.normpath(
            os.path.join(os.path.dirname(f), arg))
        if exists(local):
            return local == h
        return os.path.normpath(arg) == h

    def include_edits(self, h):
        """[(file, arg)] for every tracked *.c/*.h include that binds
        to the root header h. Covers bare 'h' and relative '../h', and
        excludes a local shadow and the doc or patch fixtures. The
        candidate grep pattern matches the INCLUDE regex, including the
        spaced form '# include', so no binding is missed."""
        cand = git("grep", "-lE",
                   r'#[ \t]*include[ \t]+"([^"]*/)?'
                   + re.escape(h) + '"',
                   "--", "*.c", "*.h", allow=(0, 1)).split()
        edits = []
        for f in cand:
            text = open(os.path.join(TOP, f),
                        encoding="utf-8").read()
            for m in INCLUDE.finditer(text):
                if self._binds_to_root(f, m.group(1), h):
                    edits.append((f, m.group(1)))
        return edits

    def internal_headers(self, cands, moving_c):
        """The candidate headers all of whose includers co-move.

        A candidate H is internal when every file that includes H (by
        full path via include_edits) is in the moving set: the moving
        .c files plus any other internal header. A header may include a
        header, so grow the internal set by fixpoint until it stops."""
        includers = {h: {f for f, _ in self.include_edits(h)}
                     for h in cands}
        internal = set()
        changed = True
        while changed:
            changed = False
            for h in cands - internal:
                if includers[h] <= (moving_c | internal):
                    internal.add(h)
                    changed = True
        return internal


class GitInterpreter:
    """State each root .c/.h file's Desire by folding the commit-subject
    label, the declared area map, and the git/C role, pin, and pairing
    into one placement.

    The label comes from the modal commit-subject prefix over a file's
    history, weighted by full commit breadth so a large sweep counts
    little per file; evidence-quality thresholds (at least 2.0 support,
    a 0.34 modal share) live in _label. The map is 'directory: area
    area ...' per line, plus per-path 'organize.subsystem=' overrides; a
    declared override beats the inferred label, and a label resolves to a
    target through its map token.

    On top of the placement this interpreter reads the Makefile and the
    include graph (through GitCModel) to set the Desire.hold that marks a
    file as staying at root: a program .c is pinned; a same-stem .h is
    either internal (it rides its source's Desire) or public (held); a
    header whose source .c is unmanaged is itself unmanaged."""

    def __init__(self, model=None):
        self._owner = {}
        self._name = ""
        self._model = model or GitCModel()

    # the Interpreter contract

    def desires(self, scope):
        """{FileId: Desire} for the files the label or an override names.

        A source .c with a confident label or a declared override is
        present; one with neither is absent, so the unmanaged count stays
        stable. A named source whose label resolves to no target is
        present with Desire(place=None).

        The Desire.hold folds in role, pin, and pairing:
        - A single-LIB_OBJS .c: Desire(place=T, hold=None).
        - A program .c (not a single LIB_OBJS object):
          Desire(place=<derived T or None>, hold=HOLD_PROGRAM); pinned.
        - A same-stem .h whose source .c is placed: internal (every
          includer co-moves) -> it rides the source's Desire; public ->
          Desire(place=<source's place>, hold=HOLD_PUBLIC).
        - A same-stem .h whose source .c is unmanaged: unmanaged.
        - A flagged header-only .h (no .c): internal (all includers
          co-move) rides into T with hold=None, else it is public."""
        votes = self._label(scope)
        over = self._overrides(scope)

        def base_place(f):
            """The label-or-override target for f, or None. Ignores role;
            the hold logic below adds it."""
            vote, ov = votes.get(f), over.get(f)
            if (vote is None or vote.primary is None) and ov is None:
                return None
            return self._place(f, vote, ov).target

        # The .c placements, with the program pin folded in. A source
        # with a label or override but no resolving target is present
        # with place None (named, but placed nowhere), so the unmanaged
        # count stays stable; a source with neither is absent.
        c_desire = {}
        for c in scope:
            if not c.endswith(".c"):
                continue
            vote, ov = votes.get(c), over.get(c)
            named = not ((vote is None or vote.primary is None)
                         and ov is None)
            if not named:
                continue
            t = base_place(c)
            if t is None:
                c_desire[c] = Desire(place=None, hold=None)
            elif self._model.is_lib_object(c):
                c_desire[c] = Desire(place=t, hold=None)
            else:
                c_desire[c] = Desire(place=t, hold=HOLD_PROGRAM)

        # The moving .c set (single LIB_OBJS objects that are placed and
        # not already at their target): the sources a header can ride.
        moving_c = {c for c, d in c_desire.items()
                    if d.place is not None and d.hold is None
                    and not os.path.dirname(c) == d.place}
        cands = {stem(c) + ".h" for c in moving_c
                 if exists(stem(c) + ".h")}
        for h in scope:
            if h.endswith(".h") and base_place(h) is not None \
                    and not exists(stem(h) + ".c"):
                cands.add(h)          # a flagged header-only lib
        internal = self._model.internal_headers(cands, moving_c)

        out = dict(c_desire)
        for h in scope:
            if not h.endswith(".h"):
                continue
            c = stem(h) + ".c"
            if exists(c):
                # A same-stem header rides its source's Desire. When the
                # source is absent or placed nowhere the header is
                # unmanaged; a public header (not internal) is held at
                # the source's target.
                src = c_desire.get(c)
                if src is None or src.place is None:
                    continue          # source unmanaged -> header absent
                if h in internal:
                    out[h] = Desire(place=src.place, hold=src.hold)
                else:
                    out[h] = Desire(place=src.place, hold=HOLD_PUBLIC)
            else:
                # A header-only lib (no same-stem .c). An internal one
                # (all includers co-move) rides into its target and
                # moves; a public one stays at root and, as before, is
                # counted nowhere in status, so it is placed nowhere.
                t = base_place(h)
                if t is None:
                    continue
                if h in internal:
                    out[h] = Desire(place=t, hold=None)
                else:
                    out[h] = Desire(place=None, hold=None)
        return out

    def declared(self, scope):
        """{FileId: DirId} of each file's OWN declared placement: the
        target its label or override resolves to. This is what git
        records as organize.subsystem and check-attr reads back, so the
        attributes command emits exactly these, independent of the role,
        pin, and pairing that desires() folds in. A file with no label or
        override, or one whose value resolves to no target, is absent."""
        votes = self._label(scope)
        over = self._overrides(scope)
        out = {}
        for f in scope:
            vote, ov = votes.get(f), over.get(f)
            if (vote is None or vote.primary is None) and ov is None:
                continue
            target = self._place(f, vote, ov).target
            if target is not None:
                out[f] = target
        return out

    def load(self, mapref):
        self._name = os.path.basename(mapref)
        owner = {}
        try:
            fh = open(mapref, encoding="utf-8")
        except FileNotFoundError:
            sys.exit(f"organize: no such map file: {mapref}")
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line or ":" not in line:
                continue
            d, areas = line.split(":", 1)
            for a in areas.split():
                owner[a] = d.strip()
        self._owner = owner

    def name(self):
        return self._name

    def target_of(self, label):
        """A label to its target dir. Accepts a map token (read), a
        multi-segment label (read/cache), or the destination directory
        name itself (index), so an override may name the directory. A
        value that resolves to nothing returns None (the caller drops
        or reports it)."""
        if label in self._owner.values():
            return label
        return self._owner.get(self._token_of(label))

    def ordered_targets(self):
        return list(dict.fromkeys(self._owner.values()))

    def override_notices(self, scope):
        """Display lines for override lint, for the core to print
        verbatim. Two kinds: an override whose value resolves to no
        target (a typo such as organize.subsystem=packs), naming the
        file, the value, and the valid area names; and a path carrying
        more than one organize.subsystem= line in the root .gitattributes
        (a duplicate the resolver should collapse). Empty when every
        override is clean."""
        lines = []
        valid = ", ".join(self.ordered_targets())
        for f, value in sorted(self._overrides(scope).items()):
            if self.target_of(value) is None:
                lines.append(
                    f"  {f}: organize.subsystem={value} resolves to no "
                    f"area; the file stays at root.\n    valid areas: "
                    f"{valid}")
        for f, n in sorted(_duplicate_subsystem_paths().items()):
            if n > 1:
                lines.append(
                    f"  {f}: {n} organize.subsystem= lines in "
                    ".gitattributes; keep one.")
        return lines

    def resolution(self, f, place):
        """The .gitattributes line that records f's placement. git
        declares a placement by setting organize.subsystem on the path,
        so the attributes command emits these and check-attr reads them
        back. The core prints the lines verbatim and names no attribute."""
        return [f"/{f}  organize.subsystem={place}"]

    # the label, override, and place internals

    def _label(self, scope):
        """Label each file by the modal commit-subject prefix over its
        history. Returns one Vote per confidently labelled file; a file
        below either threshold is absent."""
        wt = defaultdict(lambda: defaultdict(float))
        lab, touched, total = None, [], 0
        log = git("log", "--no-merges", "--name-only",
                  "--format=%x00%s")
        for line in log.split("\n"):
            if line.startswith("\x00"):
                if lab and touched:
                    w = 1.0 / total
                    for f in set(touched):
                        wt[f][lab] += w
                m = PRE.match(line[1:])
                lab = norm(m.group(1)) if m else None
                touched, total = [], 0
            elif line:
                total += 1
                if line in scope:
                    touched.append(line)
        if lab and touched:
            w = 1.0 / total
            for f in set(touched):
                wt[f][lab] += w
        votes = {}
        for f, c in wt.items():
            best, w = max(c.items(), key=lambda kv: (kv[1], kv[0]))
            tot = sum(c.values())
            if tot >= 2.0 and w / tot >= 0.34:
                votes[f] = Vote(dist=dict(c), primary=best,
                                confidence=w / tot, status="labelled")
        return votes

    def _token_of(self, label):
        """The map token for a label: the first hyphen-free segment of
        the first path component."""
        return label.split("/")[0].split("-")[0]

    def _overrides(self, scope):
        """path -> declared 'organize.subsystem' value for scope paths
        that set it.

        Batches one 'git check-attr organize.subsystem --stdin -z'. The
        NUL stream is path, attr, value triples; keep values set and not
        'unspecified'."""
        files = sorted(scope)
        if not files:
            return {}
        r = subprocess.run(
            ["git", "-C", TOP, "check-attr", "organize.subsystem",
             "--stdin", "-z"],
            input="\0".join(files) + "\0",
            capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"git check-attr organize.subsystem: "
                     f"{r.stderr.strip()}")
        fields = r.stdout.split("\0")
        over = {}
        for i in range(0, len(fields) - 2, 3):
            path, _, value = fields[i], fields[i + 1], fields[i + 2]
            if value and value != "unspecified":
                over[path] = value
        return over

    def _place(self, f, vote, override):
        if override is not None:
            label, reason = override, "override"
        elif vote is not None and vote.primary is not None:
            label, reason = vote.primary, "label"
        else:
            return Placement(target=None, label=None, reason="none")
        return Placement(target=self.target_of(label),
                         label=label, reason=reason)

    def token_for(self, target):
        """A label the map routes to target, for suggesting an override
        in a marker. Prefers the target's own name when the map owns it
        (odb, refs), else the first token that maps there (index has no
        'index' token, so this returns one it does own)."""
        if self._owner.get(target) == target:
            return target
        for tok, d in self._owner.items():
            if d == target:
                return tok
        return target


class GitTransformer:
    """The git and C and Make and meson cascade. Owns root scope, the
    include rewrite, the build-list edits, the validator, and rollback.

    The role, pin, and pairing decisions now live in GitInterpreter,
    expressed through Desire(place, hold). The core passes plan() only
    the files it should move (place set, hold None), so this transformer
    is mechanical: it renames the files it is given and edits the build
    lists, with no relocatability veto and no header pairing of its own."""

    def __init__(self, model=None):
        self._model = model or GitCModel()

    # scope and readiness

    def scope(self):
        """Flat root *.c and *.h tracked files."""
        return {f for f in git("ls-files", "*.c", "*.h").split()
                if "/" not in f}

    def target_ready(self, target):
        return os.path.isdir(os.path.join(TOP, target))

    def already_at(self, f, target):
        return os.path.dirname(f) == target

    def is_source(self, f):
        """A source unit is a .c file; a header rides with its source."""
        return f.endswith(".c")

    def preflight(self):
        """Tracked-dirty is the one blocker. Untracked files do not
        block, so apply runs with untracked research dirs present and
        git reset --hard still restores the tree."""
        if self._tracked_dirty():
            return ["tracked changes present"]
        return []

    def recover_hint(self):
        return "recover the pre-apply tree with: git reset --hard"

    # build-rule reparenting

    def _makefile(self):
        return self._model._makefile()

    def _has_localized_core(self, c):
        return self._model.has_localized_core(c)

    def _root_object_rule(self, text, name):
        """Whether the given Makefile text names the root object
        <name>.o as a rule target, that is <name>.o appears as a
        path-and-hyphen-delimited token before the first colon of some
        line, such as 'setup.o: EXTRA_CPPFLAGS = ...' or 'version.o:
        version-def.h'. The lookarounds treat a slash and a hyphen as
        part of the filename, so neither http-walker.o nor a
        setup/setup.o that apply has already reparented matches a bare
        <name> query. This is the post-rewrite safety detector: after
        the triplet rewrite it must be False for every moved object."""
        pat = re.compile(
            r'^[^:\n]*(?<![\w./-])' + re.escape(name)
            + r'\.o(?![\w./-])[^:\n]*:', re.M)
        return bool(pat.search(text))

    # the include resolver (shared with the interpreter through the model)

    def _include_edits(self, h):
        return self._model.include_edits(h)

    def _nonsource_refs(self, h):
        """Tracked '#include \"h\"' references in files that are not
        *.c/*.h (docs, patch fixtures). The rewrite leaves these."""
        allf = set(git("grep", "-l", f'#include "{h}"',
                       allow=(0, 1)).split())
        src = set(git("grep", "-l", f'#include "{h}"',
                      "--", "*.c", "*.h", allow=(0, 1)).split())
        return len(allf - src)

    # plan

    def plan(self, target, files):
        """Build one area's Plan by moving the files it is given.

        The interpreter has already decided role, pin, and pairing and
        expressed them as Desire(place, hold); the core passes here only
        the files that should move (place == target, hold None). This
        transformer is mechanical: it drops any file already in target
        and renames the rest. Every moved header is internal, so the
        carve is a pure git mv with no include rewrites, and there is no
        requirement veto: plan.skipped is empty."""
        move_set = {f for f in files if not self.already_at(f, target)}
        moves = tuple(sorted((f, f"{target}/{f}") for f in move_set))
        headers = sorted(f for f in move_set if f.endswith(".h"))
        steps = self._steps(target, moves)
        notes = self._notes(target, moves)
        return Plan(target=target, moves=moves,
                    skipped=(),
                    steps=steps, notes=notes,
                    kept_public=(),
                    paired_headers=len(headers))

    def paired_internal_header(self, c):
        """The same-stem .h of source c, when it exists and is internal
        (every file that includes it co-moves with c), else None. A
        public interface header the policy keeps at root returns None,
        so a marker never suggests moving one."""
        h = stem(c) + ".h"
        if not exists(h):
            return None
        if h in self._model.internal_headers({h}, {c}):
            return h
        return None

    def _steps(self, target, moves):
        """One move Step per file, its summary the printed line. The
        carve is a pure rename, so there are no include-rewrite edit
        Steps; the build detail rides in notes."""
        steps = []
        for src, dst in moves:
            mark = "   (internal header)" if src.endswith(".h") else ""
            steps.append(Step(
                kind="move",
                summary=f"  {src:<10}  ->  {dst}{mark}",
                reads=(src,), writes=(dst,),
                preview=None, payload=(src, dst)))
        return tuple(steps)

    def _notes(self, target, moves):
        """The build-edit report lines the core prints verbatim after
        the moves. The carve is a pure rename: every moved header is
        internal, so same-dir resolution holds and no include is
        rewritten."""
        moved_c = [s for s, _ in moves if s.endswith(".c")]
        n_c = len(moved_c)
        n_loc = sum(1 for c in moved_c if self._has_localized_core(c))
        lines = ["", "carve is a pure rename (0 include rewrites)"]
        lines += ["", "build edits:",
                  f"  Makefile:    {n_c} LIB_OBJS lines",
                  f"  meson.build: {n_c} source lines"]
        if n_loc:
            lines.append(
                f"  Makefile:    {n_loc} LOCALIZED_C_CORE lines")
        return tuple(lines)

    # apply

    def apply(self, plan, commit):
        """Patch build lists, then git mv the files, stage, and gate on
        the build. The carve is a pure rename: every moved header is
        internal, so no include is rewritten and moved files stay
        byte-identical. Edit before moving so a failure before any git
        mv leaves a recoverable tree."""
        moved_c = [os.path.basename(s) for s, _ in plan.moves
                   if s.endswith(".c")]
        lines = []
        self._patch_build(moved_c, plan.target)
        self._patch_object_rules(moved_c, plan.target)
        for src, dst in plan.moves:
            r = subprocess.run(["git", "-C", TOP, "mv", src, dst],
                               capture_output=True, text=True)
            if r.returncode:
                lines.append(f"organize apply: git mv {src} failed: "
                             f"{r.stderr.strip()}")
                lines.append(self.recover_hint())
                return ApplyResult(False, lines)
        git("add", "-u")
        v = self.validate()
        if not v.ok:
            lines.append(f"organize apply: {v.reason}; nothing "
                         "committed.")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        if commit:
            subprocess.run(
                ["git", "-C", TOP, "commit", "-m",
                 f"{plan.target}: carve out {plan.target}/"],
                check=True)
            lines.append(f"organize apply: committed the "
                         f"{plan.target} carve.")
        else:
            lines.append("organize apply: build passed; carve is "
                         "staged.")
            lines.append("review, then commit, or "
                         + self.recover_hint())
        return ApplyResult(True, lines)

    def validate(self):
        """The validator provider: confirm the organize preserved the
        artifact's invariant. For git's C sources this is the build and
        its tests; another domain plugs a different check here (a docs
        transformer would confirm that links resolve). A domain plugin, not
        the core gate. Returns a Verdict."""
        r = subprocess.run(self._build_cmd(), cwd=TOP)
        if r.returncode:
            return Verdict(False, "validator (build) failed")
        return Verdict(True, "")

    def _verify_pure_renames(self, plans):
        """Core gate, git primitive: every moved file is a content-
        preserving rename that git's rename detection scores R100.
        Domain-agnostic; needs no build. Returns a Verdict."""
        status = {}
        for line in git("diff", "--cached", "-M",
                        "--name-status").splitlines():
            p = line.split("\t")
            if p[0].startswith("R") and len(p) == 3:
                status[p[2]] = p[0]
            elif len(p) == 2:
                status[p[1]] = p[0]
        bad = [dst for pl in plans for _, dst in pl.moves
               if status.get(dst) != "R100"]
        if bad:
            return Verdict(False, "not pure renames (R100): "
                           + ", ".join(sorted(bad)[:5]))
        return Verdict(True, "")

    def apply_auto(self, plans, commit):
        """Converge several subsystems as one gated operation. Create
        each target dir, apply every build-list edit and git mv, then
        gate: first the pure-rename check (a git primitive), then the
        validator (a domain plugin). Leave the batch staged, or commit
        it. On any failure the tree is recoverable with reset."""
        lines = []
        for plan in plans:
            d = os.path.join(TOP, plan.target)
            if not os.path.isdir(d):
                os.makedirs(d)
            moved_c = [os.path.basename(s) for s, _ in plan.moves
                       if s.endswith(".c")]
            self._patch_build(moved_c, plan.target)
            self._patch_object_rules(moved_c, plan.target)
            for src, dst in plan.moves:
                r = subprocess.run(["git", "-C", TOP, "mv", src, dst],
                                   capture_output=True, text=True)
                if r.returncode:
                    lines.append(f"organize apply --auto: git mv {src} "
                                 f"failed: {r.stderr.strip()}")
                    lines.append(self.recover_hint())
                    return ApplyResult(False, lines)
        git("add", "-u")
        v = self._verify_pure_renames(plans)
        if not v.ok:
            lines.append(f"organize apply --auto: {v.reason}")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        v = self.validate()
        if not v.ok:
            lines.append(f"organize apply --auto: {v.reason}; nothing "
                         "committed.")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        n = sum(len(p.moves) for p in plans)
        lines.append(f"organize apply --auto: pure renames + validator "
                     f"passed ({n} files, {len(plans)} subsystems).")
        if commit:
            subprocess.run(
                ["git", "-C", TOP, "commit", "-m",
                 f"organize: converge {len(plans)} subsystems "
                 f"({n} files)"], check=True)
            lines.append("organize apply --auto: committed.")
        else:
            lines.append("review, then commit, or "
                         + self.recover_hint())
        return ApplyResult(True, lines)

    def _rewrite_includes(self, headers, target):
        """Rewrite each include that binds to a moved root header to
        the from-root form '#include \"target/H\"'. Rewrites the quoted
        group only, so trailing comments survive. Guards: the new token
        must be absent (no double prefix) and moved header basenames
        must be unique in the include set. One read and one write per
        file."""
        seen = set()
        for h in headers:
            if h in seen:
                sys.exit(f"organize apply: duplicate header {h}")
            seen.add(h)
            present = git("grep", "-l", f'#include "{target}/{h}"',
                          allow=(0, 1)).split()
            if present:
                sys.exit(f"organize apply: '#include \"{target}/{h}\"' "
                         "already present; refusing double prefix")
        per_file = defaultdict(list)
        for h in headers:
            for f, arg in self._include_edits(h):
                per_file[f].append((arg, h))
        for f, edits in per_file.items():
            p = os.path.join(TOP, f)
            text = open(p, encoding="utf-8").read()
            for arg, h in edits:
                pat = re.compile(
                    r'(^[ \t]*#[ \t]*include[ \t]+)"'
                    + re.escape(arg) + '"', re.M)
                text = pat.sub(r'\1"' + target + '/' + h + '"', text)
            open(p, "w", encoding="utf-8").write(text)

    def _patch_build(self, moved_c, target):
        """Full-line edit Makefile and meson.build for each moved .c.
        The LIB_OBJS and meson source edits preserve leading whitespace
        and must match exactly once per name per file, else it is a loud
        error. The optional LOCALIZED_C_CORE edit re-paths po/git-core
        sources; it exists for only some sources, so 0 or 1 match is
        fine."""
        for path, pat_t, rep_t in [
            ("Makefile", r'^([ \t]*)LIB_OBJS \+= {n}\.o$',
             r'\1LIB_OBJS += ' + target + r'/{n}.o'),
            ("meson.build", r"^([ \t]*)'{n}\.c',$",
             r"\1'" + target + r"/{n}.c',"),
        ]:
            p = os.path.join(TOP, path)
            text = open(p, encoding="utf-8").read()
            for c in moved_c:
                n = c[:-2]
                pat = re.compile(pat_t.format(n=re.escape(n)), re.M)
                new, k = pat.subn(rep_t.format(n=n), text)
                if k != 1:
                    sys.exit(f"organize apply: {path}: expected 1 line "
                             f"for {n}, found {k}")
                text = new
            open(p, "w", encoding="utf-8").write(text)
        p = os.path.join(TOP, "Makefile")
        text = open(p, encoding="utf-8").read()
        for c in moved_c:
            n = c[:-2]
            pat = re.compile(
                r'^([ \t]*)LOCALIZED_C_CORE \+= ' + re.escape(n)
                + r'\.c$', re.M)
            text, k = pat.subn(
                r'\1LOCALIZED_C_CORE += ' + target + '/' + n + '.c',
                text)
            if k > 1:
                sys.exit(f"organize apply: Makefile: expected 0 or 1 "
                         f"LOCALIZED_C_CORE line for {n}, found {k}")
        open(p, "w", encoding="utf-8").write(text)

    def _patch_object_rules(self, moved_c, target):
        """Reparent per-object Makefile rules for the moved sources.

        git writes per-object build state as a consistent triplet
        target 'NAME.sp NAME.s NAME.o:'. For each moved .c NAME, rewrite
        every such line to the same triplet under target/, preserving
        the rest of the line. A file may carry 0, 1, or 2 such lines;
        rewrite all. meson needs no change: it applies these defines
        project-wide via libgit_c_args, not per file.

        Safety: after the rewrite, the root object NAME.o must no longer
        be a rule target in any form. If a per-object rule survives that
        the triplet rewrite did not catch, stop rather than move the
        source into a tree that will not build."""
        p = os.path.join(TOP, "Makefile")
        text = open(p, encoding="utf-8").read()
        pfx = target + "/"
        for c in moved_c:
            n = c[:-2]
            e = re.escape(n)
            pat = re.compile(
                r'^(?P<lead>[ \t]*)'
                + e + r'\.sp[ \t]+' + e + r'\.s[ \t]+' + e
                + r'\.o(?P<rest>[ \t]*:.*)$', re.M)
            text = pat.sub(
                lambda m: (m.group("lead") + pfx + n + ".sp "
                           + pfx + n + ".s " + pfx + n + ".o"
                           + m.group("rest")),
                text)
        for c in moved_c:
            n = c[:-2]
            if self._root_object_rule(text, n):
                sys.exit(f"organize apply: Makefile: a per-object rule "
                         f"for {n}.o survives in a form this adapter "
                         f"does not reparent; refusing to move {n}.c")
        open(p, "w", encoding="utf-8").write(text)

    def _tracked_dirty(self):
        unstaged = subprocess.run(
            ["git", "-C", TOP, "diff", "--quiet"]).returncode
        staged = subprocess.run(
            ["git", "-C", TOP, "diff", "--cached",
             "--quiet"]).returncode
        return bool(unstaged or staged)

    def _build_cmd(self):
        """The build invocation. Use the repo's nix shell when present
        so git's link libraries and the PERL_PATH, PYTHON_PATH, and
        SHELL_PATH exports are on the path, else plain make."""
        jobs = os.cpu_count() or 1
        make = f"make -j{jobs}"
        if os.path.isfile(os.path.join(TOP, "shell.nix")):
            return ["nix-shell", "--run", make]
        return ["sh", "-c", make]


def _make_git_c():
    model = GitCModel()          # one Makefile/include reader, shared
    return (GitInterpreter(model), GitTransformer(model))


organize_core.register("git-c", _make_git_c)
