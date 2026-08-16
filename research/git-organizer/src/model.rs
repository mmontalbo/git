//! The git/C/Make model shared by the labeler and transformer: the
//! Makefile membership check and the include-graph reader.

use crate::git;
use crate::util::{basename, dirname, join, normpath, stem};
use anyhow::{Context, Result};
use regex::Regex;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::PathBuf;
use std::sync::LazyLock;

static LIB_OBJS_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^[ \t]*LIB_OBJS \+= (\S+)\.o$").unwrap());
static INCLUDE_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r#"(?m)^[ \t]*#[ \t]*include[ \t]+"([^"]+)""#).unwrap());

pub struct Model {
    pub top: PathBuf,
    lib: HashMap<String, usize>, // stem -> count of `LIB_OBJS += stem.o`
}

impl Model {
    pub fn new(top: PathBuf) -> Result<Model> {
        let makefile = fs::read_to_string(top.join("Makefile")).context("reading Makefile")?;
        let mut lib: HashMap<String, usize> = HashMap::new();
        for cap in LIB_OBJS_RE.captures_iter(&makefile) {
            *lib.entry(cap[1].to_string()).or_insert(0) += 1;
        }
        Ok(Model { top, lib })
    }

    /// Whether the Makefile builds `c` as exactly one libgit object.
    pub fn is_lib_object(&self, c: &str) -> bool {
        self.lib.get(stem(c)).copied().unwrap_or(0) == 1
    }

    pub fn exists(&self, path: &str) -> bool {
        self.top.join(path).is_file()
    }

    /// Whether `#include "arg"` in file `f` binds to the repo root header
    /// `h` (a local shadow such as `reftable/tree.h` does not count).
    fn binds_to_root(&self, f: &str, arg: &str, h: &str) -> bool {
        if basename(arg) != h {
            return false;
        }
        let local = normpath(&join(dirname(f), arg));
        if self.exists(&local) {
            return local == h;
        }
        normpath(arg) == h
    }

    /// `(file, arg)` for every tracked `*.c`/`*.h` include that binds to
    /// the root header `h`.
    pub fn include_edits(&self, h: &str) -> Result<Vec<(String, String)>> {
        let pat = format!(r#"#[ \t]*include[ \t]+"([^"]*/)?{}""#, regex::escape(h));
        let listed = git::run(&self.top, &["grep", "-lE", &pat, "--", "*.c", "*.h"], &[0, 1])?;
        let mut edits = Vec::new();
        for f in listed.split_whitespace() {
            let text = fs::read_to_string(self.top.join(f)).unwrap_or_default();
            for cap in INCLUDE_RE.captures_iter(&text) {
                if self.binds_to_root(f, &cap[1], h) {
                    edits.push((f.to_string(), cap[1].to_string()));
                }
            }
        }
        Ok(edits)
    }

    /// The candidate headers all of whose includers land in the header's
    /// own target directory, grown to a fixpoint.
    pub fn internal_headers(
        &self,
        cands: &HashSet<String>,
        home: &HashMap<String, String>,
        dir_of: &HashMap<String, String>,
    ) -> Result<HashSet<String>> {
        let mut includers: HashMap<String, HashSet<String>> = HashMap::new();
        for h in cands {
            let set = self.include_edits(h)?.into_iter().map(|(f, _)| f).collect();
            includers.insert(h.clone(), set);
        }
        let mut internal: HashSet<String> = HashSet::new();
        let mut changed = true;
        while changed {
            changed = false;
            let mut to_add = Vec::new();
            for h in cands {
                if internal.contains(h) {
                    continue;
                }
                let t = &home[h];
                let all = includers[h].iter().all(|f| {
                    dir_of.get(f) == Some(t) || (internal.contains(f) && home.get(f) == Some(t))
                });
                if all {
                    to_add.push(h.clone());
                }
            }
            if !to_add.is_empty() {
                changed = true;
                internal.extend(to_add);
            }
        }
        Ok(internal)
    }
}
