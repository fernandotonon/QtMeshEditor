#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/PsxTmdRamScanner.h"
#include "PS1/runtime/RipperHooks.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr uint32_t kTmdMagic = 0x00000041u;
constexpr size_t kTmdHeaderSize = 12u;
constexpr size_t kObjHeaderSize = 28u;

/** Write a little-endian u32 to `dst`. */
void writeU32le(uint8_t *dst, uint32_t value)
{
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

/** Write a little-endian i16 to `dst`. */
void writeI16le(uint8_t *dst, int16_t value)
{
    const uint16_t u = static_cast<uint16_t>(value);
    dst[0] = static_cast<uint8_t>(u & 0xFFu);
    dst[1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
}

/**
 * Build a minimal 1-object TMD blob at `ram + tmdStart`:
 *   - 3 vertices (forming one triangle)
 *   - 1 normal
 *   - 1 primitive packet, type 0x20 (mono-color flat-shaded tri) ilen=3
 * Returns the total span (bytes) the blob occupies starting at `tmdStart`.
 */
size_t writeSimpleTmd(uint8_t *ram, size_t tmdStart)
{
    // File header.
    writeU32le(ram + tmdStart + 0, kTmdMagic);
    writeU32le(ram + tmdStart + 4, 0u); // flags: offsets relative to byte 12
    writeU32le(ram + tmdStart + 8, 1u); // numObj

    // Object header (28 bytes).
    constexpr size_t kVertOffRel = kObjHeaderSize;                       // immediately after obj header
    constexpr size_t kNormOffRel = kVertOffRel + 3u * 8u;                // after 3 verts (×8 bytes)
    constexpr size_t kPrimOffRel = kNormOffRel + 1u * 8u;                // after 1 normal

    const size_t objHdr = tmdStart + kTmdHeaderSize;
    writeU32le(ram + objHdr + 0, static_cast<uint32_t>(kVertOffRel));
    writeU32le(ram + objHdr + 4, 3u); // nVert
    writeU32le(ram + objHdr + 8, static_cast<uint32_t>(kNormOffRel));
    writeU32le(ram + objHdr + 12, 1u); // nNorm
    writeU32le(ram + objHdr + 16, static_cast<uint32_t>(kPrimOffRel));
    writeU32le(ram + objHdr + 20, 1u); // nPrim
    writeU32le(ram + objHdr + 24, 0u); // scale (unused)

    // Vertex pool: 3 verts × 8 bytes (i16 x,y,z + pad).
    const size_t vertBase = tmdStart + kTmdHeaderSize + kVertOffRel;
    writeI16le(ram + vertBase + 0 * 8 + 0, 100);
    writeI16le(ram + vertBase + 0 * 8 + 2, 0);
    writeI16le(ram + vertBase + 0 * 8 + 4, 0);
    writeI16le(ram + vertBase + 1 * 8 + 0, -100);
    writeI16le(ram + vertBase + 1 * 8 + 2, 100);
    writeI16le(ram + vertBase + 1 * 8 + 4, 0);
    writeI16le(ram + vertBase + 2 * 8 + 0, 0);
    writeI16le(ram + vertBase + 2 * 8 + 2, 0);
    writeI16le(ram + vertBase + 2 * 8 + 4, 100);

    // Normal pool: 1 normal (i16 0, 0, 4096 = unit +Z in 12.4 fixed).
    const size_t normBase = tmdStart + kTmdHeaderSize + kNormOffRel;
    writeI16le(ram + normBase + 0, 0);
    writeI16le(ram + normBase + 2, 0);
    writeI16le(ram + normBase + 4, 4096);

    // Primitive packet: mode=0x20, flag=0, ilen=3, olen=3 + RGB(80,80,80) + n_idx + v0/v1/v2
    const size_t primBase = tmdStart + kTmdHeaderSize + kPrimOffRel;
    ram[primBase + 0] = 0x03;           // olen
    ram[primBase + 1] = 0x03;           // ilen (3 dwords = 12 bytes payload)
    ram[primBase + 2] = 0x00;           // flag
    ram[primBase + 3] = 0x20;           // mode
    const uint8_t *payloadStart = ram + primBase + 4;
    (void)payloadStart;
    // Payload: RGB (3 bytes) + pad (1) + n_idx (u16) + v0 (u16) + v1 (u16) + v2 (u16) = 12 bytes.
    ram[primBase + 4] = 0x80;           // R
    ram[primBase + 5] = 0x80;           // G
    ram[primBase + 6] = 0x80;           // B
    ram[primBase + 7] = 0x00;           // pad / mode tag
    writeU32le(ram + primBase + 8, 0u); // n_idx (u16) + v0 (u16) packed as u32 below
    // Re-write the last 8 bytes with the actual indices:
    ram[primBase + 8] = 0x00;           // n_idx lo
    ram[primBase + 9] = 0x00;           // n_idx hi (normal 0)
    ram[primBase + 10] = 0x00;          // v0 lo
    ram[primBase + 11] = 0x00;          // v0 hi (vertex 0)
    ram[primBase + 12] = 0x01;          // v1 lo
    ram[primBase + 13] = 0x00;          // v1 hi (vertex 1)
    ram[primBase + 14] = 0x02;          // v2 lo
    ram[primBase + 15] = 0x00;          // v2 hi (vertex 2)

    return kTmdHeaderSize + kObjHeaderSize + 3u * 8u + 1u * 8u + 4u + 12u; // 12+28+24+8+4+12 = 88
}

} // namespace

TEST(PsxTmdRamScannerTest, FindsSingleTmdAtOffset)
{
    std::vector<uint8_t> ram(2u * 1024u * 1024u, 0u);
    constexpr size_t kTmdStart = 0x10000;
    const size_t span = writeSimpleTmd(ram.data(), kTmdStart);
    ASSERT_GT(span, 0u);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int found = PsxTmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks);
    EXPECT_EQ(found, 1);
    ASSERT_EQ(buffer.modelMeshes().size(), 1);

    const CapturedModelMesh &cap = buffer.modelMeshes().front();
    EXPECT_EQ(cap.sourceAddress, kTmdStart);
    EXPECT_EQ(cap.format, QStringLiteral("tmd"));
    EXPECT_NE(cap.contentHash, 0ULL);
    EXPECT_FALSE(cap.mesh.isEmpty());
    EXPECT_GE(cap.mesh.vertexCount, 3);
    EXPECT_GE(cap.mesh.triangleCount, 1);

    // Position must be finite and on the order of the editor world unit scale (PSX 12.4
    // fixed × 10.0 / 4096 ≈ 0.244 for vertex magnitude 100).
    ASSERT_FALSE(cap.mesh.subMeshes.isEmpty());
    const ReconstructedVertex &v = cap.mesh.subMeshes.front().vertices.front();
    EXPECT_TRUE(std::isfinite(v.px));
    EXPECT_TRUE(std::isfinite(v.py));
    EXPECT_TRUE(std::isfinite(v.pz));
}

TEST(PsxTmdRamScannerTest, RejectsGarbageHeader)
{
    std::vector<uint8_t> ram(64u * 1024u, 0u);

    // Wrong magic — must not match.
    writeU32le(ram.data() + 0, 0xDEADBEEFu);
    writeU32le(ram.data() + 4, 0u);
    writeU32le(ram.data() + 8, 1u);

    // Correct magic but absurd object count.
    writeU32le(ram.data() + 0x1000, kTmdMagic);
    writeU32le(ram.data() + 0x1000 + 4, 0u);
    writeU32le(ram.data() + 0x1000 + 8, 999999u); // numObj way out of range

    // Correct magic but bad flags.
    writeU32le(ram.data() + 0x2000, kTmdMagic);
    writeU32le(ram.data() + 0x2000 + 4, 0xCAFEBABEu); // flags must be 0 or 1
    writeU32le(ram.data() + 0x2000 + 8, 1u);

    // Correct magic, flags=0, numObj=1, but object header OOB (vertOff points past end).
    writeU32le(ram.data() + 0x3000, kTmdMagic);
    writeU32le(ram.data() + 0x3000 + 4, 0u);
    writeU32le(ram.data() + 0x3000 + 8, 1u);
    const size_t objHdr = 0x3000 + kTmdHeaderSize;
    writeU32le(ram.data() + objHdr + 0, 0x7FFFFFFFu); // vertOff far past RAM end
    writeU32le(ram.data() + objHdr + 4, 3u);
    writeU32le(ram.data() + objHdr + 8, 0u);
    writeU32le(ram.data() + objHdr + 12, 0u);
    writeU32le(ram.data() + objHdr + 16, 0u);
    writeU32le(ram.data() + objHdr + 20, 1u);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int found = PsxTmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks);
    EXPECT_EQ(found, 0);
    EXPECT_EQ(buffer.modelMeshes().size(), 0);
}

TEST(PsxTmdRamScannerTest, DedupesIdenticalCopies)
{
    std::vector<uint8_t> ram(2u * 1024u * 1024u, 0u);
    constexpr size_t kTmdA = 0x10000;
    constexpr size_t kTmdB = 0x20000;
    const size_t spanA = writeSimpleTmd(ram.data(), kTmdA);
    const size_t spanB = writeSimpleTmd(ram.data(), kTmdB);
    EXPECT_EQ(spanA, spanB);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int found = PsxTmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks);
    // Scanner sees both magics and emits both via hooks->onModelMesh; the buffer dedupes
    // them by content hash so only one survives.
    EXPECT_EQ(buffer.modelMeshes().size(), 1);
    // `found` reflects only the unique meshes the hooks accepted.
    EXPECT_EQ(found, 1);
}

TEST(PsxTmdRamScannerTest, RejectsZeroVertexObject)
{
    std::vector<uint8_t> ram(64u * 1024u, 0u);
    constexpr size_t kTmdStart = 0x1000;
    writeU32le(ram.data() + kTmdStart + 0, kTmdMagic);
    writeU32le(ram.data() + kTmdStart + 4, 0u);
    writeU32le(ram.data() + kTmdStart + 8, 1u);
    const size_t objHdr = kTmdStart + kTmdHeaderSize;
    writeU32le(ram.data() + objHdr + 0, kObjHeaderSize); // vertOff
    writeU32le(ram.data() + objHdr + 4, 0u);             // nVert = 0 — invalid
    writeU32le(ram.data() + objHdr + 8, kObjHeaderSize);
    writeU32le(ram.data() + objHdr + 12, 0u);
    writeU32le(ram.data() + objHdr + 16, kObjHeaderSize);
    writeU32le(ram.data() + objHdr + 20, 1u);

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    EXPECT_EQ(PsxTmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks), 0);
    EXPECT_EQ(buffer.modelMeshes().size(), 0);
}

TEST(PsxTmdRamScannerTest, HandlesPureRandomEntropyWithoutFalsePositives)
{
    // 64 KiB of deterministic pseudo-random bytes. Random data has a 1-in-2^32 chance per
    // 4-byte word of matching kTmdMagic; in 64 KiB / 4 B = 16K candidates that's 4 expected
    // magic matches. None should pass the full validation chain (flags + counts + OOB).
    std::vector<uint8_t> ram(64u * 1024u);
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> dist(0u, 255u);
    for (size_t i = 0; i < ram.size(); ++i)
        ram[i] = static_cast<uint8_t>(dist(rng));

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    const int found = PsxTmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks);
    // Allow up to 1 false positive in extremely improbable cases — we expect 0 in
    // practice with this seed, but keep the bound loose so the seed isn't a hard
    // dependency of the test.
    EXPECT_LE(found, 1) << "Random entropy must not false-positive at scale";
}

TEST(PsxTmdRamScannerTest, EmitsCorrectMaterialNameForTexturedPrim)
{
    // 1-object TMD with a single textured-tri (mode=0x24, flag=0, ilen=5). Vertices share
    // the simple non-textured layout above but with a 0x24 prim packet so we hit the
    // textureSubMesh material-naming path.
    std::vector<uint8_t> ram(64u * 1024u, 0u);
    constexpr size_t kTmdStart = 0x1000;
    writeU32le(ram.data() + kTmdStart + 0, kTmdMagic);
    writeU32le(ram.data() + kTmdStart + 4, 0u); // flags=0
    writeU32le(ram.data() + kTmdStart + 8, 1u); // numObj=1

    constexpr size_t kVertOffRel = kObjHeaderSize;
    constexpr size_t kNormOffRel = kVertOffRel + 3u * 8u;
    constexpr size_t kPrimOffRel = kNormOffRel + 1u * 8u;
    const size_t objHdr = kTmdStart + kTmdHeaderSize;
    writeU32le(ram.data() + objHdr + 0, static_cast<uint32_t>(kVertOffRel));
    writeU32le(ram.data() + objHdr + 4, 3u);
    writeU32le(ram.data() + objHdr + 8, static_cast<uint32_t>(kNormOffRel));
    writeU32le(ram.data() + objHdr + 12, 1u);
    writeU32le(ram.data() + objHdr + 16, static_cast<uint32_t>(kPrimOffRel));
    writeU32le(ram.data() + objHdr + 20, 1u);
    writeU32le(ram.data() + objHdr + 24, 0u);

    const size_t vertBase = kTmdStart + kTmdHeaderSize + kVertOffRel;
    writeI16le(ram.data() + vertBase + 0 * 8 + 0, 100);
    writeI16le(ram.data() + vertBase + 1 * 8 + 0, -100);
    writeI16le(ram.data() + vertBase + 1 * 8 + 2, 100);
    writeI16le(ram.data() + vertBase + 2 * 8 + 4, 100);

    const size_t normBase = kTmdStart + kTmdHeaderSize + kNormOffRel;
    writeI16le(ram.data() + normBase + 4, 4096); // n=(0,0,1)

    // Mode 0x24 flag=0 ilen=5 packet — payload 20 bytes:
    //  +0: U0   +1: V0   +2..3: CLUT (u16)
    //  +4: U1   +5: V1   +6..7: TPAGE (u16)
    //  +8: U2   +9: V2   +10..11: pad
    //  +12: n_idx (u16)  +14..15: v0 (u16)
    //  +16: v1 (u16)     +18: v2 (u16)
    const size_t primBase = kTmdStart + kTmdHeaderSize + kPrimOffRel;
    ram[primBase + 0] = 5; // olen
    ram[primBase + 1] = 5; // ilen
    ram[primBase + 2] = 0; // flag
    ram[primBase + 3] = 0x24; // mode
    ram[primBase + 4 + 0] = 0x10;
    ram[primBase + 4 + 1] = 0x10;
    ram[primBase + 4 + 2] = 0xCD; // CLUT lo
    ram[primBase + 4 + 3] = 0xAB; // CLUT hi → CLUT=0xABCD
    ram[primBase + 4 + 4] = 0x20;
    ram[primBase + 4 + 5] = 0x20;
    ram[primBase + 4 + 6] = 0x34; // TPAGE lo
    ram[primBase + 4 + 7] = 0x12; // TPAGE hi → TPAGE=0x1234
    ram[primBase + 4 + 8] = 0x30;
    ram[primBase + 4 + 9] = 0x30;
    // 12..13 = n_idx (0); 14..15 = v0 (0); 16..17 = v1 (1); 18..19 = v2 (2)
    ram[primBase + 4 + 16] = 0x01;
    ram[primBase + 4 + 18] = 0x02;

    std::atomic<bool> armed{true};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    EXPECT_EQ(PsxTmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks), 1);
    ASSERT_EQ(buffer.modelMeshes().size(), 1);

    const CapturedModelMesh &cap = buffer.modelMeshes().front();
    ASSERT_FALSE(cap.mesh.subMeshes.isEmpty());

    // Find the textured sub-mesh and verify the material name matches the standard
    // `PS1Rip_tpage_XXXX_clut_YYYY_stN_dmN` template used by MeshReconstructor.
    bool foundTextured = false;
    for (const ReconstructedSubMesh &sub : cap.mesh.subMeshes) {
        if (sub.materialName.startsWith(QStringLiteral("PS1Rip_tpage_"))) {
            EXPECT_TRUE(sub.materialName.contains(QStringLiteral("1234"))); // TPAGE
            EXPECT_TRUE(sub.materialName.contains(QStringLiteral("abcd"))); // CLUT
            foundTextured = true;
        }
    }
    EXPECT_TRUE(foundTextured) << "Textured prim must produce a PS1Rip_tpage_* sub-mesh";
}

// #674 review: computeTmdSpan must cover the full primitive payload, not a fixed 8 bytes
// per prim. Two TMDs that differ only DEEP inside the primitive payload (past the old
// 8-byte-per-prim window) must produce DIFFERENT contentHash values so they are NOT
// dedupe-merged into one.
TEST(PsxTmdRamScannerTest, ContentHashCoversFullPrimitivePayload)
{
    // Build a TMD with one 0x28 flag=0 ilen=4 prim (mono quad — 16-byte payload).
    // The prim parser at this mode reads v3 index from d[12..13]. Old computeTmdSpan
    // hashed only the first 8 bytes of the prim packet (header + 4 payload bytes),
    // so changes to v3 — at offset 12..13 within payload — would NOT change the hash.
    auto buildMonoQuadTmd = [](uint8_t *ram, size_t tmdStart, uint16_t v3Idx) {
        writeU32le(ram + tmdStart + 0, kTmdMagic);
        writeU32le(ram + tmdStart + 4, 0u);
        writeU32le(ram + tmdStart + 8, 1u);

        constexpr size_t kVertOffRel = kObjHeaderSize;
        constexpr size_t kNormOffRel = kVertOffRel + 4u * 8u;
        constexpr size_t kPrimOffRel = kNormOffRel + 1u * 8u;
        const size_t objHdr = tmdStart + kTmdHeaderSize;
        writeU32le(ram + objHdr + 0, static_cast<uint32_t>(kVertOffRel));
        writeU32le(ram + objHdr + 4, 4u);
        writeU32le(ram + objHdr + 8, static_cast<uint32_t>(kNormOffRel));
        writeU32le(ram + objHdr + 12, 1u);
        writeU32le(ram + objHdr + 16, static_cast<uint32_t>(kPrimOffRel));
        writeU32le(ram + objHdr + 20, 1u);

        const size_t vertBase = tmdStart + kTmdHeaderSize + kVertOffRel;
        for (int i = 0; i < 4; ++i)
            writeI16le(ram + vertBase + i * 8 + 0, static_cast<int16_t>(100 + i * 10));

        const size_t normBase = tmdStart + kTmdHeaderSize + kNormOffRel;
        writeI16le(ram + normBase + 4, 4096);

        // 0x28 flag=0 ilen=4 packet (mono quad, 16-byte payload). Per parser at
        // line ~317: d[0..2] = color, d[6..7] = v0, d[8..9] = v1, d[10..11] = v2,
        // d[12..13] = v3. We vary v3 between the two builds to exercise the "deep
        // payload" coverage of computeTmdSpan/contentHash.
        const size_t primBase = tmdStart + kTmdHeaderSize + kPrimOffRel;
        ram[primBase + 0] = 4;     // olen
        ram[primBase + 1] = 4;     // ilen
        ram[primBase + 2] = 0;     // flag
        ram[primBase + 3] = 0x28;  // mode
        ram[primBase + 4 + 0] = 0x80;
        ram[primBase + 4 + 1] = 0x80;
        ram[primBase + 4 + 2] = 0x80;
        ram[primBase + 4 + 6] = 0;    // v0
        ram[primBase + 4 + 8] = 1;    // v1
        ram[primBase + 4 + 10] = 2;   // v2
        ram[primBase + 4 + 12] = static_cast<uint8_t>(v3Idx & 0xFFu);
        ram[primBase + 4 + 13] = static_cast<uint8_t>((v3Idx >> 8) & 0xFFu);
    };

    auto captureHash = [&](uint16_t v3) -> uint64_t {
        std::vector<uint8_t> ram(64u * 1024u, 0u);
        buildMonoQuadTmd(ram.data(), 0x1000, v3);
        std::atomic<bool> armed{true};
        CaptureBuffer buffer;
        RipperHooks hooks;
        hooks.setArmedFlag(&armed);
        hooks.setBuffer(&buffer);
        EXPECT_EQ(PsxTmdRamScanner::captureFromSystemRam(ram.data(), ram.size(), &hooks), 1);
        if (buffer.modelMeshes().isEmpty())
            return 0ULL;
        return buffer.modelMeshes().front().contentHash;
    };

    const uint64_t hashV3_3 = captureHash(3);
    const uint64_t hashV3_2 = captureHash(2);
    ASSERT_NE(hashV3_3, 0ULL);
    ASSERT_NE(hashV3_2, 0ULL);
    EXPECT_NE(hashV3_3, hashV3_2)
        << "contentHash must cover the full primitive payload — a byte change at "
           "offset 12 inside a 16-byte payload must change the hash";
}

#endif // ENABLE_PS1_RIP
