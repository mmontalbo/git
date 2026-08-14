"""reorg git adapter: the git and C and Make and meson triple.

Provides CommitPrefixSignal (label from commit subject prefixes),
MapPolicy (the area map with declared overrides), and
MakeMesonEnforcer (root scope, the move cascade, the build gate).
Importing this module registers the "git-c" triple. Every git, C,
Makefile, meson, include, nix, and .gitattributes string lives here.
"""
import os
import re
import sys
import subprocess
from collections import Counter, defaultdict

import reorg_core
from reorg_core import (Vote, Placement, Verdict, Step, Plan,
                        ApplyResult)

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


PRE = re.compile(r"^([A-Za-z0-9][\w./-]*):")


def norm(p):
    p = p.lower()
    return p[:-2] if p.endswith((".c", ".h")) else p


class CommitPrefixSignal:
    """Label each file by the modal commit-subject prefix over its
    history, weighted by full commit breadth so a large sweep counts
    little per file. Returns one Vote per confidently labelled file.

    Evidence-quality thresholds live here: at least 2.0 support and a
    0.34 modal share. A file below either is absent from the result."""

    def label(self, scope):
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


class MapPolicy:
    """The declared area map: 'directory: area area ...' per line, plus
    per-path 'area=' overrides. A declared override beats the inferred
    label. A label resolves to a target through its map token."""

    def __init__(self):
        self._owner = {}
        self._name = ""

    def load(self, mapref):
        self._name = os.path.basename(mapref)
        owner = {}
        for line in open(mapref, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if not line or ":" not in line:
                continue
            d, areas = line.split(":", 1)
            for a in areas.split():
                owner[a] = d.strip()
        self._owner = owner

    def name(self):
        return self._name

    def token_of(self, label):
        """The map token for a label: the first hyphen-free segment of
        the first path component."""
        return label.split("/")[0].split("-")[0]

    def overrides(self, scope):
        """path -> declared 'area=' value for scope paths that set it.

        Batches one 'git check-attr area --stdin -z'. The NUL stream is
        path, attr, value triples; keep values set and not
        'unspecified'."""
        files = sorted(scope)
        if not files:
            return {}
        r = subprocess.run(
            ["git", "-C", TOP, "check-attr", "area",
             "--stdin", "-z"],
            input="\0".join(files) + "\0",
            capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"git check-attr area: {r.stderr.strip()}")
        fields = r.stdout.split("\0")
        over = {}
        for i in range(0, len(fields) - 2, 3):
            path, _, value = fields[i], fields[i + 1], fields[i + 2]
            if value and value != "unspecified":
                over[path] = value
        return over

    def place(self, f, vote, override):
        if override is not None:
            label, reason = override, "override"
        elif vote is not None and vote.primary is not None:
            label, reason = vote.primary, "label"
        else:
            return Placement(target=None, label=None, reason="none")
        return Placement(target=self.target_of(label),
                         label=label, reason=reason)

    def target_of(self, label):
        return self._owner.get(self.token_of(label))

    def ordered_targets(self):
        return list(dict.fromkeys(self._owner.values()))


INCLUDE = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)


class MakeMesonEnforcer:
    """The git and C and Make and meson cascade. Owns root scope, the
    relocatability verdict, the include rewrite, the build-list edits,
    the build gate, and rollback."""

    def __init__(self):
        self._lib = None       # Counter of LIB_OBJS += <stem>.o lines
        self._make = None      # cached Makefile text

    # scope and readiness

    def scope(self):
        """Flat root *.c and *.h tracked files."""
        return {f for f in git("ls-files", "*.c", "*.h").split()
                if "/" not in f}

    def target_ready(self, target):
        return os.path.isdir(os.path.join(TOP, target))

    def already_placed(self, f, target):
        return os.path.dirname(f) == target

    def preflight(self):
        """Tracked-dirty is the one blocker. Untracked files do not
        block, so apply runs with untracked research dirs present and
        git reset --hard still restores the tree."""
        if self._tracked_dirty():
            return ["tracked changes present"]
        return []

    def recover_hint(self):
        return "recover the pre-apply tree with: git reset --hard"

    # relocatability

    def _makefile(self):
        if self._make is None:
            self._make = open(os.path.join(TOP, "Makefile"),
                              encoding="utf-8").read()
        return self._make

    def _is_lib_object(self, c):
        """Whether the Makefile builds c as exactly one libgit object,
        that is one 'LIB_OBJS += <stem>.o' line."""
        if self._lib is None:
            self._lib = Counter(re.findall(
                r'^[ \t]*LIB_OBJS \+= (\S+)\.o$',
                self._makefile(), re.M))
        return self._lib[stem(c)] == 1

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

    def _relocatable(self, c):
        """(ok, reason). A source moves when it is a single libgit
        object. A program (non-LIB_OBJS) cannot move because its
        program name derives from the object path. A per-object build
        rule does not block the move; apply rewrites the rule to the
        new path. reason is the standalone skip phrase; _paired_reason
        turns it into the paired-header phrase."""
        if not self._is_lib_object(c):
            return Verdict(
                False, "built as a program, not a libgit object")
        return Verdict(True, "")

    @staticmethod
    def _paired_reason(c, reason):
        """Fit a standalone skip reason into the paired-header note so
        it reads as a clause about the paired source."""
        if reason.startswith("built as"):
            return f"paired {c} is {reason}"
        return f"paired {c} {reason}"

    # the include resolver

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

    def _include_edits(self, h):
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
        """Build one area's Plan from the files the Policy placed here.

        The .c is the primary unit and pulls its same-stem .h. A
        non-relocatable .c is skipped and does not pull its header. A
        flagged .h whose .c is absent moves alone; whose .c is present
        but not moving is skipped with a reason."""
        flagged = {f for f in files
                   if not self.already_placed(f, target)}
        move_set, paired, skipped = set(), set(), []
        moving_c = set()
        for c in sorted(f for f in flagged if f.endswith(".c")):
            v = self._relocatable(c)
            if not v.ok:
                skipped.append((c, v.reason))
                continue
            moving_c.add(c)
        moving_stems = {stem(f) for f in moving_c}
        for c in moving_c:
            move_set.add(c)
            h = stem(c) + ".h"
            if exists(h):
                move_set.add(h)
                if h not in flagged:
                    paired.add(h)
        for h in flagged:
            if not h.endswith(".h"):
                continue
            c = stem(h) + ".c"
            if not exists(c):
                move_set.add(h)      # header-only lib, moves alone
            elif stem(h) not in moving_stems:
                skipped.append((h, self._skip_note(c)))
        moves = tuple(sorted((f, f"{target}/{f}") for f in move_set))
        headers = sorted(f for f in move_set if f.endswith(".h"))
        steps = self._steps(target, moves, paired, headers)
        notes = self._notes(target, headers, moves)
        return Plan(target=target, moves=moves,
                    skipped=tuple(sorted(skipped)),
                    steps=steps, notes=notes)

    def _skip_note(self, c):
        v = self._relocatable(c)
        if exists(c) and not v.ok:
            return self._paired_reason(c, v.reason)
        return f"paired {c} is unlabelled"

    def _steps(self, target, moves, paired, headers):
        """One move Step per pair, its summary the printed line, plus
        one edit Step per moved header. The core prints move summaries;
        the include and build detail rides in notes."""
        steps = []
        for src, dst in moves:
            mark = "   (paired header)" if src in paired else ""
            steps.append(Step(
                kind="move",
                summary=f"  {src:<10}  ->  {dst}{mark}",
                reads=(src,), writes=(dst,),
                preview=None, payload=(src, dst)))
        for h in headers:
            steps.append(Step(
                kind="edit", summary=f"rewrite includers of {h}",
                reads=(h,), writes=(),
                preview=None, payload=h))
        return tuple(steps)

    def _notes(self, target, headers, moves):
        """The include-rewrite and build-edit report lines the core
        prints verbatim after the moves."""
        rows, stale = [], 0
        for h in headers:
            n = len({f for f, _ in self._include_edits(h)})
            stale += self._nonsource_refs(h)
            rows.append((f'"{h}"', f'"{target}/{h}"', n))
        n_files = sum(c for _, _, c in rows)
        lines = ["", f"include rewrites: {len(rows)} headers, "
                 f"{n_files} including files"]
        for frm, to, c in rows:
            lines.append(f"  {frm:<12}  ->  {to}   ({c} files)")
        if stale:
            lines.append(
                f"  note: {stale} non-source references (docs, patch "
                "fixtures) are left as-is")
        n_c = sum(1 for s, _ in moves if s.endswith(".c"))
        lines += ["", "build edits:",
                  f"  Makefile:    {n_c} LIB_OBJS lines",
                  f"  meson.build: {n_c} source lines"]
        return tuple(lines)

    # apply

    def apply(self, plan, commit):
        """Rewrite includes and patch build lists, then git mv the
        pairs, stage, and gate on the build. Edit before moving so a
        failure before any git mv leaves a recoverable tree."""
        headers = [s for s, _ in plan.moves if s.endswith(".h")]
        moved_c = [os.path.basename(s) for s, _ in plan.moves
                   if s.endswith(".c")]
        lines = []
        self._rewrite_includes(headers, plan.target)
        self._patch_build(moved_c, plan.target)
        self._patch_object_rules(moved_c, plan.target)
        for src, dst in plan.moves:
            r = subprocess.run(["git", "-C", TOP, "mv", src, dst],
                               capture_output=True, text=True)
            if r.returncode:
                lines.append(f"reorg apply: git mv {src} failed: "
                             f"{r.stderr.strip()}")
                lines.append(self.recover_hint())
                return ApplyResult(False, lines)
        git("add", "-u")
        r = subprocess.run(self._build_cmd(), cwd=TOP)
        if r.returncode:
            lines.append("reorg apply: build failed; nothing "
                         "committed.")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        if commit:
            subprocess.run(
                ["git", "-C", TOP, "commit", "-m",
                 f"{plan.target}: carve out {plan.target}/"],
                check=True)
            lines.append(f"reorg apply: committed the "
                         f"{plan.target} carve.")
        else:
            lines.append("reorg apply: build passed; carve is "
                         "staged.")
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
                sys.exit(f"reorg apply: duplicate header {h}")
            seen.add(h)
            present = git("grep", "-l", f'#include "{target}/{h}"',
                          allow=(0, 1)).split()
            if present:
                sys.exit(f"reorg apply: '#include \"{target}/{h}\"' "
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
        Each edit preserves leading whitespace and must match exactly
        once per name per file, else it is a loud error."""
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
                    sys.exit(f"reorg apply: {path}: expected 1 line "
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
                sys.exit(f"reorg apply: Makefile: a per-object rule "
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
    return (CommitPrefixSignal(), MapPolicy(), MakeMesonEnforcer())


reorg_core.register("git-c", _make_git_c)
