//! git's labeler: emit `path \0 key \0 value` for every governed .c/.h.
//!
//! It mines each root source's subsystem from its commit history, folds in
//! the C role/pairing, and reconciles against the declared attributes (the
//! read-back). A declaration takes precedence; mining fills the gaps. It
//! writes nothing; the git organize builtin takes these labels and updates
//! .gitattributes. A placed file emits `subsystem <dir>`; a public
//! interface header emits `role public`.

use anyhow::Result;
use lexopt::prelude::*;
use organize::layout::AreaMap;
use organize::model::Model;
use organize::{git, labeler};
use std::io::{BufWriter, Write};
use std::path::Path;
use std::process::ExitCode;

const USAGE: &str = "git labeler: emit `path \\0 key \\0 value` per governed .c/.h.\n\
usage: labeler [--map FILE]\n";

fn run() -> Result<()> {
    let mut map_path: Option<String> = None;
    let mut parser = lexopt::Parser::from_env();
    while let Some(arg) = parser.next()? {
        match arg {
            Long("map") => map_path = Some(parser.value()?.string()?),
            Long("help") | Short('h') => {
                print!("{USAGE}");
                return Ok(());
            }
            other => return Err(other.unexpected().into()),
        }
    }

    let top = git::toplevel()?;
    let map_path = map_path.unwrap_or_else(|| {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("git-layout.map")
            .to_string_lossy()
            .into_owned()
    });
    let model = Model::new(top)?;
    let map = AreaMap::load(&map_path)?;
    let labels = labeler::all_labels(&model, &map)?;

    let mut rows: Vec<(&String, &String)> = labels.iter().collect();
    rows.sort();
    let stdout = std::io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    for (path, dir) in rows {
        if dir.is_empty() {
            writeln!(out, "{path}\0role\0public")?;
        } else {
            writeln!(out, "{path}\0subsystem\0{dir}")?;
        }
    }
    out.flush()?;
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            if let Some(io) = e.root_cause().downcast_ref::<std::io::Error>() {
                if io.kind() == std::io::ErrorKind::BrokenPipe {
                    return ExitCode::SUCCESS;
                }
            }
            eprintln!("{e:#}");
            ExitCode::FAILURE
        }
    }
}
