use clap::{Args, Parser, Subcommand};
use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::ExitCode;
use wake_threshold::calibrate::Generator;
use wake_threshold::enroll::{self, Options, Quality};
use wake_threshold::resolver::{Item, Resolver};

/// Calibrated wake-word thresholds for phrases, for the Phonlet Wake en-1 model.
///
/// Writes a keyword file that every Phonlet Wake SDK reads as it is:
///   {"model": {"id": "en-1", "sha256": ...},
///    "keywords": [{"name", "pronunciations", "threshold_loose", "threshold_strict",
///                  "endpoint", "metadata"}, ...],
///    "metadata": {"generator", "rate_loose", "rate_strict", "unknown", "resolved"}}
/// Each phrase's metadata holds diagnostics (pronunciation source, expected threshold error,
/// warnings); the engine never reads them.
///
/// The command name `enroll` may be left out. Words missing from the dictionary get a predicted
/// pronunciation (metadata.source "g2p"); give explicit ARPAbet phones to override it.
///
/// Exit codes: 0 at least one phrase enrolled, 1 none enrolled, 2 usage or input error.
///
/// Examples:
///   wake-threshold "hey computer"
///   wake-threshold "hey computer" jarvis --out keywords.json
///   wake-threshold "snowboy=S N OW B OY|S N OW B OW"
///   wake-threshold pronounce snowboy
#[derive(Parser)]
#[command(name = "wake-threshold", version, verbatim_doc_comment)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Enroll phrases into a keyword file: pronunciations plus calibrated thresholds (the default)
    Enroll(EnrollArgs),
    /// Show how phrases are pronounced, without thresholds (fast)
    Pronounce(PhraseArgs),
    /// Show the embedded model, bundle and policy identities
    Info(OutArgs),
}

#[derive(Args)]
struct OutArgs {
    /// Write the JSON to this file instead of standard output
    #[arg(short, long)]
    out: Option<PathBuf>,
    /// Single-line JSON
    #[arg(long)]
    compact: bool,
}

#[derive(Args)]
struct PhraseArgs {
    /// Phrases: plain text, or 'word=PH PH ...|PH PH ...' with explicit ARPAbet pronunciations
    phrases: Vec<String>,
    /// Read more phrases from a file, one per line ('-' for standard input)
    #[arg(short, long)]
    file: Option<PathBuf>,
    #[command(flatten)]
    out: OutArgs,
}

#[derive(Args)]
struct EnrollArgs {
    #[command(flatten)]
    phrases: PhraseArgs,
    /// Fire only once the speech after the phrase stops (detection option; thresholds unchanged)
    #[arg(long)]
    endpoint: bool,
    /// False alarms per hour of speech at sensitivity 1, the loose end (default 10)
    #[arg(long, alias = "rate-a")]
    rate_loose: Option<f64>,
    /// False alarms per hour of speech at sensitivity 0, the strict end (default 0.1)
    #[arg(long, alias = "rate-b")]
    rate_strict: Option<f64>,
    /// Composition threads (default: all cores)
    #[arg(long)]
    threads: Option<usize>,
    /// Reject phrases with quality warnings (they go to "unknown") instead of only warning
    #[arg(long)]
    strict: bool,
    /// Warn when a pronunciation has fewer phonemes
    #[arg(long, default_value_t = 4)]
    min_phonemes: usize,
    /// Warn when a pronunciation has more phonemes
    #[arg(long, default_value_t = 12)]
    max_phonemes: usize,
}

fn phrases(args: &PhraseArgs) -> Result<Vec<Item>, String> {
    let mut lines = args.phrases.clone();
    if let Some(path) = &args.file {
        let mut text = String::new();
        if path.as_os_str() == "-" {
            std::io::stdin().read_to_string(&mut text).map_err(|e| e.to_string())?;
        } else {
            text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
        }
        lines.extend(text.lines().filter(|l| !l.trim().is_empty()).map(str::to_string));
    }
    if lines.is_empty() {
        return Err("give at least one phrase (or --file)".into());
    }
    Ok(lines.iter().map(|l| Item::parse(l)).collect())
}

fn emit(value: &impl serde::Serialize, out: &OutArgs) -> Result<(), String> {
    let mut text = if out.compact { serde_json::to_string(value) } else { serde_json::to_string_pretty(value) }
        .map_err(|e| e.to_string())?;
    text.push('\n');
    match &out.out {
        Some(path) => {
            std::fs::write(path, text).map_err(|e| format!("{}: {e}", path.display()))?;
            eprintln!("wrote {}", path.display());
        }
        None => std::io::stdout().write_all(text.as_bytes()).map_err(|e| e.to_string())?,
    }
    Ok(())
}

fn load_resolver() -> Result<Resolver, String> {
    let resolver = Resolver::load();
    enroll::check_identity(&resolver)?;
    Ok(resolver)
}

fn run(cli: Cli) -> Result<bool, String> {
    match cli.command {
        Command::Info(out) => {
            let resolver = load_resolver()?;
            emit(&enroll::info(None, &resolver), &out)?;
        }
        Command::Pronounce(args) => {
            let items = phrases(&args)?;
            let resolver = load_resolver()?;
            let results: Vec<serde_json::Value> = items.iter().map(|item| match resolver.resolve(item, 64) {
                Ok(r) => serde_json::to_value(r).unwrap(),
                Err(e) => serde_json::json!({"text": item.text(), "error": e}),
            }).collect();
            emit(&results, &args.out)?;
        }
        Command::Enroll(args) => {
            let items = phrases(&args.phrases)?;
            let threads = args.threads.unwrap_or_else(|| std::thread::available_parallelism().map_or(4, |n| n.get()));
            let (generator, resolver) = std::thread::scope(|s| {
                let g = s.spawn(|| Generator::load(threads));
                let r = load_resolver();
                (g.join().unwrap(), r)
            });
            let (generator, resolver) = (generator?, resolver?);
            let quality = Quality { min_phonemes: args.min_phonemes, max_phonemes: args.max_phonemes,
                                    strict: args.strict, ..Quality::default() };
            let opts = Options { endpoint: args.endpoint, rate_a: args.rate_loose, rate_b: args.rate_strict, quality };
            let response = enroll::enroll(&generator, &resolver, &items, &opts)?;
            for kw in &response.keywords {
                for warning in &kw.metadata.warnings {
                    eprintln!("warning: {}: {}", kw.name, warning.message);
                }
            }
            for u in &response.metadata.unknown {
                eprintln!("not enrolled: {}: {}", u.text, u.error);
            }
            emit(&response, &args.phrases.out)?;
            return Ok(!response.keywords.is_empty());
        }
    }
    Ok(true)
}

fn main() -> ExitCode {
    // `wake-threshold "hey computer"` means enroll.
    let mut argv: Vec<String> = std::env::args().collect();
    let commands = ["enroll", "pronounce", "info", "help", "-h", "--help", "-V", "--version"];
    if argv.len() > 1 && !commands.contains(&argv[1].as_str()) {
        argv.insert(1, "enroll".into());
    }
    match run(Cli::parse_from(argv)) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::from(2)
        }
    }
}
