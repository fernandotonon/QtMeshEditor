// Phase 13 enforcement: the TRELLIS.2 integration must never (re)introduce the
// prohibited NVIDIA research-only libraries. nvdiffrast and nvdiffrec are
// under the NVIDIA Source Code License ("research or evaluation purposes
// only") and are excluded from QtMeshEditor's TRELLIS.2 backend end-to-end —
// see docs/trellis2-dependencies.md. This test scans the sidecar sources and
// dependency manifests from the repo (QTMESH_UT_SOURCE_ROOT) and fails if any
// non-allowlisted reference appears. scripts/check-trellis2-restricted-deps.sh
// is the same gate for CI contexts without the test binary.
#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace {

QString sourceRoot()
{
#ifdef QTMESH_UT_SOURCE_ROOT
    return QString::fromUtf8(QTMESH_UT_SOURCE_ROOT);
#else
    return {};
#endif
}

// Lines that may legitimately NAME the prohibited packages: prohibition
// comments/docs, the startup guard, and the dependency report. Everything is
// judged line-by-line so an actual `import nvdiffrast` can never hide behind
// an allowlisted keyword elsewhere in the file.
bool lineIsAllowlisted(const QString& line)
{
    const QString t = line.trimmed();
    // Python/requirements comments and Markdown prose are documentation.
    // NB: only a Markdown BULLET ("- text") is prose — a bare '-' prefix
    // would also allowlist pip requirement flags like
    // "-e git+…/nvdiffrast.git#egg=nvdiffrast".
    if (t.startsWith(QLatin1Char('#')) || t.startsWith(QLatin1Char('*'))
        || t.startsWith(QLatin1String("- ")) || t.startsWith(QLatin1Char('|'))
        || t.startsWith(QLatin1Char('>')))
        return true;
    // The explicit guard/report constructs in generate.py.
    if (t.contains(QLatin1String("PROHIBITED_MODULES"))
        || t.contains(QLatin1String("NOT INSTALLED")))
        return true;
    // Docstring prose (module docstrings mention the exclusion by name).
    if (t.startsWith(QLatin1Char('"')) || t.startsWith(QLatin1String("'''"))
        || t.contains(QLatin1String("are PROHIBITED"))
        || t.contains(QLatin1String("prohibited")))
        return true;
    return false;
}

} // namespace

TEST(Trellis2GuardTest, SidecarNeverImportsRestrictedNvidiaLibraries)
{
    const QString root = sourceRoot();
    ASSERT_FALSE(root.isEmpty());
    const QDir dir(QDir(root).filePath(QStringLiteral("ai/trellis2")));
    ASSERT_TRUE(dir.exists()) << "ai/trellis2 sidecar directory missing";

    const QStringList banned = {QStringLiteral("nvdiffrast"),
                                QStringLiteral("nvdiffrec")};
    QStringList violations;
    QDirIterator it(dir.absolutePath(),
                    {QStringLiteral("*.py"), QStringLiteral("*.txt"),
                     QStringLiteral("*.toml"), QStringLiteral("*.cfg")},
                    QDir::Files, QDirIterator::Subdirectories);
    int scanned = 0;
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.contains(QLatin1String("/env/"))
            || path.contains(QLatin1String("/TRELLIS.2/")))
            continue;   // only OUR sidecar files, not a user-installed runtime
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        ++scanned;
        int lineNo = 0;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine());
            ++lineNo;
            for (const QString& b : banned) {
                if (!line.contains(b, Qt::CaseInsensitive))
                    continue;
                // A real import/require is a violation no matter what.
                const bool isImport =
                    line.contains(QLatin1String("import nvdiffrast"))
                    || line.contains(QLatin1String("import nvdiffrec"))
                    || line.contains(QLatin1String("from nvdiffrast"))
                    || line.contains(QLatin1String("from nvdiffrec"));
                if (isImport || !lineIsAllowlisted(line))
                    violations << QStringLiteral("%1:%2: %3")
                                      .arg(path).arg(lineNo)
                                      .arg(line.trimmed());
            }
        }
    }
    EXPECT_GT(scanned, 2);   // requirements.txt + generate.py + qtm3d.py at least
    EXPECT_TRUE(violations.isEmpty())
        << "prohibited nvdiffrast/nvdiffrec reference(s):\n"
        << violations.join(QLatin1Char('\n')).toStdString();
}

TEST(Trellis2GuardTest, RequirementsNeverListRestrictedOrTrapPackages)
{
    const QString root = sourceRoot();
    ASSERT_FALSE(root.isEmpty());
    QFile req(QDir(root).filePath(QStringLiteral("ai/trellis2/requirements.txt")));
    ASSERT_TRUE(req.open(QIODevice::ReadOnly));
    while (!req.atEnd()) {
        const QString line = QString::fromUtf8(req.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QString pkg = line.split(QRegularExpression(
            QStringLiteral("[=<>!\\[; ]"))).value(0).toLower();
        EXPECT_NE(pkg, QStringLiteral("nvdiffrast")) << line.toStdString();
        EXPECT_NE(pkg, QStringLiteral("nvdiffrec")) << line.toStdString();
        // The PyPI package named `cumesh` is an unrelated, unlicensed project —
        // CuMesh must be built from the pinned JeffreyXiang/CuMesh checkout.
        EXPECT_NE(pkg, QStringLiteral("cumesh")) << line.toStdString();
        // Editable/VCS requirement forms ("-e git+…#egg=nvdiffrast",
        // "nvdiffrast @ git+…") hide the name from the pkg-prefix parse —
        // ban the names ANYWHERE in a non-comment requirement line.
        const QString low = line.toLower();
        EXPECT_FALSE(low.contains(QLatin1String("nvdiffrast"))
                     || low.contains(QLatin1String("nvdiffrec"))
                     || low.contains(QLatin1String("cumesh")))
            << line.toStdString();
    }
}

TEST(Trellis2GuardTest, CppIntegrationNeverReferencesRestrictedLibraries)
{
    const QString root = sourceRoot();
    ASSERT_FALSE(root.isEmpty());
    // The C++ replacement layer must be genuinely independent: outside of
    // exclusion comments, the sources may not reference the NVIDIA libraries
    // at all (no bindings, no dlopen, no subprocess invocations).
    const QStringList files = {
        QStringLiteral("src/ImageTo3D/Trellis2Predictor.cpp"),
        QStringLiteral("src/ImageTo3D/Trellis2Bake.cpp"),
        QStringLiteral("src/ImageTo3D/Trellis2Interchange.cpp"),
    };
    for (const QString& rel : files) {
        QFile f(QDir(root).filePath(rel));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << rel.toStdString();
        int lineNo = 0;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine());
            ++lineNo;
            if (!line.contains(QLatin1String("nvdiff"), Qt::CaseInsensitive))
                continue;
            const QString t = line.trimmed();
            EXPECT_TRUE(t.startsWith(QLatin1String("//"))
                        || t.startsWith(QLatin1String("*")))
                << rel.toStdString() << ":" << lineNo << ": "
                << t.toStdString();
        }
    }
}
