//! cargo-fuzz target: `duetos_wifi80211_parse_ie` — one
//! Information-Element (tag/length/payload) step of the IE-list
//! walker used over a Beacon / Probe Response body.
#![no_main]

use duetos_wifi80211::{duetos_wifi80211_parse_ie, DuetosWifiIe};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    // The FFI call takes both a buffer and a start offset into it. The
    // first native-pointer-width bytes select `off`; the remainder is the
    // buffer `parse_ie` walks. Inputs shorter than that width fall back to
    // offset 0 over the whole input, so they're still exercised rather
    // than skipped.
    let width = core::mem::size_of::<usize>();
    let (off, buf): (usize, &[u8]) = if data.len() >= width {
        let mut off = 0usize;
        for (shift, byte) in data[..width].iter().enumerate() {
            off |= (*byte as usize) << (shift * 8);
        }
        (off, &data[width..])
    } else {
        (0, data)
    };

    let mut out = DuetosWifiIe::default();
    // SAFETY: `buf` is a valid slice, readable for its own length, for the
    // duration of this call. `out` is a stack-local `DuetosWifiIe`, writable
    // and non-aliasing with `buf`, that outlives the call. `off` is
    // attacker-controlled input — `parse_ie`'s own bounds checks against
    // `buf.len()` are exactly the surface this target fuzzes.
    unsafe {
        duetos_wifi80211_parse_ie(buf.as_ptr(), buf.len(), off, &mut out as *mut _);
    }
});
