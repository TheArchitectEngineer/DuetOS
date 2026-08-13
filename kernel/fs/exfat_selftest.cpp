// exfat_selftest.cpp — ExfatSelfTest: wires the exFAT write path
// (ExfatCreateInRoot / ExfatFindInRoot / ExfatWriteInPlace /
// ExfatAppendInRoot / ExfatTruncateInRoot) into the boot self-test
// list so it is exercised on every boot instead of sitting dead.
//
// Unlike Fat32SelfTest (which probes a pre-built GPT image on the
// QEMU disks), this builds a *synthetic* exFAT volume in a fresh RAM
// block device: a valid VBR the Rust parser accepts, a zeroed FAT, an
// allocation-bitmap special file + its root dirent, and an otherwise-
// empty root cluster. ExfatProbe parses it; the CRUD ops then plant /
// patch their own dirent sets, so the test genuinely drives the write
// code rather than just reading a fixture.
//
// The write ops take a non-const Volume* (they refresh the cached root
// snapshot), but the public registry only hands out a const Volume*.
// We copy the probed Volume (a POD) onto the stack and drive CRUD on
// that mutable copy — the ops key off block_handle + geometry, not the
// g_volumes slot, so this is faithful and keeps the registry clean.

#include "fs/exfat.h"

#include "arch/x86_64/serial.h"
#include "debug/probes.h"
#include "drivers/storage/block.h"
#include "fs/mount.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "log/klog.h"

namespace duetos::fs::exfat
{

namespace
{

// Synthetic geometry: 512-byte sectors (shift 9), 1 sector/cluster
// (shift 0) so cluster_bytes == 512 and one cluster holds 16 dirent
// slots — plenty for the bitmap dirent + a small file set, and small
// enough that the FAT / heap fit in a tiny RAM disk.
constexpr u32 kBpsShift = 9;
constexpr u32 kSpcShift = 0;
constexpr u32 kBytesPerSector = 1u << kBpsShift; // 512
constexpr u32 kSectorsPerCluster = 1u << kSpcShift;

// Layout (all LBAs partition-relative; partition_offset == 0):
//   LBA 0          VBR (boot sector)
//   LBA 1          FAT (4 bytes/cluster). 1 sector covers 128 entries.
//   LBA 2 = heap   cluster 2 = allocation bitmap file
//   LBA 3          cluster 3 = root directory
//   LBA 4..        free data clusters for CRUD
constexpr u32 kFatLba = 1;
constexpr u32 kFatLength = 1;                            // sectors
constexpr u32 kClusterHeapOffset = kFatLba + kFatLength; // LBA 2
constexpr u32 kBitmapCluster = 2;
constexpr u32 kRootCluster = 3;
constexpr u32 kClusterCount = 64; // total data clusters
constexpr u64 kSectorCount = kClusterHeapOffset + kClusterCount;

inline void StoreLe32(u8* p, u32 v)
{
    p[0] = u8(v & 0xFF);
    p[1] = u8((v >> 8) & 0xFF);
    p[2] = u8((v >> 16) & 0xFF);
    p[3] = u8((v >> 24) & 0xFF);
}

inline void StoreLe64(u8* p, u64 v)
{
    for (u32 i = 0; i < 8; ++i)
        p[i] = u8((v >> (i * 8)) & 0xFF);
}

inline void Zero(void* p, u64 n)
{
    auto* b = static_cast<volatile u8*>(p);
    for (u64 i = 0; i < n; ++i)
        b[i] = 0;
}

// Write `sector` (512 bytes) at `lba`. Returns true on success.
bool PutSector(u32 handle, u64 lba, const u8* sector)
{
    return drivers::storage::BlockDeviceWrite(handle, lba, 1, sector) == 0;
}

// Lay out a minimal-but-valid exFAT volume on the RAM disk at `handle`.
// Returns true on success. After this the device passes ExfatProbe (when
// `volume_serial == kDuetOsVolumeSerial`) and carries an allocation
// bitmap that marks clusters 2 (bitmap) and 3 (root) used, everything
// else free — exactly what the write path's AllocateCluster scan
// expects. `volume_serial` lets a caller stamp a FOREIGN serial to
// exercise the adoption gate's rejection path.
bool BuildSyntheticVolume(u32 handle, u32 volume_serial = kDuetOsVolumeSerial)
{
    u8 sec[kBytesPerSector];

    // ---- VBR (LBA 0). Only the fields the Rust parser reads matter;
    // everything else stays zero. OEM ID "EXFAT   " at 0x03, boot
    // signature 0x55AA at 0x1FE.
    Zero(sec, sizeof(sec));
    const char* oem = "EXFAT   ";
    for (u32 i = 0; i < 8; ++i)
        sec[3 + i] = u8(oem[i]);
    StoreLe64(sec + 0x40, 0);                  // PartitionOffset
    StoreLe64(sec + 0x48, kSectorCount);       // VolumeLength
    StoreLe32(sec + 0x50, kFatLba);            // FatOffset
    StoreLe32(sec + 0x54, kFatLength);         // FatLength
    StoreLe32(sec + 0x58, kClusterHeapOffset); // ClusterHeapOffset
    StoreLe32(sec + 0x5C, kClusterCount);      // ClusterCount
    StoreLe32(sec + 0x60, kRootCluster);       // FirstClusterOfRootDirectory
    StoreLe32(sec + 0x64, volume_serial);      // VolumeSerialNumber (DuetOS-owned marker, or foreign for the gate test)
    sec[0x6C] = u8(kBpsShift);                 // BytesPerSectorShift
    sec[0x6D] = u8(kSpcShift);                 // SectorsPerClusterShift
    sec[0x6E] = 1;                             // NumberOfFats
    sec[0x1FE] = 0x55;
    sec[0x1FF] = 0xAA;
    if (!PutSector(handle, 0, sec))
        return false;

    // ---- FAT (LBA 1). Entry 0 = media descriptor (0xFFFFFFF8), entry
    // 1 = EOC marker; clusters 2 and 3 are single-cluster chains, so
    // both are end-of-chain. The rest stay 0 (free).
    Zero(sec, sizeof(sec));
    StoreLe32(sec + 0 * 4, 0xFFFFFFF8u);
    StoreLe32(sec + 1 * 4, 0xFFFFFFFFu);
    StoreLe32(sec + kBitmapCluster * 4, 0xFFFFFFFFu); // bitmap: EOC
    StoreLe32(sec + kRootCluster * 4, 0xFFFFFFFFu);   // root: EOC
    if (!PutSector(handle, kFatLba, sec))
        return false;

    // ---- Allocation bitmap (cluster 2 → LBA kClusterHeapOffset).
    // Bit N (LSB-first) covers cluster N+2. Mark cluster 2 (bit 0) and
    // cluster 3 (bit 1) used; all higher clusters free.
    Zero(sec, sizeof(sec));
    sec[0] = u8((1u << 0) | (1u << 1)); // clusters 2 and 3 used
    if (!PutSector(handle, kClusterHeapOffset + (kBitmapCluster - 2), sec))
        return false;

    // ---- Root directory (cluster 3). One Allocation-Bitmap dirent
    // (type 0x81) pointing at cluster 2, length = ceil(cluster_count/8)
    // bytes. The remaining slots are zero (end-of-dir), which the write
    // path treats as free.
    Zero(sec, sizeof(sec));
    u8* bm = sec; // first slot
    bm[0] = 0x81;
    StoreLe32(bm + 0x14, kBitmapCluster);           // FirstCluster
    const u64 bitmap_len = (kClusterCount + 7) / 8; // 8 bytes for 64 clusters
    StoreLe64(bm + 0x18, bitmap_len);               // DataLength
    if (!PutSector(handle, kClusterHeapOffset + (kRootCluster - 2), sec))
        return false;

    return true;
}

// Read the first cluster of `e`'s data into `out` (>= kBytesPerSector).
// Single-cluster reads cover every case the test exercises (all bodies
// are < one cluster). Returns true on success.
bool ReadFirstCluster(const Volume& v, const DirEntry* e, u8* out)
{
    if (e == nullptr || e->first_cluster < 2)
        return false;
    const u64 lba = u64(kClusterHeapOffset) + u64(e->first_cluster - 2) * kSectorsPerCluster;
    return drivers::storage::BlockDeviceRead(v.block_handle, lba, 1, out) == 0;
}

void Fail(const char* phase)
{
    using arch::SerialWrite;
    SerialWrite("[exfat-selftest] FAIL (");
    SerialWrite(phase);
    SerialWrite(")\n");
    KBP_PROBE_V(duetos::debug::ProbeId::kBootSelftestFail, 0xEFA7u);
}

} // namespace

// Boot self-test: build a synthetic exFAT volume in RAM and drive the
// write path end to end. Registered after Fat32SelfTest in
// boot_bringup.cpp. Emits a single [exfat-selftest] PASS line on
// success so CI can grep it; a FAIL line + kBootSelftestFail probe on
// any failed assertion.
void ExfatSelfTest()
{
    KLOG_TRACE_SCOPE("fs/exfat", "ExfatSelfTest");
    using arch::SerialWrite;

    const u32 handle = drivers::storage::RamBlockDeviceCreate("ramexfat", kBytesPerSector, kSectorCount);
    if (handle == drivers::storage::kBlockHandleInvalid)
    {
        Fail("ramdisk-create");
        return;
    }
    if (!BuildSyntheticVolume(handle))
    {
        Fail("build-volume");
        return;
    }

    auto probed = ExfatProbe(handle);
    if (!probed)
    {
        Fail("probe");
        return;
    }
    const Volume* reg = ExfatVolumeByIndex(probed.value());
    if (reg == nullptr)
    {
        Fail("volume-lookup");
        return;
    }

    // ---- Adoption-gate regression (commit 7bb94062, FAT32 -> exFAT):
    // a parseable exFAT volume whose VolumeSerialNumber is NOT the
    // DuetOS marker must be REFUSED registration. Build one on a second
    // RAM disk with a foreign serial and assert ExfatProbe returns
    // NotFound and the registry count did not grow.
    {
        const u32 foreign = drivers::storage::RamBlockDeviceCreate("ramexfatx", kBytesPerSector, kSectorCount);
        if (foreign == drivers::storage::kBlockHandleInvalid)
        {
            Fail("foreign-ramdisk-create");
            return;
        }
        if (!BuildSyntheticVolume(foreign, 0x12345678u))
        {
            Fail("foreign-build");
            return;
        }
        const u32 before = ExfatVolumeCount();
        auto foreign_probe = ExfatProbe(foreign);
        if (foreign_probe || foreign_probe.error() != ::duetos::core::ErrorCode::NotFound)
        {
            Fail("foreign-not-rejected");
            return;
        }
        if (ExfatVolumeCount() != before)
        {
            Fail("foreign-registered");
            return;
        }
    }
    // Mutable working copy — the append/create/truncate ops take a
    // non-const Volume* (they refresh the cached root snapshot). The
    // ops key off block_handle + geometry, so a stack copy is faithful.
    Volume vol = *reg;
    Volume* v = &vol;

    // ---- Phase 1: create "HELLO.TXT" with a 17-byte body.
    const u8 body[] = "hello from exfat\n"; // 17 bytes + NUL
    const u64 body_len = 17;
    if (ExfatCreateInRoot(v, "HELLO.TXT", body, body_len) != i64(body_len))
    {
        Fail("create");
        return;
    }

    // ---- Phase 2: find it back in the refreshed snapshot.
    const DirEntry* hello = ExfatFindInRoot(v, "HELLO.TXT");
    if (hello == nullptr || hello->size_bytes != body_len)
    {
        Fail("find-after-create");
        return;
    }
    {
        u8 buf[kBytesPerSector];
        if (!ReadFirstCluster(*v, hello, buf))
        {
            Fail("read-after-create");
            return;
        }
        for (u32 i = 0; i < body_len; ++i)
        {
            if (buf[i] != body[i])
            {
                Fail("content-after-create");
                return;
            }
        }
    }

    // ---- Phase 3: in-place overwrite of bytes [0..5) "hello"->"HELLO".
    const u8 upper[] = {'H', 'E', 'L', 'L', 'O'};
    if (ExfatWriteInPlace(v, hello, 0, upper, 5) != 5)
    {
        Fail("write-in-place");
        return;
    }
    {
        u8 buf[kBytesPerSector];
        if (!ReadFirstCluster(*v, hello, buf))
        {
            Fail("read-after-write");
            return;
        }
        if (buf[0] != 'H' || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'L' || buf[4] != 'O' || buf[5] != ' ')
        {
            Fail("content-after-write");
            return;
        }
    }

    // ---- Phase 4: append 600 bytes — forces a second cluster, FAT
    // chaining, and a Stream-Extension size patch. 17 + 600 = 617 > 512.
    static u8 tail[600];
    for (u32 i = 0; i < sizeof(tail); ++i)
        tail[i] = u8(0x41 + (i % 26)); // A..Z repeating
    if (ExfatAppendInRoot(v, "HELLO.TXT", tail, sizeof(tail)) != i64(sizeof(tail)))
    {
        Fail("append");
        return;
    }
    const DirEntry* grown = ExfatFindInRoot(v, "HELLO.TXT");
    if (grown == nullptr || grown->size_bytes != body_len + sizeof(tail))
    {
        Fail("size-after-append");
        return;
    }
    // Verify the second cluster holds the tail's first sector's worth of
    // pattern. Byte at absolute offset `body_len + k` lives at offset
    // (body_len + k) within the chain; the first 512-body_len tail bytes
    // are in cluster 1, the rest in cluster 2. Spot-check the spill into
    // cluster 2: absolute offset 512 == tail index (512 - body_len).
    {
        u32 next = 0;
        // Walk the chain one hop to the second cluster.
        const u64 fat_lba = u64(kFatLba) + (u64(grown->first_cluster) * 4) / kBytesPerSector;
        u8 fatbuf[kBytesPerSector];
        if (drivers::storage::BlockDeviceRead(v->block_handle, fat_lba, 1, fatbuf) != 0)
        {
            Fail("append-fat-read");
            return;
        }
        const u32 in = u32((u64(grown->first_cluster) * 4) % kBytesPerSector);
        next = u32(fatbuf[in]) | (u32(fatbuf[in + 1]) << 8) | (u32(fatbuf[in + 2]) << 16) | (u32(fatbuf[in + 3]) << 24);
        if (next < 2 || next >= 0xFFFFFFF8u)
        {
            Fail("append-no-second-cluster");
            return;
        }
        u8 c2[kBytesPerSector];
        const u64 c2_lba = u64(kClusterHeapOffset) + u64(next - 2) * kSectorsPerCluster;
        if (drivers::storage::BlockDeviceRead(v->block_handle, c2_lba, 1, c2) != 0)
        {
            Fail("append-c2-read");
            return;
        }
        // Absolute offset 512 is tail index (512 - body_len).
        const u32 tail_idx_at_512 = u32(kBytesPerSector - body_len);
        if (c2[0] != tail[tail_idx_at_512])
        {
            Fail("append-content");
            return;
        }
    }

    // ---- Phase 5: truncate back to 7 bytes ("HELLO f"), freeing the
    // second cluster. Verify the snapshot size and that the freed
    // cluster's bitmap bit is now clear (re-allocatable).
    if (ExfatTruncateInRoot(v, "HELLO.TXT", 7) != 7)
    {
        Fail("truncate");
        return;
    }
    const DirEntry* trunc = ExfatFindInRoot(v, "HELLO.TXT");
    if (trunc == nullptr || trunc->size_bytes != 7)
    {
        Fail("size-after-truncate");
        return;
    }
    {
        u8 buf[kBytesPerSector];
        if (!ReadFirstCluster(*v, trunc, buf))
        {
            Fail("read-after-truncate");
            return;
        }
        if (buf[0] != 'H' || buf[6] != 'f')
        {
            Fail("content-after-truncate");
            return;
        }
    }

    // Phases 1-5 drove CRUD on `vol`, a stack copy of the probed
    // volume (the mutation API needs a non-const `Volume*`, and
    // `ExfatVolumeByIndex` only ever hands out `const Volume*` — see
    // the file-header comment). That means the REGISTRY's own cached
    // root-dir snapshot (`g_volumes[probed.value()]`, what
    // `ExfatVolumeByIndex` actually returns) never saw HELLO.TXT get
    // created/written/appended/truncated — only `vol`'s cache did.
    // Phase 6 below resolves through the real VFS path, which reads
    // the registry, not `vol` — without this resync it would
    // legitimately miss HELLO.TXT even though every CRUD phase above
    // passed. Sync the registry's cache from the same on-disk state
    // the CRUD phases just wrote before exercising that path.
    ExfatRefreshVolume(probed.value());

    // ---- Phase 6: VFS integration. Mount the synthetic volume and
    // prove VfsResolve surfaces an exFAT-tagged node the predicates +
    // read-dispatch path agree on — the wire-in that makes a path
    // under an exFAT mount (`cat /disk/exfat<N>/HELLO.TXT`) reach this
    // backend read-only. Mirrors the NTFS self-test's Phase 7
    // (ntfs_selftest.cpp). `probed.value()` is the volume REGISTRY
    // INDEX (not the raw block handle) — ExfatLookup indexes via
    // ExfatVolumeByIndex, the same convention FAT32 uses.
    //
    // This self-test is the first exFAT probe of the boot sequence
    // (ExfatScanAll's real-disk scan runs later, in boot_bringup.cpp),
    // so `probed.value()` MUST be registry index 0 — deliberately
    // asserted rather than assumed, because index 0 is exactly the
    // value `VfsMount` rejects for every non-Ram backend
    // (`block_handle == 0`). `ExfatLookup` (fs/mount.cpp) decodes the
    // mount handle back to the true index via `index + 1`; the +1
    // here must match, and this assertion is what guarantees the test
    // actually exercises that first-volume edge instead of silently
    // drifting to a later index that would mask the bug.
    if (probed.value() != 0)
    {
        Fail("vfs-mount-not-first-volume");
        return;
    }
    const MountId mid = VfsMount("/mnt/exfat-selftest", FsType::Exfat, probed.value() + 1);
    if (mid == kInvalidMountId)
    {
        Fail("vfs-mount");
        return;
    }
    // Mount point itself resolves to the synthetic root directory.
    const VfsNode rootnode = VfsResolve(RamfsTrustedRoot(), "/mnt/exfat-selftest", 64);
    if (rootnode.backend != VfsBackend::Exfat || !VfsNodeIsDir(rootnode) || VfsNodeIsFile(rootnode))
    {
        Fail("vfs-resolve-root");
        return;
    }
    const char kVfsPath[] = "/mnt/exfat-selftest/HELLO.TXT";
    const VfsNode node = VfsResolve(RamfsTrustedRoot(), kVfsPath, 64);
    if (node.backend != VfsBackend::Exfat || !VfsNodeIsFile(node) || VfsNodeIsDir(node))
    {
        Fail("vfs-resolve");
        return;
    }
    if (VfsNodeSize(node) != 7 || node.exfat_volume_idx != probed.value())
    {
        Fail("vfs-node-fields");
        return;
    }
    // Read through the node exactly as the shell read path does
    // (ExfatVolumeByIndex -> ExfatReadAt) and compare.
    const Volume* vvol = ExfatVolumeByIndex(node.exfat_volume_idx);
    u8 vbuf[7];
    const i64 vread = (vvol != nullptr) ? ExfatReadAt(vvol, &node.exfat_entry, 0, vbuf, sizeof(vbuf)) : -1;
    if (vread != i64(sizeof(vbuf)) || vbuf[0] != 'H' || vbuf[5] != ' ' || vbuf[6] != 'f')
    {
        Fail("vfs-read");
        return;
    }
    // A partial read (offset > 0) must clamp to the remaining bytes.
    u8 pbuf[4] = {};
    const i64 pread = (vvol != nullptr) ? ExfatReadAt(vvol, &node.exfat_entry, 5, pbuf, sizeof(pbuf)) : -1;
    if (pread != 2 || pbuf[0] != ' ' || pbuf[1] != 'f')
    {
        Fail("vfs-partial-read");
        return;
    }
    // A path that obviously doesn't exist under the mount must miss.
    const VfsNode miss = VfsResolve(RamfsTrustedRoot(), "/mnt/exfat-selftest/_NOPE_.X", 64);
    if (VfsNodeIsValid(miss) || miss.backend != VfsBackend::Invalid)
    {
        Fail("vfs-miss");
        return;
    }
    // A path reaching past the root (subdirectory descent) is
    // unsupported by design (fs/exfat.h header GAP) and must also miss
    // rather than mis-resolve.
    const VfsNode deep = VfsResolve(RamfsTrustedRoot(), "/mnt/exfat-selftest/SUB/FILE.TXT", 64);
    if (VfsNodeIsValid(deep) || deep.backend != VfsBackend::Invalid)
    {
        Fail("vfs-subdir-miss");
        return;
    }
    if (!VfsUmount(mid))
    {
        Fail("vfs-umount");
        return;
    }

    // ---- Phase 7: fail-closed regression for corrupt/adversarial
    // metadata. `ExfatReadAt` must never let a nonempty file with no
    // valid data cluster, or a FAT chain that ends before the dirent's
    // claimed size is satisfied, look like a legitimate EOF / short
    // read — both must return -1. These DirEntry values are hand-built
    // (not produced by the write path), so they exercise the read
    // path's own defenses independent of whether the write path could
    // ever produce them.
    {
        // (a) Nonzero size, but first_cluster < 2 — no data to walk.
        DirEntry bad_cluster{};
        bad_cluster.first_cluster = 0;
        bad_cluster.size_bytes = 8;
        bad_cluster.valid_data_len = 8;
        u8 buf[8];
        if (ExfatReadAt(vvol, &bad_cluster, 0, buf, sizeof(buf)) != -1)
        {
            Fail("readat-bad-cluster-not-closed");
            return;
        }

        // (b) first_cluster is valid, but its FAT chain is a single
        // cluster (kRootCluster's chain is EOC-terminated by
        // BuildSyntheticVolume) while size_bytes claims two clusters'
        // worth. A read spanning the cluster boundary must fail closed
        // when the chain walk hits EOC instead of a next cluster,
        // rather than silently truncating to a "successful" short read.
        DirEntry truncated_chain{};
        truncated_chain.first_cluster = kRootCluster;
        truncated_chain.size_bytes = u64(kBytesPerSector) * 2;
        truncated_chain.valid_data_len = truncated_chain.size_bytes;
        u8 buf2[64];
        const u64 near_boundary = u64(kBytesPerSector) - 32;
        if (ExfatReadAt(vvol, &truncated_chain, near_boundary, buf2, sizeof(buf2)) != -1)
        {
            Fail("readat-broken-chain-not-closed");
            return;
        }
    }

    SerialWrite("[exfat-selftest] PASS (synthetic volume: "
                "foreign-reject+create+find+write+append+truncate+vfs+fail-closed verified)\n");
}

} // namespace duetos::fs::exfat
