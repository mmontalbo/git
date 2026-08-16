//! Shared logic for git's two organize binaries.
//!
//! `labeler` mines each governed .c/.h for its subsystem, folds in the C
//! role/pairing, and reconciles against the declared attributes (the
//! read-back). It emits `path \0 key \0 value` labels. `organizer` reads
//! labels and emits the mv and reference-patch operations. Neither writes
//! .gitattributes; the git organize builtin owns that.

pub mod git;
pub mod labeler;
pub mod layout;
pub mod model;
pub mod transform;
pub mod types;
pub mod util;
