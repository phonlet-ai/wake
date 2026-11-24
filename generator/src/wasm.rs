//! WebAssembly entry point: one JSON request in, one JSON response out, through linear memory.
//! No bindings crate; the host needs four exports.
//!
//! ```text
//! ptr = wt_alloc(n)          write n bytes of UTF-8 request JSON at ptr
//! len = wt_call(ptr, n)      runs the request and frees its buffer
//! out = wt_result()          len bytes of UTF-8 response JSON, valid until the next call
//! ```
//!
//! Requests: `{"command": "enroll", "phrases": ["hey kettle"]}` with the CLI's enroll options
//! (`endpoint`, `rate_loose`, `rate_strict`, `strict`, `min_phonemes`, `max_phonemes`), or the
//! commands `pronounce`, `info` and `load` (loads the calibration data ahead of the first phrase).
//! The response is what the CLI prints for the same command, or `{"error": "..."}`.
use crate::calibrate::Generator;
use crate::enroll::{self, Options, Quality};
use crate::resolver::{Item, Resolver};
use serde::Deserialize;
use serde_json::{Value, json};
use std::sync::{Mutex, OnceLock};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request {
    command: String,
    #[serde(default)]
    phrases: Vec<String>,
    #[serde(default)]
    endpoint: bool,
    #[serde(alias = "rate_a")]
    rate_loose: Option<f64>,
    #[serde(alias = "rate_b")]
    rate_strict: Option<f64>,
    #[serde(default)]
    strict: bool,
    min_phonemes: Option<usize>,
    max_phonemes: Option<usize>,
}

static GENERATOR: OnceLock<Generator> = OnceLock::new();
static RESOLVER: OnceLock<Resolver> = OnceLock::new();
static RESULT: Mutex<Vec<u8>> = Mutex::new(Vec::new());

fn resolver() -> Result<&'static Resolver, String> {
    if let Some(r) = RESOLVER.get() {
        return Ok(r);
    }
    let r = Resolver::load();
    enroll::check_identity(&r)?;
    Ok(RESOLVER.get_or_init(|| r))
}

fn generator() -> Result<&'static Generator, String> {
    if let Some(g) = GENERATOR.get() {
        return Ok(g);
    }
    let g = Generator::load(1)?;
    Ok(GENERATOR.get_or_init(|| g))
}

/// Like the CLI's positional phrases: each one is resolved, an empty one reports its own error.
fn items(phrases: &[String]) -> Result<Vec<Item>, String> {
    if phrases.is_empty() {
        return Err("give at least one phrase".into());
    }
    Ok(phrases.iter().map(|l| Item::parse(l)).collect())
}

fn handle(request: &[u8]) -> Result<Value, String> {
    let req: Request = serde_json::from_slice(request).map_err(|e| format!("request: {e}"))?;
    match req.command.as_str() {
        "load" => {
            resolver()?;
            generator()?;
            Ok(json!({"loaded": true}))
        }
        "info" => Ok(enroll::info(GENERATOR.get(), resolver()?)),
        "pronounce" => {
            let r = resolver()?;
            Ok(Value::Array(items(&req.phrases)?.iter().map(|item| match r.resolve(item, 64) {
                Ok(res) => serde_json::to_value(res).unwrap(),
                Err(e) => json!({"text": item.text(), "error": e}),
            }).collect()))
        }
        "enroll" => {
            let items = items(&req.phrases)?;
            let (r, g) = (resolver()?, generator()?);
            let defaults = Quality::default();
            let quality = Quality {
                min_phonemes: req.min_phonemes.unwrap_or(defaults.min_phonemes),
                max_phonemes: req.max_phonemes.unwrap_or(defaults.max_phonemes),
                strict: req.strict,
                ..defaults
            };
            let opts = Options { endpoint: req.endpoint, rate_a: req.rate_loose, rate_b: req.rate_strict, quality };
            serde_json::to_value(enroll::enroll(g, r, &items, &opts)?).map_err(|e| e.to_string())
        }
        other => Err(format!("unknown command {other:?} (enroll, pronounce, info, load)")),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn wt_alloc(len: usize) -> *mut u8 {
    let mut buf = Vec::<u8>::with_capacity(len);
    let ptr = buf.as_mut_ptr();
    std::mem::forget(buf);
    ptr
}

/// # Safety
/// `ptr` and `len` come from one `wt_alloc` call, with `len` bytes written; the buffer is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wt_call(ptr: *mut u8, len: usize) -> usize {
    let request = unsafe { Vec::from_raw_parts(ptr, len, len) };
    let value = handle(&request).unwrap_or_else(|e| json!({"error": e}));
    let mut out = RESULT.lock().unwrap();
    *out = serde_json::to_vec(&value).unwrap();
    out.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn wt_result() -> *const u8 {
    RESULT.lock().unwrap().as_ptr()
}
