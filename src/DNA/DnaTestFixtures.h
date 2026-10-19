#ifndef DNA_DNATESTFIXTURES_H
#define DNA_DNATESTFIXTURES_H

// Test-only: a tiny but complete MetaHuman-style DNA written with the library's
// own BinaryStreamWriter, so the importer / scene tests need no Epic asset.
// (Epic's characters are Fab Standard License and cannot be redistributed.)

#ifdef ENABLE_OPENRIGLOGIC

#include <dna/BinaryStreamWriter.h>
#include <dna/Writer.h>
#include <trio/streams/MemoryStream.h>

#include <cstdint>
#include <vector>

namespace DnaTest {

// A tiny but complete character: 3-joint spine (cm, world space, Y up), one
// quad mesh skinned to the joints, and one blend-shape channel.
inline std::vector<uint8_t> makeSyntheticDna(bool withGeometry = true, uint16_t lodCount = 1)
{
    auto stream = pma::makeScoped<dna::MemoryStream>();
    auto writer = pma::makeScoped<dna::BinaryStreamWriter>(stream.get());

    writer->setName("synthetic");
    writer->setArchetype(dna::Archetype::other);
    writer->setGender(dna::Gender::other);
    writer->setAge(30);
    writer->setTranslationUnit(dna::TranslationUnit::cm);
    writer->setRotationUnit(dna::RotationUnit::degrees);
    writer->setCoordinateSystem({tdm::axis_dir::right, tdm::axis_dir::up, tdm::axis_dir::back});
    writer->setRotationSequence(dna::RotationSequence::xyz);
    writer->setLODCount(lodCount);
    writer->setDBMaxLOD(0);
    writer->setDBComplexity("test");
    writer->setDBName("test");

    const char* names[] = {"root", "spine", "head"};
    for (uint16_t i = 0; i < 3; ++i) writer->setJointName(i, names[i]);
    const uint16_t parents[] = {0, 0, 1};  // root is its own parent
    writer->setJointHierarchy(parents, 3);
    const dna::Vector3 t[] = {{0, 0, 0}, {0, 100, 0}, {0, 150, 0}};
    const dna::Vector3 r[] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    writer->setNeutralJointTranslations(t, 3);
    writer->setNeutralJointRotations(r, 3);
    const uint16_t allJoints[] = {0, 1, 2};
    writer->setJointIndices(0, allJoints, 3);
    for (uint16_t l = 0; l < lodCount; ++l) writer->setLODJointMapping(l, 0);

    writer->setBlendShapeChannelName(0, "smile");
    const uint16_t bsIdx[] = {0};
    writer->setBlendShapeChannelIndices(0, bsIdx, 1);
    for (uint16_t l = 0; l < lodCount; ++l) writer->setLODBlendShapeChannelMapping(l, 0);

    writer->setMeshName(0, "body_lod0_mesh");
    const uint16_t meshIdx[] = {0};
    writer->setMeshIndices(0, meshIdx, 1);
    for (uint16_t l = 0; l < lodCount; ++l) writer->setLODMeshMapping(l, 0);
    writer->setMeshBlendShapeChannelMapping(0, 0, 0);

    if (withGeometry) {
        const dna::Position pos[] = {{-10, 0, 0}, {10, 0, 0}, {10, 150, 0}, {-10, 150, 0}};
        writer->setVertexPositions(0, pos, 4);
        const dna::TextureCoordinate uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        writer->setVertexTextureCoordinates(0, uv, 4);
        const dna::Normal nrm[] = {{0, 0, 1}};
        writer->setVertexNormals(0, nrm, 1);
        const dna::VertexLayout layouts[] = {{0, 0, 0}, {1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
        writer->setVertexLayouts(0, layouts, 4);
        const uint32_t face[] = {0, 1, 2, 3};
        writer->setFaceVertexLayoutIndices(0, 0, face, 4);
        writer->setMaximumInfluencePerVertex(0, 2);
        const float w0[] = {1.0f};
        const uint16_t j0[] = {0};
        const float w1[] = {0.5f, 0.5f};
        const uint16_t j1[] = {1, 2};
        writer->setSkinWeightsValues(0, 0, w0, 1);
        writer->setSkinWeightsJointIndices(0, 0, j0, 1);
        writer->setSkinWeightsValues(0, 1, w0, 1);
        writer->setSkinWeightsJointIndices(0, 1, j0, 1);
        writer->setSkinWeightsValues(0, 2, w1, 2);
        writer->setSkinWeightsJointIndices(0, 2, j1, 2);
        writer->setSkinWeightsValues(0, 3, w1, 2);
        writer->setSkinWeightsJointIndices(0, 3, j1, 2);
        writer->setBlendShapeChannelIndex(0, 0, 0);
        const dna::Delta d[] = {{0, 10, 0}, {0, 10, 0}};
        writer->setBlendShapeTargetDeltas(0, 0, d, 2);
        const uint32_t vi[] = {2, 3};
        writer->setBlendShapeTargetVertexIndices(0, 0, vi, 2);
    }
    writer->write();

    const uint64_t n = stream->size();
    std::vector<uint8_t> bytes(n);
    stream->seek(0);
    stream->read(reinterpret_cast<char*>(bytes.data()), n);
    return bytes;
}

}  // namespace DnaTest

#endif  // ENABLE_OPENRIGLOGIC
#endif  // DNA_DNATESTFIXTURES_H
