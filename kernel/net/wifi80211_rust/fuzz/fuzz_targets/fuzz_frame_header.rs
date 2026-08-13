//! cargo-fuzz target: `duetos_wifi80211_parse_frame_header` — the
//! 24-byte 802.11 MAC header decode (frame control byte, flags,
//! duration/ID, three MAC addresses, sequence control; rejects
//! extension frames per IEEE 802.11-2020 §9.2).
#![no_main]

use duetos_wifi80211::{duetos_wifi80211_parse_frame_header, DuetosWifiFrameHeader};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut out = DuetosWifiFrameHeader::default();
    // SAFETY: `data` is a valid slice, readable for its own length, for the
    // duration of this call. `out` is a stack-local `DuetosWifiFrameHeader`,
    // writable and non-aliasing with `data`, that outlives the call.
    unsafe {
        duetos_wifi80211_parse_frame_header(data.as_ptr(), data.len(), &mut out as *mut _);
    }
});
