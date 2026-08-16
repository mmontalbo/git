//! Shared records and constants for the op stream.

use std::collections::BTreeMap;

/// The repository root as a target directory: a file whose target dir is
/// ROOT stays at the top (a public interface header kept at the root).
pub const ROOT: &str = "";

/// The reason the transformer holds a program rename: git builds the
/// program from a path-derived name, so moving the source breaks the
/// build.
pub const HOLD_PROGRAM: &str = "built as a program, not a libgit object";

/// A file moves to a new path, content identical (R100). `ok` is false
/// when the move cannot be performed (a conflict); `reason` states why.
pub struct Rename {
    pub src: String,
    pub dst: String,
    pub ok: bool,
    pub reason: String,
}

/// The tree diff: the renames plus the moved `.c` basenames grouped by
/// target directory (the grouping the Makefile and meson patches
/// reparent).
pub struct Diff {
    pub renames: Vec<Rename>,
    pub moved: BTreeMap<String, Vec<String>>,
}
