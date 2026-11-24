//! The embedded, build-verified assets: JSON files verbatim, arrays in deflated blobs.
use std::collections::HashMap;

include!(concat!(env!("OUT_DIR"), "/identity.rs"));

pub const BUNDLE_MANIFEST: &str = include_str!("../assets/bundle/manifest.json");
pub const GENERATOR: &str = include_str!("../assets/bundle/generator.json");
pub const POLICY: &str = include_str!("../assets/bundle/policy.json");
pub const G2P_MANIFEST: &str = include_str!("../assets/g2p/manifest.json");
pub const META: [&str; 2] = [
    include_str!("../assets/bundle/evidence/heldout/meta.json"),
    include_str!("../assets/bundle/evidence/hunt/meta.json"),
];
const EVIDENCE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/evidence.bin"));
const PRONUNCIATION: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/pronunciation.bin"));

pub struct Array {
    pub dtype: String,
    pub shape: Vec<usize>,
    pub data: Vec<u8>,
}

impl Array {
    fn check(&self, dtype: &str) {
        assert_eq!(self.dtype, dtype, "embedded array has dtype {}, expected {dtype}", self.dtype);
    }
    pub fn u8(self) -> Vec<u8> {
        self.check("|u1");
        self.data
    }
    pub fn i64(&self) -> Vec<i64> {
        self.check("<i8");
        self.data.chunks_exact(8).map(|c| i64::from_le_bytes(c.try_into().unwrap())).collect()
    }
    pub fn f32(&self) -> Vec<f32> {
        self.check("<f4");
        self.data.chunks_exact(4).map(|c| f32::from_le_bytes(c.try_into().unwrap())).collect()
    }
    pub fn text(self) -> String {
        String::from_utf8(self.u8()).expect("embedded text is UTF-8")
    }
}

pub struct Blob(HashMap<String, Array>);

impl Blob {
    fn open(packed: &[u8]) -> Blob {
        let raw = miniz_oxide::inflate::decompress_to_vec(packed).expect("embedded blob inflates");
        let mut at = 4;
        let mut take = |n: usize| { at += n; &raw[at - n..at] };
        let count = u32::from_le_bytes(raw[..4].try_into().unwrap());
        let mut map = HashMap::new();
        for _ in 0..count {
            let n = u16::from_le_bytes(take(2).try_into().unwrap()) as usize;
            let name = String::from_utf8(take(n).to_vec()).unwrap();
            let n = take(1)[0] as usize;
            let dtype = String::from_utf8(take(n).to_vec()).unwrap();
            let ndim = take(1)[0] as usize;
            let shape = (0..ndim).map(|_| u64::from_le_bytes(take(8).try_into().unwrap()) as usize).collect();
            let n = u64::from_le_bytes(take(8).try_into().unwrap()) as usize;
            let data = take(n).to_vec();
            map.insert(name, Array { dtype, shape, data });
        }
        Blob(map)
    }

    pub fn take(&mut self, name: &str) -> Array {
        self.0.remove(name).unwrap_or_else(|| panic!("embedded array {name} missing"))
    }
}

pub fn evidence() -> Blob { Blob::open(EVIDENCE) }
pub fn pronunciation() -> Blob { Blob::open(PRONUNCIATION) }
