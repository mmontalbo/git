"""organize git adapter: the git and C and Make and meson pair.

Provides GitInterpreter (label from commit subject prefixes plus the
area map with declared overrides, folded into one target dir per file)
and GitTransformer (root scope, the tree diff, the validator).
Importing this module registers the "git-c" pair. Every git, C,
Makefile, meson, include, nix, and .gitattributes string lives here.
"""
import os
import re
import sys
import subprocess
from collections import Counter, defaultdict

import organize_core
from organize_core import (Verdict, Rename, Patch, Diff,
                        ApplyResult, ROOT)

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

# The reason the transformer holds a program rename: git builds the
# program from a path-derived name, so moving the source breaks the
# build. The interpreter still gives a program its subsystem dir; the
# transformer's diff() marks the rename not-ok with this reason.
HOLD_PROGRAM = "built as a program, not a libgit object"


class GitCModel:
    """The git/C/Make analysis both seams share: the Makefile
    membership check and the include-graph reader.

    The Interpreter reads this to fold role (public/internal) and
    pairing (header rides its source) into each file's target dir; the
    Transformer reads the same helpers (is_lib_object gates a rename)
    so the diff and the interpreter agree. The Makefile text and its
    LIB_OBJS index are cached on first use."""

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
    """Name each root .c/.h file's TARGET directory by folding the
    commit-subject label, the declared area map, and the git/C role and
    pairing into one placement.

    The label comes from the modal commit-subject prefix over a file's
    history, weighted by full commit breadth so a large sweep counts
    little per file; evidence-quality thresholds (at least 2.0 support,
    a 0.34 modal share) live in _label. The map is 'directory: area
    area ...' per line, plus per-path 'organize.subsystem=' overrides; a
    declared override beats the inferred label, and a label resolves to a
    target through its map token.

    On top of the placement this interpreter reads the Makefile and the
    include graph (through GitCModel) to set each file's role: a .c with
    a resolvable subsystem targets that dir, program or not (the
    transformer decides a program cannot move); a same-stem .h is either
    internal (it rides its source into that dir) or public (it targets
    ROOT, kept at the top); a header whose source .c is unmanaged is
    itself unmanaged (absent)."""

    def __init__(self, model=None):
        self._owner = {}
        self._name = ""
        self._model = model or GitCModel()

    # the Interpreter contract

    def targets(self, scope):
        """{FileId: DirId} for the files the label or an override names.

        A source .c with a confident label or a declared override is
        present; one with neither is absent, so the untracked count stays
        stable. A named source whose label resolves to no target is
        absent (named, but placed nowhere).

        Role and pairing fold in:
        - A .c with a resolvable subsystem targets that dir, program or
          not; the transformer holds a program's rename.
        - A same-stem .h whose source .c targets a dir: internal (every
          includer co-moves) -> it targets the same dir; public ->
          ROOT (kept at the top).
        - A same-stem .h whose source .c is absent: absent.
        - A flagged header-only .h (no .c): internal (all includers
          co-move) targets its dir; public (not internal) is absent, so
          no rule places it and it is counted nowhere."""
        labels = self._label(scope)
        over = self._overrides(scope)

        def base_place(f):
            """The label-or-override target for f, or None. Ignores role;
            the role logic below adds it."""
            label, ov = labels.get(f), over.get(f)
            if label is None and ov is None:
                return None
            return self._place(f, label, ov)

        # The .c placements. A source with a label or override that
        # resolves to a target targets that dir, program or not. A
        # source with neither, or one whose value resolves to no target,
        # is absent, so the untracked count stays stable.
        c_target = {}
        for c in scope:
            if not c.endswith(".c"):
                continue
            t = base_place(c)
            if t is not None:
                c_target[c] = t

        # The moving .c set (library objects that will actually move):
        # the sources a header can ride. A program's .c is excluded, so
        # its same-stem header stays public and targets ROOT.
        moving_c = {c for c, t in c_target.items()
                    if self._model.is_lib_object(c)
                    and not os.path.dirname(c) == t}
        cands = {stem(c) + ".h" for c in moving_c
                 if exists(stem(c) + ".h")}
        for h in scope:
            if h.endswith(".h") and base_place(h) is not None \
                    and not exists(stem(h) + ".c"):
                cands.add(h)          # a flagged header-only lib
        internal = self._model.internal_headers(cands, moving_c)

        out = dict(c_target)
        for h in scope:
            if not h.endswith(".h"):
                continue
            c = stem(h) + ".c"
            if exists(c):
                # A same-stem header rides its source. When the source
                # is absent the header is absent; an internal header
                # targets the source's dir; a public header targets ROOT.
                t = c_target.get(c)
                if t is None:
                    continue          # source unmanaged -> header absent
                out[h] = t if h in internal else ROOT
            else:
                # A header-only lib (no same-stem .c). An internal one
                # (all includers co-move) targets its dir. A public one
                # (flagged for a subsystem but not internal) is absent:
                # no rule places it, so it is neither renamed nor
                # organized, as before.
                t = base_place(h)
                if t is None or h not in internal:
                    continue
                out[h] = t
        return out

    def declared(self, scope):
        """{FileId: DirId} of each file's OWN declared placement: the
        target its label or override resolves to. This is what git
        records as organize.subsystem and check-attr reads back, so the
        attributes command emits exactly these, independent of the role
        and pairing that targets() folds in. A file with no label or
        override, or one whose value resolves to no target, is absent."""
        labels = self._label(scope)
        over = self._overrides(scope)
        out = {}
        for f in scope:
            label, ov = labels.get(f), over.get(f)
            if label is None and ov is None:
                continue
            target = self._place(f, label, ov)
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
        history. Returns {file: label} for each confidently labelled
        file; a file below either threshold is absent."""
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
        labels = {}
        for f, c in wt.items():
            best, w = max(c.items(), key=lambda kv: (kv[1], kv[0]))
            tot = sum(c.values())
            if tot >= 2.0 and w / tot >= 0.34:
                labels[f] = best
        return labels

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

    def _place(self, f, label, override):
        """The target directory an override or a label resolves to, an
        override winning, or None when neither is set or the value
        resolves to no target."""
        if override is not None:
            chosen = override
        elif label is not None:
            chosen = label
        else:
            return None
        return self.target_of(chosen)


class GitTransformer:
    """The git and C and Make and meson tree diff. Owns root scope, the
    build-list patches, the validator, and rollback.

    The role and pairing decisions live in GitInterpreter, which hands
    diff() a target dir per file. This transformer computes the tree
    diff: a Rename per file whose target path differs from its current
    path (ok False for a program, which git builds from a path-derived
    name), plus a Patch per build-list file the ok renames reparent
    (Makefile LIB_OBJS and per-object rules, meson sources). Every moved
    header is internal, so the moves are pure git mv with no include
    rewrite."""

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

    # diff

    def diff(self, targets):
        """The tree diff from the current tree to the target tree.

        For each file whose target path differs from its current path,
        emit a Rename. A .c is ok when the Makefile builds it as one
        LIB_OBJS object; a program .c (not a single LIB_OBJS object) is
        not ok, with the HOLD_PROGRAM reason, so it stays at root. A
        header rides its source and is always ok (every moved header is
        internal, so it is a pure git mv). The ok renames entail the
        build-list PATCHES: one Patch per referrer file (Makefile,
        meson.build) that reparents the moved objects. Each build patch
        is unambiguous, so ok is True; the payload carries the moved
        basenames grouped by target for apply to consume."""
        renames = []
        for f, t in targets.items():
            dst = os.path.basename(f) if t == ROOT else f"{t}/{f}"
            if dst == f:
                continue               # already at its target path
            if f.endswith(".c") and not self._model.is_lib_object(f):
                renames.append(Rename(src=f, dst=dst, ok=False,
                                       reason=HOLD_PROGRAM))
            else:
                renames.append(Rename(src=f, dst=dst, ok=True, reason=""))
        renames.sort(key=lambda r: r.src)

        # The build-list patches the ok .c renames entail. Group the
        # moved .c basenames by target so a patch reparents each object
        # to its new directory.
        moved = defaultdict(list)      # target -> [basename, ...]
        for r in renames:
            if r.ok and r.src.endswith(".c"):
                moved[os.path.dirname(r.dst)].append(os.path.basename(r.src))
        patches = self._build_patches(dict(moved)) if moved else ()
        return Diff(renames=tuple(renames), patches=tuple(patches))

    def _build_patches(self, moved_by_target):
        """One Patch per build-list referrer the moved objects reparent.

        moved_by_target is {target_dir: [<name>.c, ...]}. The Makefile
        patch reparents LIB_OBJS, LOCALIZED_C_CORE, and the per-object
        triplet rules; the meson.build patch reparents the source
        lines. Each is unambiguous (a full-line edit matched once per
        name), so ok is True; the payload is the grouping the apply
        edit consumes."""
        n_c = sum(len(v) for v in moved_by_target.values())
        n_loc = sum(1 for v in moved_by_target.values()
                    for base in v if self._has_localized_core(base))
        make_lines = [f"{n_c} LIB_OBJS lines"]
        if n_loc:
            make_lines.append(f"{n_loc} LOCALIZED_C_CORE lines")
        return (
            Patch(path="Makefile", ok=True, reason="",
                  summary=f"Makefile: {', '.join(make_lines)}",
                  payload=("Makefile", moved_by_target)),
            Patch(path="meson.build", ok=True, reason="",
                  summary=f"meson.build: {n_c} source lines",
                  payload=("meson.build", moved_by_target)),
        )

    # apply

    def apply(self, diff, commit):
        """Perform the ok renames and the build patches, stage, and gate
        on the build. Every moved header is internal, so no include is
        rewritten and moved files stay byte-identical. Create each
        target dir and patch the build lists before any git mv, so a
        failure before a mv leaves a recoverable tree. Ignore the not-ok
        renames (the held programs)."""
        ok_renames = [r for r in diff.renames if r.ok]
        ok_patches = [p for p in diff.patches if p.ok]
        lines = []
        if not ok_renames:
            return ApplyResult(True, ["organize apply: nothing to do"])
        # Create each target directory the renames land in.
        for r in ok_renames:
            d = os.path.dirname(r.dst)
            if d and not os.path.isdir(os.path.join(TOP, d)):
                os.makedirs(os.path.join(TOP, d))
        # Patch the build lists, then git mv the files.
        for p in ok_patches:
            self._apply_patch(p)
        for r in ok_renames:
            res = subprocess.run(["git", "-C", TOP, "mv", r.src, r.dst],
                                 capture_output=True, text=True)
            if res.returncode:
                lines.append(f"organize apply: git mv {r.src} failed: "
                             f"{res.stderr.strip()}")
                lines.append(self.recover_hint())
                return ApplyResult(False, lines)
        git("add", "-u")
        v = self._verify_pure_renames(ok_renames)
        if not v.ok:
            lines.append(f"organize apply: {v.reason}")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        v = self.validate()
        if not v.ok:
            lines.append(f"organize apply: {v.reason}; nothing "
                         "committed.")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        n = len(ok_renames)
        held = sum(1 for r in diff.renames if not r.ok)
        lines.append(f"organize apply: pure renames + validator passed "
                     f"({n} files, {len(ok_patches)} build patches, "
                     f"{held} held).")
        if commit:
            subprocess.run(
                ["git", "-C", TOP, "commit", "-m",
                 f"organize: apply {n} renames"], check=True)
            lines.append("organize apply: committed.")
        else:
            lines.append("review, then commit, or "
                         + self.recover_hint())
        return ApplyResult(True, lines)

    def _apply_patch(self, patch):
        """Perform one build-list Patch: dispatch by its referrer path
        to the full-line edit for that file. The payload is (path,
        {target: [<name>.c, ...]}), the grouping diff() computed."""
        path, moved_by_target = patch.payload
        for target, names in moved_by_target.items():
            if path == "Makefile":
                self._patch_makefile(names, target)
                self._patch_object_rules(names, target)
            elif path == "meson.build":
                self._patch_meson(names, target)

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

    def _verify_pure_renames(self, ok_renames):
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
        bad = [r.dst for r in ok_renames
               if status.get(r.dst) != "R100"]
        if bad:
            return Verdict(False, "not pure renames (R100): "
                           + ", ".join(sorted(bad)[:5]))
        return Verdict(True, "")

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

    def _patch_makefile(self, moved_c, target):
        """Full-line edit Makefile for each moved .c: reparent the
        LIB_OBJS line and, when present, the LOCALIZED_C_CORE line. The
        LIB_OBJS edit preserves leading whitespace and must match
        exactly once per name, else it is a loud error. The
        LOCALIZED_C_CORE edit re-paths po/git-core sources; it exists
        for only some sources, so 0 or 1 match is fine."""
        p = os.path.join(TOP, "Makefile")
        text = open(p, encoding="utf-8").read()
        for c in moved_c:
            n = c[:-2]
            pat = re.compile(
                r'^([ \t]*)LIB_OBJS \+= ' + re.escape(n) + r'\.o$', re.M)
            new, k = pat.subn(
                r'\1LIB_OBJS += ' + target + '/' + n + '.o', text)
            if k != 1:
                sys.exit(f"organize apply: Makefile: expected 1 line "
                         f"for {n}, found {k}")
            text = new
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

    def _patch_meson(self, moved_c, target):
        """Full-line edit meson.build for each moved .c: reparent its
        source line. The edit preserves leading whitespace and must
        match exactly once per name, else it is a loud error."""
        p = os.path.join(TOP, "meson.build")
        text = open(p, encoding="utf-8").read()
        for c in moved_c:
            n = c[:-2]
            pat = re.compile(r"^([ \t]*)'" + re.escape(n) + r"\.c',$",
                             re.M)
            new, k = pat.subn(r"\1'" + target + "/" + n + ".c',", text)
            if k != 1:
                sys.exit(f"organize apply: meson.build: expected 1 line "
                         f"for {n}, found {k}")
            text = new
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
