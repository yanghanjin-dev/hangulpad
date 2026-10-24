//! HangulPad engine CLI — Phase 1 focuses on HWPX roundtrip.

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};

#[derive(Parser, Debug)]
#[command(
    name = "hp",
    version,
    about = "HangulPad engine CLI (HWPX first; HWP read stub)"
)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand, Debug)]
enum Commands {
    /// Read a document and print plain text extracted via IR.
    Open {
        /// Input path (`.hwpx` or `.hwp`).
        input: PathBuf,
    },
    /// Create a minimal HWPX from plain text (UTF-8).
    New {
        /// Output `.hwpx` path.
        output: PathBuf,
        /// Initial paragraph text (default: empty document).
        #[arg(short, long)]
        text: Option<String>,
        /// Document title metadata.
        #[arg(long)]
        title: Option<String>,
    },
    /// Save IR-backed rewrite: read HWPX, write HWPX (default save format).
    Save {
        /// Input `.hwpx`.
        input: PathBuf,
        /// Output `.hwpx`.
        output: PathBuf,
    },
    /// Roundtrip HWPX: input → IR → output, then print extracted text.
    Roundtrip {
        /// Input `.hwpx`.
        input: PathBuf,
        /// Output `.hwpx`.
        output: PathBuf,
    },
    /// Probe whether a path looks like HWP (CFB) or HWPX.
    Probe {
        /// Path to inspect.
        input: PathBuf,
    },
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    match cli.command {
        Commands::Open { input } => {
            let doc = open_any(&input)?;
            println!("{}", doc.plain_text());
        }
        Commands::New {
            output,
            text,
            title,
        } => {
            let mut doc = match text {
                Some(t) => hp_ir::Document::from_plain_text(t),
                None => hp_ir::Document::new(),
            };
            if let Some(title) = title {
                doc.meta.title = title;
            }
            hp_hwpx::write_path(&doc, &output)?;
            eprintln!("wrote {}", output.display());
        }
        Commands::Save { input, output } => {
            let doc = hp_hwpx::read_path(&input)?;
            hp_hwpx::write_path(&doc, &output)?;
            eprintln!("saved {}", output.display());
        }
        Commands::Roundtrip { input, output } => {
            let doc = hp_hwpx::roundtrip(&input, &output)?;
            println!("{}", doc.plain_text());
            eprintln!("roundtrip ok → {}", output.display());
        }
        Commands::Probe { input } => {
            if hp_hwpx::looks_like_hwpx(&input)? {
                println!("hwpx");
            } else if hp_hwp::looks_like_hwp(&input)? {
                println!("hwp-cfb");
            } else {
                println!("unknown");
            }
        }
    }
    Ok(())
}

fn open_any(path: &std::path::Path) -> Result<hp_ir::Document, Box<dyn std::error::Error>> {
    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    match ext.as_str() {
        "hwpx" => Ok(hp_hwpx::read_path(path)?),
        "hwp" => Ok(hp_hwp::read_path(path)?),
        _ => {
            if hp_hwpx::looks_like_hwpx(path)? {
                Ok(hp_hwpx::read_path(path)?)
            } else if hp_hwp::looks_like_hwp(path)? {
                Ok(hp_hwp::read_path(path)?)
            } else {
                Err(format!("unsupported or unrecognized file: {}", path.display()).into())
            }
        }
    }
}
