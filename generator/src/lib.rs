//! Phonlet Wake threshold generator: text phrase -> calibrated keyword thresholds for the en-1
//! model, from the embedded evidence bundle. A port of Phonlet's Python enrollment reference.
pub mod assets;
pub mod calibrate;
pub mod enroll;
pub mod g2p;
pub mod numeric;
pub mod par;
pub mod resolver;
pub mod rng;
pub mod text;
#[cfg(target_arch = "wasm32")]
pub mod wasm;
pub mod world;
