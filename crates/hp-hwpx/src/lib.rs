//! HWPX (OWPML / KS X 6101) package codec for HangulPad.
//!
//! Reads and writes minimal valid HWPX ZIP packages through [`hp_ir::Document`].
//! OSS libraries are reference only — this crate owns the codec.

#![deny(missing_docs)]

mod error;
mod package;
mod read;
mod write;
mod xml_ns;

pub use error::{Error, Result};
pub use package::{is_hwpx_mimetype, looks_like_hwpx, MIMETYPE};
pub use read::read_path;
pub use write::write_path;

/// Round-trip: load HWPX → IR → write HWPX.
pub fn roundtrip(
    input: impl AsRef<std::path::Path>,
    output: impl AsRef<std::path::Path>,
) -> Result<hp_ir::Document> {
    let doc = read_path(input)?;
    write_path(&doc, output)?;
    Ok(doc)
}
