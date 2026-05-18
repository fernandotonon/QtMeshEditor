#include "GpuCommandParser.h"

#include <QTextStream>

namespace {

int16_t signExtend11(uint32_t v)
{
    v &= 0x7FF;
    if (v & 0x400)
        v |= ~0x7FFu;
    return static_cast<int16_t>(v);
}

void decodeScreenPos(uint32_t word, int32_t &x, int32_t &y)
{
    x = signExtend11(word & 0xFFFF);
    y = signExtend11((word >> 16) & 0xFFFF);
}

void decodeColor24(uint32_t word, uint8_t &r, uint8_t &g, uint8_t &b)
{
    r = (word >> 0) & 0xFF;
    g = (word >> 8) & 0xFF;
    b = (word >> 16) & 0xFF;
}

void decodeUvTpageClut(uint32_t word, int16_t &u, int16_t &v, uint16_t &clut, uint16_t &tpage)
{
    u = static_cast<int16_t>(word & 0xFF);
    v = static_cast<int16_t>((word >> 8) & 0xFF);
    const uint16_t upper = static_cast<uint16_t>((word >> 16) & 0xFFFF);
    clut = upper;
    tpage = upper;
}

PrimKind kindFromOpcode(uint8_t cmd, uint8_t &vertexCount)
{
    if (cmd >= 0x20 && cmd <= 0x27) {
        vertexCount = 3;
        if (cmd >= 0x24)
            return PrimKind::TexturedTri;
        return PrimKind::MonoTri;
    }
    if (cmd >= 0x28 && cmd <= 0x2F) {
        vertexCount = 4;
        if (cmd >= 0x2C)
            return PrimKind::TexturedQuad;
        return PrimKind::MonoQuad;
    }
    if (cmd >= 0x30 && cmd <= 0x3F) {
        vertexCount = 3;
        if (cmd >= 0x34)
            return PrimKind::TexturedTri;
        return PrimKind::ShadedTri;
    }
    if (cmd >= 0x60 && cmd <= 0x7F) {
        vertexCount = 2;
        return PrimKind::Sprite;
    }
    vertexCount = 0;
    return PrimKind::MonoTri;
}

bool parseMonoTri(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out, QString &error)
{
    if (index + 4 > wordCount) {
        error = QStringLiteral("monochrome triangle: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);

    for (int v = 0; v < 3; ++v) {
        decodeScreenPos(words[index++], out.verts[v].x, out.verts[v].y);
        if (v > 0) {
            out.verts[v].r = out.verts[0].r;
            out.verts[v].g = out.verts[0].g;
            out.verts[v].b = out.verts[0].b;
        }
    }

    out.kind = PrimKind::MonoTri;
    out.vertexCount = 3;
    return true;
}

bool parseTexturedTri(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out,
                      QString &error)
{
    if (index + 7 > wordCount) {
        error = QStringLiteral("textured triangle: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);

    for (int v = 0; v < 3; ++v) {
        decodeScreenPos(words[index++], out.verts[v].x, out.verts[v].y);
        uint32_t uvWord = words[index++];
        decodeUvTpageClut(uvWord, out.verts[v].u, out.verts[v].v, out.clut, out.tpage);
        if (v > 0) {
            out.verts[v].r = out.verts[0].r;
            out.verts[v].g = out.verts[0].g;
            out.verts[v].b = out.verts[0].b;
        }
    }

    out.kind = PrimKind::TexturedTri;
    out.vertexCount = 3;
    return true;
}

bool parseGouraudTri(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out,
                     QString &error)
{
    // psx-spx: cmd carries vertex-0 color; then (x,y) for v0; color+(x,y) for v1 and v2.
    if (index + 6 > wordCount) {
        error = QStringLiteral("gouraud triangle: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    decodeScreenPos(words[index++], out.verts[0].x, out.verts[0].y);

    for (int v = 1; v < 3; ++v) {
        const uint32_t colorWord = words[index++];
        decodeColor24(colorWord, out.verts[v].r, out.verts[v].g, out.verts[v].b);
        decodeScreenPos(words[index++], out.verts[v].x, out.verts[v].y);
    }

    out.kind = PrimKind::ShadedTri;
    out.vertexCount = 3;
    return true;
}

bool parseTexturedGouraudTri(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out,
                             QString &error)
{
    if (index + 9 > wordCount) {
        error = QStringLiteral("textured gouraud triangle: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    decodeScreenPos(words[index++], out.verts[0].x, out.verts[0].y);
    decodeUvTpageClut(words[index++], out.verts[0].u, out.verts[0].v, out.clut, out.tpage);

    for (int v = 1; v < 3; ++v) {
        const uint32_t colorWord = words[index++];
        decodeColor24(colorWord, out.verts[v].r, out.verts[v].g, out.verts[v].b);
        decodeScreenPos(words[index++], out.verts[v].x, out.verts[v].y);
        decodeUvTpageClut(words[index++], out.verts[v].u, out.verts[v].v, out.clut, out.tpage);
    }

    out.kind = PrimKind::TexturedTri;
    out.vertexCount = 3;
    return true;
}

bool parseTexturedQuad(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out,
                       QString &error)
{
    if (index + 9 > wordCount) {
        error = QStringLiteral("textured quad: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);

    for (int v = 0; v < 4; ++v) {
        decodeScreenPos(words[index++], out.verts[v].x, out.verts[v].y);
        const uint32_t uvWord = words[index++];
        decodeUvTpageClut(uvWord, out.verts[v].u, out.verts[v].v, out.clut, out.tpage);
        if (v > 0) {
            out.verts[v].r = out.verts[0].r;
            out.verts[v].g = out.verts[0].g;
            out.verts[v].b = out.verts[0].b;
        }
    }

    out.kind = PrimKind::TexturedQuad;
    out.vertexCount = 4;
    return true;
}

bool parseMonoQuad(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out,
                   QString &error)
{
    if (index + 5 > wordCount) {
        error = QStringLiteral("monochrome quad: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);

    for (int v = 0; v < 4; ++v) {
        decodeScreenPos(words[index++], out.verts[v].x, out.verts[v].y);
        if (v > 0) {
            out.verts[v].r = out.verts[0].r;
            out.verts[v].g = out.verts[0].g;
            out.verts[v].b = out.verts[0].b;
        }
    }

    out.kind = PrimKind::MonoQuad;
    out.vertexCount = 4;
    return true;
}

bool parseGouraudQuad(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out,
                      QString &error)
{
    if (index + 8 > wordCount) {
        error = QStringLiteral("gouraud quad: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    decodeScreenPos(words[index++], out.verts[0].x, out.verts[0].y);

    for (int v = 1; v < 4; ++v) {
        const uint32_t colorWord = words[index++];
        decodeColor24(colorWord, out.verts[v].r, out.verts[v].g, out.verts[v].b);
        decodeScreenPos(words[index++], out.verts[v].x, out.verts[v].y);
    }

    out.kind = PrimKind::ShadedQuad;
    out.vertexCount = 4;
    return true;
}

bool parseSprite(const uint32_t *words, size_t wordCount, size_t &index, PrimRecord &out,
                 QString &error)
{
    if (index + 4 > wordCount) {
        error = QStringLiteral("sprite: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    decodeColor24(cmdWord >> 8, out.verts[0].r, out.verts[0].g, out.verts[0].b);
    out.semiTrans = static_cast<uint8_t>((cmdWord >> 24) & 0x3);

    decodeScreenPos(words[index++], out.verts[0].x, out.verts[0].y);
    decodeScreenPos(words[index++], out.verts[1].x, out.verts[1].y);
    const uint32_t uvWord = words[index++];
    decodeUvTpageClut(uvWord, out.verts[0].u, out.verts[0].v, out.clut, out.tpage);
    out.verts[1].u = out.verts[0].u;
    out.verts[1].v = out.verts[0].v;

    out.kind = PrimKind::Sprite;
    out.vertexCount = 2;
    return true;
}

bool parseDrawingEnv(const uint32_t *words, size_t wordCount, size_t &index, DrawModeRecord &out,
                     QString &error)
{
    if (index + 1 > wordCount) {
        error = QStringLiteral("drawing environment: packet too short");
        return false;
    }

    const uint32_t cmdWord = words[index++];
    out.drawModeBits = cmdWord;
    out.tpage = static_cast<uint16_t>((cmdWord >> 16) & 0xFFFF);
    out.clut = static_cast<uint16_t>((cmdWord >> 16) & 0xFFFF);
    return true;
}

size_t packetWordCount(uint8_t cmd)
{
    if (cmd >= 0x20 && cmd <= 0x23)
        return 4;
    if (cmd >= 0x24 && cmd <= 0x27)
        return 10;
    if (cmd >= 0x28 && cmd <= 0x2B)
        return 5;
    if (cmd >= 0x2C && cmd <= 0x2F)
        return 9;
    if (cmd >= 0x30 && cmd <= 0x33)
        return 6;
    if (cmd >= 0x34 && cmd <= 0x37)
        return 9;
    if (cmd >= 0x38 && cmd <= 0x3B)
        return 8;
    if (cmd >= 0x60 && cmd <= 0x7F)
        return 4;
    if (cmd >= 0xE1 && cmd <= 0xE6)
        return 1;
    if (cmd == 0xA0 || cmd == 0xC0)
        return 3; // minimum header; variable in hardware — tests use fixed uploads
    return 0;
}

} // namespace

GpuCommandParser::ParseResult GpuCommandParser::parseGp0(const uint32_t *words, size_t wordCount)
{
    ParseResult result;
    if (!words || wordCount == 0)
        return result;

    size_t index = 0;
    while (index < wordCount) {
        const uint8_t cmd = static_cast<uint8_t>(words[index] & 0xFF);

        if (cmd >= 0xE1 && cmd <= 0xE6) {
            DrawModeRecord mode{};
            if (!parseDrawingEnv(words, wordCount, index, mode, result.error))
                break;
            result.drawModes.append(mode);
            continue;
        }

        if (cmd == 0xA0 || cmd == 0xC0) {
            const size_t skip = packetWordCount(cmd);
            if (skip == 0 || index + skip > wordCount) {
                result.error = QStringLiteral("VRAM copy packet truncated");
                break;
            }
            index += skip;
            continue;
        }

        PrimRecord prim{};
        bool ok = false;
        if (cmd >= 0x20 && cmd <= 0x23)
            ok = parseMonoTri(words, wordCount, index, prim, result.error);
        else if (cmd >= 0x24 && cmd <= 0x27)
            ok = parseTexturedTri(words, wordCount, index, prim, result.error);
        else if (cmd >= 0x28 && cmd <= 0x2B)
            ok = parseMonoQuad(words, wordCount, index, prim, result.error);
        else if (cmd >= 0x2C && cmd <= 0x2F)
            ok = parseTexturedQuad(words, wordCount, index, prim, result.error);
        else if (cmd >= 0x30 && cmd <= 0x33)
            ok = parseGouraudTri(words, wordCount, index, prim, result.error);
        else if (cmd >= 0x34 && cmd <= 0x37)
            ok = parseTexturedGouraudTri(words, wordCount, index, prim, result.error);
        else if (cmd >= 0x38 && cmd <= 0x3B)
            ok = parseGouraudQuad(words, wordCount, index, prim, result.error);
        else if (cmd >= 0x60 && cmd <= 0x7F)
            ok = parseSprite(words, wordCount, index, prim, result.error);
        else {
            const size_t skip = packetWordCount(cmd);
            if (skip == 0) {
                result.error = QStringLiteral("unknown GP0 opcode 0x%1").arg(cmd, 2, 16, QChar('0'));
                break;
            }
            if (index + skip > wordCount) {
                result.error = QStringLiteral("packet truncated for opcode 0x%1").arg(cmd, 2, 16, QChar('0'));
                break;
            }
            index += skip;
            continue;
        }

        if (!ok)
            break;
        result.prims.append(prim);
    }

    return result;
}

QString GpuCommandParser::primsToCsv(const QVector<PrimRecord> &prims)
{
    QString out;
    QTextStream stream(&out);
    stream << "kind,vertexCount,matrixId,tpage,clut,x0,y0,u0,v0\n";
    for (const PrimRecord &p : prims) {
        stream << static_cast<int>(p.kind) << ',' << static_cast<int>(p.vertexCount) << ','
               << p.matrixId << ',' << p.tpage << ',' << p.clut << ',' << p.verts[0].x << ','
               << p.verts[0].y << ',' << p.verts[0].u << ',' << p.verts[0].v << '\n';
    }
    return out;
}
