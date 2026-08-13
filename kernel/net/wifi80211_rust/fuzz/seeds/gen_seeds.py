#!/usr/bin/env python3
# DuetOS 802.11 (duetos_wifi80211) cargo-fuzz seed generator.
#
# WHAT  Emits deterministic, synthetic byte sequences for each of the
#       5 fuzz_targets/ binaries in this crate, matching the exact
#       frame/IE/EAPOL-Key shapes already exercised by the crate's own
#       `#[cfg(test)]` unit tests in ../src/lib.rs (both a well-formed
#       instance and a couple of the documented rejection edges per
#       parser). Every byte here is constructed from the public IEEE
#       802.11-2020 (frame format, §9.2 / §9.4.2.10) and IEEE 802.1X-2010
#       (EAPOL, §11.4) specs — no captured over-the-air traffic.
#
# WHY   A blind mutator starting from an empty corpus spends almost all
#       of its budget failing the first length/prefix gate in each
#       parser. Seeding one well-formed instance per parser puts the
#       fuzzer one mutation away from the interesting internal walkers
#       (the IE list, the country-IE triplet loop, the EAPOL body).
#
# USAGE  python3 gen_seeds.py [fuzz_crate_root]
#        (default: this script's parent directory, i.e. run as
#        `python3 seeds/gen_seeds.py` from kernel/net/wifi80211_rust/fuzz/)
#        Writes corpus/<target_name>/*.bin, cargo-fuzz's default
#        per-target corpus layout.

import os
import sys


def frame_header(fc0: int, flags: int = 0, addr1=b"\xff" * 6, addr2=b"\x02" * 6, addr3=b"\x02" * 6) -> bytes:
    buf = bytearray(24)
    buf[0] = fc0
    buf[1] = flags
    buf[2:4] = (0).to_bytes(2, "little")
    buf[4:10] = addr1
    buf[10:16] = addr2
    buf[16:22] = addr3
    buf[22:24] = (0).to_bytes(2, "little")
    return bytes(buf)


def beacon_with_ssid(ssid: bytes) -> bytes:
    # 24-byte MAC header (subtype=Beacon) + 12-byte fixed body + SSID IE
    # (id=0) + DS Parameter Set IE (id=3, len=1, channel=6). Mirrors
    # `make_beacon_with_ssid` in ../src/lib.rs's test module.
    buf = bytearray(frame_header(0x80))
    buf += (0x1234_5678).to_bytes(8, "little")  # timestamp
    buf += (100).to_bytes(2, "little")  # beacon_interval
    buf += (0x0011).to_bytes(2, "little")  # capability_info
    ie_off = len(buf)
    buf += bytes([0, len(ssid)]) + ssid  # SSID IE
    buf += bytes([3, 1, 6])  # DS Parameter Set IE
    assert ie_off == 36
    return bytes(buf)


def eapol_key(body_overrides: dict = None) -> bytes:
    # Mirrors `make_eapol_key` in ../src/lib.rs's test module: EAPOL
    # header (version 2, type EAPOL-Key, 95-byte body length) + the
    # 95-byte fixed EAPOL-Key descriptor prefix (nonce/iv/rsc/reserved/
    # mic left zero-filled).
    header = bytes([2, 0x03]) + (95).to_bytes(2, "big")
    body = bytearray(95)
    body[0] = 2  # Key Descriptor Type: RSN
    body[1:3] = (0x008A).to_bytes(2, "big")  # Key Info
    body[3:5] = (16).to_bytes(2, "big")  # Key Length
    body[5:13] = (1).to_bytes(8, "big")  # Replay Counter
    body[93:95] = (0).to_bytes(2, "big")  # Key Data Length
    for off, val in (body_overrides or {}).items():
        body[off : off + len(val)] = val
    return header + bytes(body)


def country_ie_triplets(prefix: bytes, triplets) -> bytes:
    buf = bytearray(prefix)
    for t in triplets:
        buf += bytes(t)
    return bytes(buf)


def write(root: str, target: str, name: str, data: bytes) -> None:
    out_dir = os.path.join(root, "corpus", target)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, name), "wb") as fh:
        fh.write(data)


def main() -> None:
    default_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = sys.argv[1] if len(sys.argv) > 1 else default_root

    # --- fuzz_frame_header ---
    write(root, "fuzz_frame_header", "beacon.bin", frame_header(0x80))
    write(root, "fuzz_frame_header", "data_frame.bin", frame_header(0x08))
    write(root, "fuzz_frame_header", "extension_frame.bin", frame_header(0b0000_1100))
    write(root, "fuzz_frame_header", "too_short.bin", bytes(10))

    # --- fuzz_beacon_body ---
    beacon = beacon_with_ssid(b"DUETSSID")
    write(root, "fuzz_beacon_body", "beacon_with_ssid.bin", beacon)
    write(root, "fuzz_beacon_body", "minimal_no_ie.bin", bytes(36))
    write(root, "fuzz_beacon_body", "too_short.bin", bytes(30))

    # --- fuzz_ie --- (first 4 bytes = little-endian u32 offset, rest = buffer)
    ssid_off = (36).to_bytes(4, "little")
    write(root, "fuzz_ie", "ssid_ie.bin", ssid_off + beacon)
    truncated_off = (0).to_bytes(4, "little")
    write(root, "fuzz_ie", "truncated_ie.bin", truncated_off + bytes([0, 10, 1, 2, 3]))

    # --- fuzz_country_ie --- (payload only, no element-id/length header)
    write(root, "fuzz_country_ie", "minimal.bin", b"USI")
    write(root, "fuzz_country_ie", "subband_triplet.bin", country_ie_triplets(b"USI", [(1, 11, 30)]))
    write(root, "fuzz_country_ie", "negative_dbm.bin", country_ie_triplets(b"JPI", [(1, 14, 0xFF)]))
    write(
        root,
        "fuzz_country_ie",
        "operating_triplet_skipped.bin",
        country_ie_triplets(b"USI", [(1, 11, 30), (201, 0, 0), (36, 8, 17)]),
    )
    write(
        root,
        "fuzz_country_ie",
        "caps_at_16_triplets.bin",
        country_ie_triplets(b"USI", [((1 + i) & 0xFF, 1, 20) for i in range(20)]),
    )
    write(
        root,
        "fuzz_country_ie",
        "trailing_partial_triplet.bin",
        country_ie_triplets(b"USI", [(1, 11, 30), (36, 8, 17)]) + bytes([0xAA, 0xBB]),
    )
    write(root, "fuzz_country_ie", "too_short.bin", b"US")

    # --- fuzz_eapol_key ---
    write(root, "fuzz_eapol_key", "well_formed.bin", eapol_key())
    wrong_type = bytearray(eapol_key())
    wrong_type[1] = 0x01  # EAP packet, not EAPOL-Key
    write(root, "fuzz_eapol_key", "wrong_type.bin", bytes(wrong_type))
    write(root, "fuzz_eapol_key", "truncated.bin", bytes(10))
    write(
        root,
        "fuzz_eapol_key",
        "oversized_keydata.bin",
        eapol_key({93: (0xFFFF).to_bytes(2, "big")}),
    )

    total = sum(
        len(os.listdir(os.path.join(root, "corpus", t)))
        for t in ("fuzz_frame_header", "fuzz_beacon_body", "fuzz_ie", "fuzz_country_ie", "fuzz_eapol_key")
    )
    print(f"seeded {os.path.join(root, 'corpus')}: {total} files across 5 targets")


if __name__ == "__main__":
    main()
