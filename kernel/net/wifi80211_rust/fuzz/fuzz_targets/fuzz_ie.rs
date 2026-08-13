//! cargo-fuzz target: `duetos_wifi80211_parse_ie` — one
//! Information-Element (tag/length/payload) step of the IE-list
//! walker used over a Beacon / Probe Response body.
#![no_main]

use duetos_wifi80211::{duetos_wifi80211_parse_ie, DuetosWifiIe};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    // The FFI call takes both a buffer and a start offset into it. The
    // first 4 bytes of the fuzz input select `off` (as a `u32`, so the
    // split stays host-pointer-width independent); the remainder is the
    // buffer `parse_ie` walks. Inputs shorter than 4 bytes fall back to
    // offset 0 over the whole input, so they're still exercised rather
    // than skipped.
    let (off, buf): (usize, &[u8]) = if data.len() >= 4 {
        let off = u32::from_le_bytes([data[0], data[1], data[2], data[3]]) as usize;
        (off, &data[4..])
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
