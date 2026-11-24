//! Verify every copied asset against its manifest SHA256, then pack what the binary needs into
//! deflated blobs in OUT_DIR. A tampered or mismatched asset fails the build, never the user.
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

fn sha256(bytes: &[u8]) -> String {
    Sha256::digest(bytes).iter().map(|b| format!("{b:02x}")).collect()
}

fn read(path: &Path) -> Vec<u8> {
    println!("cargo:rerun-if-changed={}", path.display());
    fs::read(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn json(path: &Path) -> serde_json::Value {
    serde_json::from_slice(&read(path)).unwrap()
}

fn verify(path: &Path, expected: &serde_json::Value) -> Vec<u8> {
    let bytes = read(path);
    let actual = sha256(&bytes);
    assert_eq!(Some(actual.as_str()), expected.as_str(), "checksum mismatch: {}", path.display());
    bytes
}

/// One .npy member of an .npz: (dtype, shape, little-endian data).
fn npz(path: &Path, bytes: &[u8]) -> Vec<(String, String, Vec<u64>, Vec<u8>)> {
    let mut archive = zip::ZipArchive::new(std::io::Cursor::new(bytes)).unwrap();
    let mut out = Vec::new();
    for i in 0..archive.len() {
        let mut file = archive.by_index(i).unwrap();
        let name = file.name().trim_end_matches(".npy").to_string();
        let mut raw = Vec::new();
        file.read_to_end(&mut raw).unwrap();
        assert_eq!(&raw[..6], b"\x93NUMPY", "{}: {name} is not .npy", path.display());
        let (len, start) = if raw[6] == 1 {
            (u16::from_le_bytes([raw[8], raw[9]]) as usize, 10)
        } else {
            (u32::from_le_bytes(raw[8..12].try_into().unwrap()) as usize, 12)
        };
        let header = std::str::from_utf8(&raw[start..start + len]).unwrap();
        assert!(header.contains("'fortran_order': False"), "{name}: fortran order");
        let descr = header.split("'descr': '").nth(1).unwrap().split('\'').next().unwrap().to_string();
        let shape = header.split("'shape': (").nth(1).unwrap().split(')').next().unwrap();
        let shape: Vec<u64> = shape.split(',').map(str::trim).filter(|s| !s.is_empty())
            .map(|s| s.parse().unwrap()).collect();
        let data = raw[start + len..].to_vec();
        let item: u64 = descr[2..].parse().unwrap();
        assert!(descr.starts_with('<') || descr.starts_with('|'), "{name}: big-endian {descr}");
        assert_eq!(data.len() as u64, shape.iter().product::<u64>() * item, "{name}: size");
        out.push((name, descr, shape, data));
    }
    out
}

/// Blob format: [u32 count] then per entry: u16 name length, name, u8 dtype length, dtype,
/// u8 ndim, u64 shape..., u64 byte length, bytes. All little-endian; deflated as a whole.
struct Blob(Vec<u8>, u32);

impl Blob {
    fn new() -> Self { Blob(Vec::new(), 0) }
    fn add(&mut self, name: &str, dtype: &str, shape: &[u64], data: &[u8]) {
        self.0.extend((name.len() as u16).to_le_bytes());
        self.0.extend(name.as_bytes());
        self.0.push(dtype.len() as u8);
        self.0.extend(dtype.as_bytes());
        self.0.push(shape.len() as u8);
        for s in shape { self.0.extend(s.to_le_bytes()); }
        self.0.extend((data.len() as u64).to_le_bytes());
        self.0.extend(data);
        self.1 += 1;
    }
    fn write(self, path: &Path) -> String {
        let mut raw = self.1.to_le_bytes().to_vec();
        raw.extend(self.0);
        let packed = miniz_oxide::deflate::compress_to_vec(&raw, 9);
        fs::write(path, &packed).unwrap();
        sha256(&raw)
    }
}

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("assets");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let bundle = root.join("bundle");
    let manifest = json(&bundle.join("manifest.json"));
    let files = manifest["files"].as_object().unwrap();
    let mut verified = std::collections::BTreeMap::new();
    for (name, digest) in files {
        verified.insert(name.clone(), verify(&bundle.join(name), digest));
    }
    let g2p_manifest = json(&root.join("g2p/manifest.json"));
    let g2p_files = &g2p_manifest["files"];
    assert_eq!(g2p_files["lexicon.txt"], files["lexicon.txt"], "G2P and bundle lexicons differ");
    assert_eq!(g2p_manifest["model_sha256"], manifest["model_sha256"], "G2P assets belong to another model");
    let g2p = |name: &str, key: &str| verify(&root.join("g2p").join(name), &g2p_files[key]);
    let checkpoint = g2p("checkpoint20.npz", "vendor/g2p_en/checkpoint20.npz");
    let homographs = g2p("homographs.en", "vendor/g2p_en/homographs.en");
    g2p("LICENSE.txt", "vendor/g2p_en/LICENSE.txt");

    // Evidence: both stores and the calibration pass's text, in the generator's order.
    let generator = json(&bundle.join("generator.json"));
    let mut evidence = Blob::new();
    for (i, store) in generator["stores"].as_array().unwrap().iter().enumerate() {
        let name = store["store"].as_str().unwrap();
        for (key, dtype, shape, data) in npz(Path::new(name), &verified[name]) {
            evidence.add(&format!("store{i}/{key}"), &dtype, &shape, &data);
        }
    }
    let text = format!("{}/text.npz", generator["text"].as_str().unwrap());
    for (key, dtype, shape, data) in npz(Path::new(&text), &verified[&text]) {
        evidence.add(&format!("text/{key}"), &dtype, &shape, &data);
    }
    let evidence_sha = evidence.write(&out.join("evidence.bin"));

    // Pronunciation: the lexicon, the neural G2P weights and the homograph tables.
    let lexicon = &verified["lexicon.txt"];
    let mut pron = Blob::new();
    pron.add("lexicon", "|u1", &[lexicon.len() as u64], lexicon);
    pron.add("homographs", "|u1", &[homographs.len() as u64], &homographs);
    let oov = read(&root.join("g2p/homograph_oov.json"));
    pron.add("homograph_oov", "|u1", &[oov.len() as u64], &oov);
    for (key, dtype, shape, data) in npz(Path::new("checkpoint20.npz"), &checkpoint) {
        pron.add(&format!("g2p/{key}"), &dtype, &shape, &data);
    }
    let pron_sha = pron.write(&out.join("pronunciation.bin"));
    fs::write(out.join("identity.rs"), format!(
        "pub const EVIDENCE_BLOB_SHA256: &str = \"{evidence_sha}\";\n\
         pub const PRONUNCIATION_BLOB_SHA256: &str = \"{pron_sha}\";\n\
         pub const G2P_ASSET_SHA256: &str = \"{}\";\n",
        sha256(&read(&root.join("g2p/manifest.json"))))).unwrap();
}
