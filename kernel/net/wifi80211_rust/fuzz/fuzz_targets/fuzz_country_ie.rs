//! cargo-fuzz target: `duetos_wifi80211_parse_country_ie` — the
//! 802.11d Country Information Element payload decoder (alpha2 +
//! environment + up to 16 stored sub-band triplets; operating-class
//! triplets, first_channel >= 201, are parsed but discarded).
#![no_main]

use duetos_wifi80211::{duetos_wifi80211_parse_country_ie, DuetosWifiCountryIe};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut out = DuetosWifiCountryIe::default();
    // SAFETY: `data` is a valid slice, readable for its own length, for the
    // duration of this call. `out` is a stack-local `DuetosWifiCountryIe`,
    // writable and non-aliasing with `data`, that outlives the call.
    unsafe {
        duetos_wifi80211_parse_country_ie(data.as_ptr(), data.len(), &mut out as *mut _);
    }
});
