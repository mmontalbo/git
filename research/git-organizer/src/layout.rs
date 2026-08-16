//! The declared area map (`git-layout.map`): "directory: token token".

use anyhow::{Context, Result};
use std::collections::HashMap;
use std::fs;

pub struct AreaMap {
    owner: HashMap<String, String>, // token -> directory
    ordered: Vec<String>,           // unique directories, in map order
}

impl AreaMap {
    pub fn load(mapref: &str) -> Result<AreaMap> {
        let content = fs::read_to_string(mapref)
            .with_context(|| format!("organize: no such map file: {mapref}"))?;
        let mut owner = HashMap::new();
        let mut ordered: Vec<String> = Vec::new();
        for line in content.lines() {
            let line = line.split('#').next().unwrap_or("").trim();
            let Some((dir, areas)) = line.split_once(':') else {
                continue;
            };
            let dir = dir.trim();
            for token in areas.split_whitespace() {
                owner.insert(token.to_string(), dir.to_string());
            }
            if !ordered.iter().any(|d| d == dir) {
                ordered.push(dir.to_string());
            }
        }
        Ok(AreaMap { owner, ordered })
    }

    /// The map token for a label: the first hyphen-free segment of the
    /// first path component.
    fn token_of(label: &str) -> &str {
        label.split('/').next().unwrap_or(label).split('-').next().unwrap_or(label)
    }

    /// A label to its target directory: the directory name itself, a map
    /// token, or None.
    pub fn target_of(&self, label: &str) -> Option<String> {
        if self.ordered.iter().any(|d| d == label) {
            return Some(label.to_string());
        }
        self.owner.get(Self::token_of(label)).cloned()
    }
}
