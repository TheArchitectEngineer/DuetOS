//! cargo-fuzz target: `duetos_wifi80211_parse_beacon_body` — the
//! fixed 12-byte prefix of a Beacon / Probe Response body
//! (timestamp + beacon_interval + capability_info) sitting right
//! after the 24-byte MAC header.
#![no_main]

use duetos_wifi80211::{duetos_wifi80211_parse_beacon_body, DuetosWifiBeaconBody};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut out = DuetosWifiBeaconBody::default();
    // SAFETY: `data` is a valid slice, readable for its own length, for the
    // duration of this call. `out` is a stack-local `DuetosWifiBeaconBody`,
    // writable and non-aliasing with `data`, that outlives the call.
    unsafe {
        duetos_wifi80211_parse_beacon_body(data.as_ptr(), data.len(), &mut out as *mut _);
    }
});
