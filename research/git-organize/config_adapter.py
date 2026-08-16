"""organize config adapter: a domain-neutral Interpreter and Transformer.

One adapter that fills the two seams from a JSON config plus command
call-outs, so a project reorganizes with no Python. It holds no domain
literal: no build command, no language suffix, no attribute file name,
no reference syntax. Every such specific comes from the config file,
which names the map, the members glob, the tag command, the reference
patterns, and the validate command.

This is an adapter, not the pure core, so it runs subprocess, touches
the filesystem, compiles regexes, reads JSON, and expands globs. What
it never does is name a domain: read the config for that.

The config keys (JSON):
  name          the adapter (pair) name to register.
  map           path to a "dir: token token" layout map file.
  members       list of globs for the files in scope.
  tagCmd        optional command implementing the tag protocol; stdin
                is NUL-separated paths, stdout lines "path<TAB>key=value"
                with keys area, role, kind. Absent: a file's area is the
                map token that appears in its path.
  overridesCmd  optional command giving declared overrides, highest
                precedence; stdout lines "path<TAB>area=X".
  relocateCmd   optional predictive check; stdin paths, stdout
                "path<TAB>ok" or "path<TAB>reason". Absent: all movable.
                Its reasons become the requirement conflicts.
  references    list of reference patterns with {path}/{stem}/{title}
                placeholders; on apply, a line matching a pattern that
                names a moved file is repointed to the new path.
  validate      command run in the tree after staging (exit 0 = valid);
                absent: no validation.
  root          the tree root the adapter governs; defaults to the
                config file's directory.
"""
import glob
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

import organize_core
from organize_core import (Vote, Placement, Verdict, Rename, Patch, Diff,
                          ApplyResult, ROOT)


def _load_config(path):
    """Read the JSON config and resolve its relative paths against the
    config file's directory. root defaults to that directory."""
    with open(path, encoding="utf-8") as fh:
        cfg = dict(json.load(fh))
    base = os.path.dirname(os.path.abspath(path))
    cfg["_base"] = base
    root = cfg.get("root")
    cfg["root"] = os.path.abspath(os.path.join(base, root)) if root else base
    # The map is passed to the core, which reads it verbatim, so resolve
    # it to an absolute path against the config's directory here.
    if cfg.get("map"):
        cfg["map"] = os.path.abspath(os.path.join(base, cfg["map"]))
    return cfg


def _resolve(cfg, value):
    """A config path or command, resolved against the config directory.

    A bare word that names a file next to the config becomes its
    absolute path; anything else (an absolute path, a multi-word command,
    a PATH executable) passes through untouched, so `validate: linkcheck`
    and `tagCmd: ./tag.sh` both work."""
    if value is None:
        return None
    parts = shlex.split(value)
    if len(parts) == 1:
        cand = os.path.join(cfg["_base"], parts[0])
        if os.path.exists(cand):
            return [os.path.abspath(cand)]
    if len(parts) == 1 and (os.sep in parts[0] or parts[0].startswith(".")):
        return [os.path.join(cfg["_base"], parts[0])]
    return parts


def _run_batch(argv, paths, root):
    """The batched command protocol: write NUL-separated paths to the
    command's stdin once, read "path<TAB>result" lines from its stdout.

    Returns {path: result}. A path the command does not name is absent
    from the map, which the caller reads as its default (movable, no
    tag). Exit nonzero is loud, so a broken call-out fails the run rather
    than silently dropping every file."""
    if not argv:
        return {}
    r = subprocess.run(argv, cwd=root, input="\0".join(sorted(paths)) + "\0",
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"organize: command {argv[0]} failed: {r.stderr.strip()}")
    out = {}
    for line in r.stdout.splitlines():
        if "\t" not in line:
            continue
        path, result = line.split("\t", 1)
        out[path] = result
    return out


class ConfigInterpreter:
    """Name each file's target directory from the tag command (or a
    map-token fallback) plus the declared layout map and
    command-supplied overrides, folded into one placement.

    The tag command speaks the tag protocol: for each file it emits zero
    or more "path<TAB>key=value" lines with keys area, role, kind. The
    area key is the placement label; role and kind ride in the vote's
    distribution. With no tag command, a file's area is the first map
    token whose name appears as a path component, so a plain map is
    enough for a flat tree. The map is "directory: token token" per
    line, the same format the git adapter's layout map uses; a token
    routes to its directory. An override, from the overrides command, is
    a declared area at highest precedence."""

    def __init__(self, cfg):
        self._cfg = cfg
        self._owner = {}
        self._name = cfg.get("name", "config layout")
        self._tag_cmd = _resolve(cfg, cfg.get("tagCmd"))
        self._over_cmd = _resolve(cfg, cfg.get("overridesCmd"))
        self._root = cfg["root"]

    # the Interpreter contract

    def targets(self, scope):
        """{FileId: DirId} for the files the label or an override names.

        A file with a label or a declared override that resolves to a
        target is present; one with neither, or one whose value resolves
        to no target, is absent. There is no role or pin: every member
        is a source, so a target is a plain subsystem directory."""
        votes = self._label(scope)
        over = self._overrides(scope)
        out = {}
        for f in scope:
            vote = votes.get(f)
            ov = over.get(f)
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
            d, tokens = line.split(":", 1)
            for tok in tokens.split():
                owner[tok] = d.strip()
        self._owner = owner

    def name(self):
        return self._name

    def target_of(self, label):
        """A label to its target directory. Accepts a map token, a
        multi-part label, or the destination directory name itself, so
        an override may name the directory. Unresolved returns None."""
        if label in self._owner.values():
            return label
        return self._owner.get(self._token_of(label))

    def ordered_targets(self):
        return list(dict.fromkeys(self._owner.values()))

    def token_for(self, target):
        """A label the map routes to target, for suggesting an override.
        Prefers the target's own name when the map owns it, else the
        first token that maps there."""
        if self._owner.get(target) == target:
            return target
        for tok, d in self._owner.items():
            if d == target:
                return tok
        return target

    # the label, override, and place internals

    def _tag_lines(self, scope):
        """{path: {key: value}} from the tag command, keys area/role/kind."""
        raw = _run_batch(self._tag_cmd, scope, self._root)
        tags = {}
        for path, kv in raw.items():
            if "=" in kv:
                key, value = kv.split("=", 1)
                tags.setdefault(path, {})[key.strip()] = value.strip()
        return tags

    def _label(self, scope):
        votes = {}
        if self._tag_cmd:
            for f, kv in self._tag_lines(scope).items():
                if f not in scope:
                    continue
                area = kv.get("area")
                if not area:
                    continue
                dist = {area: 1.0}
                for k in ("role", "kind"):
                    if kv.get(k):
                        dist[f"{k}:{kv[k]}"] = 1.0
                votes[f] = Vote(dist=dist, primary=area,
                                confidence=1.0, status="labelled")
            return votes
        # Fallback: the map token that appears in the file's path.
        for f in scope:
            token = self._token_in_path(f)
            if token:
                votes[f] = Vote(dist={token: 1.0}, primary=token,
                                confidence=1.0, status="labelled")
        return votes

    def _token_of(self, label):
        """The map token for a label: the first hyphen-free segment of
        the first path component, so guide/api and guide-v2 both reduce
        to guide."""
        return label.split("/")[0].split("-")[0]

    def _token_in_path(self, path):
        """The first map token that appears as a component of path, for
        the no-tag-command fallback. None when no token matches."""
        parts = re.split(r"[\\/._-]", path.lower())
        for tok in self._owner:
            if tok.lower() in parts:
                return tok
        return None

    def _overrides(self, scope):
        """{path: area} declared by the overrides command, for scope
        paths that set an area= value. Absent command: no overrides."""
        if not self._over_cmd:
            return {}
        over = {}
        for path, result in _run_batch(self._over_cmd, scope,
                                        self._root).items():
            if path not in scope or "=" not in result:
                continue
            key, value = result.split("=", 1)
            if key.strip() == "area" and value.strip():
                over[path] = value.strip()
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


class ConfigTransformer:
    """The generic tree diff: scope from member globs, relocatability
    from a command, and diff/apply built from the primitives.

    The primitives are mkdir the target, mv the file, repoint each
    reference the config's patterns match, run the validate command, and
    report. diff() emits a Rename per movable file (ok from the relocate
    command) and a Patch per referrer file the moves repoint, computed
    as a future before the writes; apply() performs the ok renames then
    writes the ok patches. Only the reference repoint reads a domain
    specific, and that specific is a pattern string in the config, not a
    literal here."""

    def __init__(self, cfg):
        self._cfg = cfg
        self._root = cfg["root"]
        self._members = cfg.get("members", [])
        self._reloc_cmd = _resolve(cfg, cfg.get("relocateCmd"))
        self._validate_cmd = _resolve(cfg, cfg.get("validate"))
        self._patterns = cfg.get("references", [])
        self._reloc = None         # cached {path: reason} from relocateCmd

    # scope and readiness

    def scope(self):
        """The files any member glob matches, as paths relative to the
        tree root. A recursive glob (one with a double-star segment)
        descends; a target directory a prior apply created is still in
        scope, so a second run sees a clean tree."""
        found = set()
        for pat in self._members:
            for p in glob.glob(os.path.join(self._root, pat),
                               recursive=True):
                if os.path.isfile(p):
                    found.add(os.path.relpath(p, self._root))
        return found

    def target_ready(self, target):
        return os.path.isdir(os.path.join(self._root, target))

    def already_at(self, f, target):
        return os.path.dirname(f) == target

    def is_source(self, f):
        """Every member is a source; the generic adapter has no riders.
        A domain that pairs files supplies a plugin."""
        return True

    def preflight(self):
        return []

    def recover_hint(self):
        return "recover the pre-apply tree by moving the files back"

    # relocatability

    def _relocatability(self, scope):
        """{path: reason} for the files the relocate command blocks.

        The command speaks the predictive-check protocol: a path it names
        with a reason cannot move (a requirement conflict); a path it
        names "ok", or does not name, is movable. Absent command: every
        file moves. Cached so plan runs it once."""
        if self._reloc is None:
            if not self._reloc_cmd:
                self._reloc = {}
            else:
                self._reloc = {
                    p: r for p, r in
                    _run_batch(self._reloc_cmd, scope, self._root).items()
                    if r.strip() and r.strip() != "ok"}
        return self._reloc

    # diff

    def diff(self, targets):
        """The tree diff from the current tree to the target tree.

        For each file whose target path (target dir joined with its
        basename, or the basename when the dir is ROOT) differs from its
        current path, emit a Rename: ok True when the relocate command
        clears the move, ok False with the command's reason when it
        blocks it (a requirement conflict).

        Then hold any otherwise-ok move that a referrer collides with by
        basename without a clean local link (a mechanical conflict the
        repointer cannot prove unrelated): re-emit it ok False naming the
        referrer and the offending reference. Finally compute the
        reference PATCHES the surviving ok renames entail: each referrer
        file whose text a config pattern rewrites to name a moved file's
        new path, as a Patch(ok=True) whose payload is the patched text
        future. A referrer that is itself moving is patched at its new
        path."""
        renames = []
        blocked = self._relocatability(self.scope())
        for f, t in sorted(targets.items()):
            dst = os.path.basename(f) if t == ROOT \
                else f"{t}/{os.path.basename(f)}"
            if dst == f:
                continue               # already at its target path
            reason = blocked.get(f)
            if reason:
                renames.append(Rename(src=f, dst=dst, ok=False,
                                       reason=reason))
            else:
                renames.append(Rename(src=f, dst=dst, ok=True, reason=""))
        # A requirement conflict already blocked some moves; the rest are
        # candidates for a mechanical conflict from a colliding reference.
        candidates = [(r.src, r.dst) for r in renames if r.ok]
        collisions = self._reference_collisions(candidates)
        held = set()
        final_renames = []
        for r in renames:
            if r.ok and r.src in collisions:
                held.add(r.src)
                final_renames.append(Rename(src=r.src, dst=r.dst, ok=False,
                                            reason=collisions[r.src]))
            else:
                final_renames.append(r)
        ok_moves = [(r.src, r.dst) for r in final_renames if r.ok]
        patches = self._reference_patches(ok_moves)
        return Diff(renames=tuple(final_renames), patches=tuple(patches))

    def _reference_patches(self, moves):
        """A Patch per referrer file whose text the config patterns
        rewrite to name a moved file's new path. Each patch is computed
        as a future from the file's current text and the moves, before
        any write; a referrer that is itself moving is patched at its
        new path (the reference-relative link is computed from there).
        Only a clean local link (no URL scheme, resolving to a moved
        file's old path) is rewritten, so ok is True; the payload is
        (read_path, new_text) for apply."""
        if not self._patterns or not moves:
            return ()
        # A moving referrer is read at its old path but written at its
        # new path, so links from it route from the new location.
        move_dst = {src: dst for src, dst in moves}
        files = self.scope() | {src for src, _dst in moves}
        patches = []
        for f in sorted(files):
            fp = os.path.join(self._root, f)
            if not os.path.isfile(fp):
                continue
            final = move_dst.get(f, f)
            text = open(fp, encoding="utf-8").read()
            # References resolve from where the file lives now (f); the
            # rewritten link routes from where it will live (final).
            new_text, hits = self._repoint_text(f, final, text, moves)
            if hits:
                patches.append(Patch(
                    path=final, ok=True, reason="",
                    summary=f"{final}: {hits} reference line(s)",
                    payload=(f, new_text)))
        return patches

    def _reference_collisions(self, moves):
        """{move_src: reason} for each candidate move a referrer collides
        with by basename but does not cleanly link to (a mechanical
        conflict). A reference collides with a move M when its basename
        equals M's old basename; it is clean for M only when it is a
        local link that resolves to M's old path. A collision that is not
        clean (a URL, or a local link that resolves elsewhere or nowhere)
        cannot be proven unrelated, so the whole move is held. Returns the
        first offending reference per move; a move absent from the result
        stays ok."""
        if not self._patterns or not moves:
            return {}
        old_base = {os.path.basename(src): src for src, _dst in moves}
        held = {}
        for f in sorted(self.scope()):
            fp = os.path.join(self._root, f)
            if not os.path.isfile(fp):
                continue
            text = open(fp, encoding="utf-8").read()
            for captured in self._captured_references(text):
                base = os.path.basename(captured)
                src = old_base.get(base)
                if src is None or src in held:
                    continue
                # A clean local link to this move's old path is fine; any
                # other same-basename reference holds the move.
                resolved = self._resolve_ref(f, captured)
                if resolved == src:
                    continue
                held[src] = self._collision_reason(f, captured)
        return held

    def _collision_reason(self, referrer, captured):
        """The held-move reason naming the referrer and the offending
        reference, distinguishing a URL from a mismatched local link."""
        if self._is_url(captured):
            kind = "colliding with this file but not a local link"
        else:
            kind = "colliding with this file but resolving elsewhere"
        return (f"a reference in {referrer} names {captured}, {kind}; "
                "move by hand after resolving it")

    def _captured_references(self, text):
        """Every path a config pattern captures in one file's text, in
        order, for the collision scan (both clean and colliding)."""
        found = []
        for template in self._patterns:
            regex = self._pattern_regex(template)
            if regex is None:
                continue
            for m in regex.finditer(text):
                found.append(m.group("path"))
        return found

    @staticmethod
    def _is_url(captured):
        """True when a captured reference is a URL, not a local path: it
        carries a scheme (http:, https:, mailto:) or is protocol-relative
        (starts with //). Such a reference names no file in the tree."""
        return (captured.startswith("//") or
                re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", captured) is not None)

    def _resolve_ref(self, referring_file, captured):
        """The old tree-relative path a captured reference names, or None
        when it names no local file. A URL, a protocol-relative link, or
        a pure anchor (#...) resolves to None. A local link resolves
        against the referring file's directory:
        normpath(join(dirname(referring_file), captured))."""
        if self._is_url(captured) or captured.startswith("#"):
            return None
        ref_dir = os.path.dirname(referring_file)
        joined = os.path.join(ref_dir, captured) if ref_dir else captured
        return os.path.normpath(joined)

    # apply

    def apply(self, diff, commit):
        """Perform the ok renames then write the ok patches, validate,
        and report. Create each target dir, os.rename each ok rename,
        then write each ok patch's precomputed text at its path (which
        now exists after the renames). Ignore the not-ok renames (the
        requirement conflicts). commit is accepted for the protocol;
        this adapter leaves the tree in place for review."""
        ok_renames = [r for r in diff.renames if r.ok]
        ok_patches = [p for p in diff.patches if p.ok]
        lines = []
        if not ok_renames:
            return ApplyResult(True, ["organize apply: nothing to do"])
        # Create every target first, so a mv never races an absent dir.
        for r in ok_renames:
            d = os.path.dirname(r.dst)
            if d:
                Path(self._root, d).mkdir(parents=True, exist_ok=True)
        # mv each file.
        for r in ok_renames:
            src_abs = os.path.join(self._root, r.src)
            dst_abs = os.path.join(self._root, r.dst)
            if os.path.exists(dst_abs):
                lines.append(f"organize apply: {r.dst} already exists; "
                             "refusing to overwrite")
                lines.append(self.recover_hint())
                return ApplyResult(False, lines)
            os.rename(src_abs, dst_abs)
        # Write each precomputed patch at its (now existing) path.
        for p in ok_patches:
            _read, new_text = p.payload
            open(os.path.join(self._root, p.path), "w",
                 encoding="utf-8").write(new_text)
        # validate the staged tree.
        v = self.validate()
        if not v.ok:
            lines.append(f"organize apply: {v.reason}; nothing committed.")
            lines.append(self.recover_hint())
            return ApplyResult(False, lines)
        lines.append(f"organize apply: moved {len(ok_renames)} files, "
                     f"repointed {len(ok_patches)} reference file(s).")
        if v.reason:
            lines.append(v.reason)
        lines.append("review the tree; the moves are on disk.")
        return ApplyResult(True, lines)

    def _pattern_regex(self, template):
        """Compile a reference template into a regex with a named 'path'
        group, escaping the literal text and turning the placeholders
        into captures. {path} and {stem} capture the referenced target;
        {title} captures free text (a link title). Returns the compiled
        regex, or None when the template names no path."""
        # Split on the placeholders, keeping them, so literal runs are
        # escaped and placeholders become groups.
        parts = re.split(r"(\{path\}|\{stem\}|\{title\})", template)
        out = []
        for part in parts:
            if part == "{path}":
                out.append(r"(?P<path>[^)\"'\s]+)")
            elif part == "{stem}":
                out.append(r"(?P<path>[^)\"'\s.]+)")
            elif part == "{title}":
                out.append(r"[^\]]*")
            elif part:
                out.append(re.escape(part))
        if "path" not in "".join(p for p in parts if p in
                                 ("{path}", "{stem}")):
            return None
        return re.compile("".join(out))

    def _repoint_text(self, read_from, write_to, text, moves):
        """Rewrite, in one file's text, every clean local link whose
        route changes. A reference is clean when it resolves (from
        read_from, where the file lives now) to a tree path; a URL or an
        anchor is left untouched. Its target's new path is the move
        destination when the target moves, else its old path (an unmoved
        target keeps its place). The replacement routes from write_to
        (where this file will live) to that new path, so a link stays
        relative and follows both endpoints. A link whose route is
        unchanged (neither endpoint moved relative to the other) is left
        byte-identical. Returns (new_text, hit_count)."""
        move_by_old = {src: dst for src, dst in moves}
        hits = [0]
        out_dir = os.path.dirname(write_to)

        def repoint_one(regex):
            def sub(m):
                captured = m.group("path")
                resolved = self._resolve_ref(read_from, captured)
                if resolved is None:
                    return m.group(0)     # a URL or an anchor
                # The target's path after apply: its destination when it
                # moves, else where it already sits (which is resolved).
                dst = move_by_old.get(resolved, resolved)
                new_rel = os.path.relpath(dst, out_dir) if out_dir else dst
                if new_rel == captured:
                    return m.group(0)     # route unchanged; leave as-is
                hits[0] += 1
                return m.group(0).replace(captured, new_rel)
            return sub

        new_text = text
        for template in self._patterns:
            regex = self._pattern_regex(template)
            if regex is None:
                continue
            new_text = regex.sub(repoint_one(regex), new_text)
        return new_text, hits[0]

    def validate(self):
        """Run the validate command in the tree; exit 0 means the
        artifact still holds. Absent command: valid with no check. A
        domain plugs its checker here (a link checker, a build)."""
        if not self._validate_cmd:
            return Verdict(True, "no validate command; skipped")
        r = subprocess.run(self._validate_cmd, cwd=self._root,
                           capture_output=True, text=True)
        if r.returncode:
            detail = (r.stderr or r.stdout).strip()
            return Verdict(False, f"validate failed: {detail}")
        return Verdict(True, "validate passed")


def register_config(path):
    """Load a config file and register its pair under the config's
    name. The launcher calls this for --config FILE; the core then
    dispatches to it like any other pair."""
    cfg = _load_config(path)
    name = cfg.get("name", "config")

    def build():
        interp = ConfigInterpreter(cfg)
        transformer = ConfigTransformer(cfg)
        return (interp, transformer)

    organize_core.register(name, build)
    return name, cfg


# The generic dispatch. The config adapter keeps its own status summary
# and all-areas apply so the "config layout clean" phrasing and the
# already/untracked counts stay stable; status <area> and apply --area X
# read from the diff, so they defer to the core unchanged.


def _config_status(interp, transformer, mapref):
    """The state summary for a config pair, built from the tree diff:
    renamed is the ok renames, conflict is the requirement conflicts the
    relocate command flags (the not-ok renames), clean is the addressed
    files already in place, untracked is the sources no rule addresses."""
    interp.load(mapref)
    scope = transformer.scope()
    targets = interp.targets(scope)
    diff = transformer.diff(targets)
    renamed_srcs = {r.src for r in diff.renames}
    ok = [r for r in diff.renames if r.ok]
    conflict_renames = [r for r in diff.renames if not r.ok]
    renamed = len(ok)
    conflict = len(conflict_renames)
    already = sum(1 for f in targets if f not in renamed_srcs)
    untracked = len(scope - set(targets))

    ok_by_area, conflict_by_area = {}, {}
    for r in diff.renames:
        area = targets.get(r.src)
        bucket = ok_by_area if r.ok else conflict_by_area
        bucket[area] = bucket.get(area, 0) + 1

    clean_layout = renamed == 0 and conflict == 0
    banner = "(layout clean)" if clean_layout else "(layout not clean)"
    print(f"organize status against {interp.name()}   {banner}\n")
    print(f"renamed    {renamed:<5} will move on apply")
    print(f"conflict   {conflict:<5} cannot auto-apply "
          "(requirement: a check blocks the move)")
    print(f"clean      {already:<5} already in their area")
    print(f"untracked  {untracked:<5} no rule places these\n")
    for t in interp.ordered_targets():
        n_move = ok_by_area.get(t, 0)
        n_skip = conflict_by_area.get(t, 0)
        if not (n_move or n_skip):
            continue
        state = "exists" if transformer.target_ready(t) else "new"
        print(f"  {t + '/':<14}{state:<7}{n_move:>3} move"
              f"{'':>4}{n_skip:>3} conflict")
    print("\napply --unconflicted moves every unconflicted file in one\n"
          "validated pass; status <area> shows one area's move set.")


def _config_apply_auto(interp, transformer, mapref):
    """All-areas apply for a config pair. Compute the whole tree's diff,
    then run it as one operation through the generic apply, so a
    cross-area reference repoints against the whole batch and validate
    sees the fully moved tree. Routes through the transformer directly."""
    interp.load(mapref)
    blockers = transformer.preflight()
    if blockers:
        sys.exit("organize apply --unconflicted: " + "; ".join(blockers))
    diff = organize_core._diff(interp, transformer)
    ok = [r for r in diff.renames if r.ok]
    held = [r for r in diff.renames if not r.ok]
    if not ok:
        print("organize apply --unconflicted: nothing to carve")
        return
    ok_patches = [p for p in diff.patches if p.ok]
    print(f"converging {len(ok)} files, patching {len(ok_patches)} "
          f"referrer files; holding {len(held)} conflict sources at "
          f"root\n")
    result = transformer.apply(diff, False)
    for line in result.lines:
        print(line)
    if not result.ok:
        sys.exit(1)


def dispatch(name, mapref, argv):
    """The launcher entry point for a config pair. Route the two
    git-coupled commands (the default status summary and
    apply --unconflicted) through the generic seams; defer every other
    command to the core, which handles a config pair unchanged.

    argv is the argument list with --config already removed. A --map
    override, if present, replaces the config's map."""
    probe, argv = list(argv), list(argv)
    map_override, probe = organize_core.take_opt(probe, "--map")
    if map_override is not None:
        mapref = map_override
    area, probe = organize_core.take_opt(probe, "--area")
    auto = "--unconflicted" in probe
    words = [a for a in probe if not a.startswith("-")]
    cmd = words[0] if words else "status"
    interp, transformer = organize_core.get_pair(name)
    flags = ("--by-area", "--conflicts", "--plan", "--exit-code")
    named_area = area or (words[1] if cmd == "status" and len(words) > 1
                          else None)
    if cmd == "status" and named_area is None \
            and not any(f in probe for f in flags):
        _config_status(interp, transformer, mapref)
        return
    if cmd == "apply" and auto:
        _config_apply_auto(interp, transformer, mapref)
        return
    # Every other command (status <area>, apply --area X, status
    # --by-area, status --plan) reads from the diff, so the core handles
    # it with no second signal. The core re-reads sys.argv, so leave it
    # untouched and pass the defaults.
    organize_core.main(default_pair=name, default_mapref=mapref)
