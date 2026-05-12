#include "MemoryEstimator.h"
#include "Manager.h"

#include <Ogre.h>
#include <OgrePixelFormat.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>
#include <OgreTextureManager.h>
#include <OgreHardwareBufferManager.h>

#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

// ----- Primitive math helpers ------------------------------------------------

quint64 MemoryEstimator::meshBytes(unsigned int vertexCount, unsigned int vertexStride,
                                   unsigned int indexCount, unsigned int indexSize)
{
    return static_cast<quint64>(vertexCount) * vertexStride
         + static_cast<quint64>(indexCount) * indexSize;
}

quint64 MemoryEstimator::textureBytes(unsigned int width, unsigned int height,
                                      unsigned int bytesPerPixel, bool hasMips)
{
    quint64 base = static_cast<quint64>(width) * height * bytesPerPixel;
    if (hasMips) {
        // Full mip chain converges to base * 4/3 ≈ +33%.
        base = (base * 4) / 3;
    }
    return base;
}

QString MemoryEstimator::formatBytes(quint64 bytes)
{
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * 1024.0;
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;

    if (bytes >= static_cast<quint64>(GB))
        return QString::number(bytes / GB, 'f', 2) + " GB";
    if (bytes >= static_cast<quint64>(MB))
        return QString::number(bytes / MB, 'f', 2) + " MB";
    if (bytes >= static_cast<quint64>(KB))
        return QString::number(bytes / KB, 'f', 1) + " KB";
    return QString::number(bytes) + " B";
}

quint64 MemoryEstimator::parseBudget(const QString& spec)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*(\\d+(?:\\.\\d+)?)\\s*([KMG]?B?)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    auto m = re.match(spec);
    if (!m.hasMatch()) return 0;

    bool ok = false;
    double value = m.captured(1).toDouble(&ok);
    if (!ok || value < 0) return 0;

    QString unit = m.captured(2).toUpper();
    double multiplier = 1.0;
    if (unit.startsWith('K'))      multiplier = 1024.0;
    else if (unit.startsWith('M')) multiplier = 1024.0 * 1024.0;
    else if (unit.startsWith('G')) multiplier = 1024.0 * 1024.0 * 1024.0;

    return static_cast<quint64>(value * multiplier);
}

// ----- Ogre-backed estimators ------------------------------------------------

// LCOV_EXCL_START — exercised only when Ogre is initialised (skipped in unit tests).
MeshMemoryEstimate MemoryEstimator::estimateEntity(const Ogre::Entity* entity)
{
    MeshMemoryEstimate est;
    if (!entity) return est;

    const Ogre::MeshPtr& mesh = entity->getMesh();
    if (!mesh) return est;

    est.name = QString::fromStdString(mesh->getName());

    auto accumulateVertexData = [&](Ogre::VertexData* vd) {
        if (!vd) return;
        est.vertexCount += vd->vertexCount;
        // Sum the stride from every bound vertex buffer in the declaration.
        // Most meshes use a single source, but tangents/UV2 etc. may use extras.
        QSet<unsigned short> seen;
        for (const auto& elem : vd->vertexDeclaration->getElements()) {
            unsigned short source = elem.getSource();
            if (seen.contains(source)) continue;
            seen.insert(source);
            unsigned int stride = vd->vertexDeclaration->getVertexSize(source);
            est.vertexBytes += static_cast<quint64>(vd->vertexCount) * stride;
        }
    };

    accumulateVertexData(mesh->sharedVertexData);

    for (unsigned int i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* sub = mesh->getSubMesh(i);
        if (!sub) continue;

        if (!sub->useSharedVertices)
            accumulateVertexData(sub->vertexData);

        if (sub->indexData) {
            est.indexCount += sub->indexData->indexCount;
            unsigned int indexSize =
                (sub->indexData->indexBuffer
                 && sub->indexData->indexBuffer->getType()
                    == Ogre::HardwareIndexBuffer::IT_32BIT) ? 4 : 2;
            est.indexBytes += static_cast<quint64>(sub->indexData->indexCount) * indexSize;
        }
    }

    return est;
}

QList<TextureMemoryEstimate> MemoryEstimator::estimateAllTextures()
{
    QList<TextureMemoryEstimate> out;

    auto& mgr = Ogre::TextureManager::getSingleton();
    auto it = mgr.getResourceIterator();
    QSet<QString> seen;

    while (it.hasMoreElements()) {
        Ogre::ResourcePtr res = it.getNext();
        Ogre::TexturePtr tex = std::dynamic_pointer_cast<Ogre::Texture>(res);
        if (!tex) continue;

        // Skip unloaded textures (no resident pixels yet).
        unsigned int w = tex->getWidth();
        unsigned int h = tex->getHeight();
        if (w == 0 || h == 0) continue;

        QString name = QString::fromStdString(tex->getName());
        if (seen.contains(name)) continue;
        seen.insert(name);

        TextureMemoryEstimate est;
        est.name = name;
        est.width = w;
        est.height = h;
        size_t bpp = Ogre::PixelUtil::getNumElemBytes(tex->getFormat());
        est.bytesPerPixel = static_cast<unsigned int>(bpp);
        est.hasMips = tex->getNumMipmaps() > 0;
        est.bytes = textureBytes(w, h, est.bytesPerPixel, est.hasMips);
        out.append(est);
    }

    return out;
}

SceneMemoryReport MemoryEstimator::estimateScene(quint64 budgetBytes)
{
    SceneMemoryReport report;
    report.budgetBytes = budgetBytes;

    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return report;

    QSet<QString> seenMeshes;
    for (Ogre::SceneNode* node : mgr->getSceneNodes()) {
        if (!node) continue;
        for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); ++i) {
            Ogre::MovableObject* obj = node->getAttachedObject(i);
            if (!obj || obj->getMovableType() != "Entity") continue;

            auto* entity = static_cast<Ogre::Entity*>(obj);
            MeshMemoryEstimate est = estimateEntity(entity);
            // De-duplicate identical mesh instances so totals reflect unique
            // GPU residents, not draw-call counts.
            if (est.name.isEmpty() || seenMeshes.contains(est.name)) continue;
            seenMeshes.insert(est.name);
            report.meshes.append(est);
            report.meshTotalBytes += est.totalBytes();
        }
    }

    report.textures = estimateAllTextures();
    for (const auto& t : report.textures)
        report.textureTotalBytes += t.bytes;

    return report;
}
// LCOV_EXCL_STOP

// ----- Serialisation --------------------------------------------------------

QJsonObject MemoryEstimator::toJson(const SceneMemoryReport& report)
{
    QJsonObject obj;

    QJsonArray meshArr;
    for (const auto& m : report.meshes) {
        QJsonObject mo;
        mo["name"] = m.name;
        mo["vertexCount"] = static_cast<int>(m.vertexCount);
        mo["indexCount"] = static_cast<int>(m.indexCount);
        mo["vertexBytes"] = static_cast<qint64>(m.vertexBytes);
        mo["indexBytes"] = static_cast<qint64>(m.indexBytes);
        mo["totalBytes"] = static_cast<qint64>(m.totalBytes());
        meshArr.append(mo);
    }
    obj["meshes"] = meshArr;

    QJsonArray texArr;
    for (const auto& t : report.textures) {
        QJsonObject to;
        to["name"] = t.name;
        to["width"] = static_cast<int>(t.width);
        to["height"] = static_cast<int>(t.height);
        to["bytesPerPixel"] = static_cast<int>(t.bytesPerPixel);
        to["hasMips"] = t.hasMips;
        to["bytes"] = static_cast<qint64>(t.bytes);
        texArr.append(to);
    }
    obj["textures"] = texArr;

    QJsonObject totals;
    totals["meshBytes"] = static_cast<qint64>(report.meshTotalBytes);
    totals["textureBytes"] = static_cast<qint64>(report.textureTotalBytes);
    totals["totalBytes"] = static_cast<qint64>(report.totalBytes());
    obj["totals"] = totals;

    if (report.budgetBytes > 0) {
        QJsonObject budget;
        budget["bytes"] = static_cast<qint64>(report.budgetBytes);
        budget["overBudget"] = report.overBudget();
        obj["budget"] = budget;
    }

    return obj;
}

QString MemoryEstimator::toText(const SceneMemoryReport& report)
{
    QString out;
    QTextStream s(&out);

    s << "Memory Report\n";
    s << "=============\n\n";

    s << "Meshes (" << report.meshes.size() << "):\n";
    if (report.meshes.isEmpty()) {
        s << "  (none)\n";
    } else {
        for (const auto& m : report.meshes) {
            s << "  " << m.name
              << "  v=" << m.vertexCount
              << "  i=" << m.indexCount
              << "  " << formatBytes(m.totalBytes()) << "\n";
        }
    }
    s << "  TOTAL: " << formatBytes(report.meshTotalBytes) << "\n\n";

    s << "Textures (" << report.textures.size() << "):\n";
    if (report.textures.isEmpty()) {
        s << "  (none)\n";
    } else {
        for (const auto& t : report.textures) {
            s << "  " << t.name
              << "  " << t.width << "x" << t.height
              << "  " << t.bytesPerPixel << "Bpp"
              << (t.hasMips ? "  +mips" : "")
              << "  " << formatBytes(t.bytes) << "\n";
        }
    }
    s << "  TOTAL: " << formatBytes(report.textureTotalBytes) << "\n\n";

    s << "Scene total: " << formatBytes(report.totalBytes()) << "\n";

    if (report.budgetBytes > 0) {
        s << "Budget:      " << formatBytes(report.budgetBytes);
        if (report.overBudget())
            s << "  *** OVER BUDGET ***";
        s << "\n";
    }

    return out;
}
