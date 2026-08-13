//! cargo-fuzz target: `duetos_wifi80211_parse_eapol_key` — the
//! IEEE 802.1X-2010 §11.4 EAPOL-Key descriptor decode (4-way
//! handshake message): EAPOL header, 95-byte fixed descriptor
//! prefix, and the variable-length KeyData length gate.
#![no_main]

use duetos_wifi80211::{duetos_wifi80211_parse_eapol_key, DuetosWifiEapolKey};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut out = DuetosWifiEapolKey::default();
    // SAFETY: `data` is a valid slice, readable for its own length, for the
    // duration of this call. `out` is a stack-local `DuetosWifiEapolKey`,
    // writable and non-aliasing with `data`, that outlives the call.
    unsafe {
        duetos_wifi80211_parse_eapol_key(data.as_ptr(), data.len(), &mut out as *mut _);
    }
});
