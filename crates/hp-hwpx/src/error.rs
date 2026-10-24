//! Codec errors.

use std::io;
use std::path::PathBuf;

/// HWPX codec result.
pub type Result<T> = std::result::Result<T, Error>;

/// Errors produced while reading or writing HWPX packages.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// I/O failure.
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),

    /// ZIP archive error.
    #[error("ZIP error: {0}")]
    Zip(#[from] zip::result::ZipError),

    /// Missing required package entry.
    #[error("missing required HWPX entry: {0}")]
    MissingEntry(String),

    /// Invalid or unexpected mimetype.
    #[error("invalid HWPX mimetype (expected application/hwp+zip): {0:?}")]
    InvalidMimetype(String),

    /// XML parse/serialize failure.
    #[error("XML error: {0}")]
    Xml(String),

    /// Package structure / semantic error.
    #[error("invalid HWPX package: {0}")]
    InvalidPackage(String),

    /// Path-related error with context.
    #[error("{context}: {path}")]
    Path {
        /// What failed.
        context: &'static str,
        /// Path involved.
        path: PathBuf,
        /// Underlying I/O error.
        #[source]
        source: io::Error,
    },
}
