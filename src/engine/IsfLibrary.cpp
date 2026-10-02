#include "IsfLibrary.h"
#include "Isf.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

QString IsfLibrary::bundledFolder()
{
    const QString app = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        app + "/isf",                     // Windows / Linux: next to the executable
        app + "/../Resources/isf",        // macOS: Fulskrin.app/Contents/Resources/isf
        app + "/../share/fulskrin/isf",   // Linux, installed
        app + "/../isf",                  // build folder
        app + "/../../isf",
    };
    for (const QString &c : candidates)
        if (QFileInfo(c).isDir()) return QDir(c).absolutePath();
    return {};
}

QStringList IsfLibrary::systemFolders()
{
    QStringList f;
#ifdef Q_OS_MACOS
    f << "/Library/Graphics/ISF" << QDir::homePath() + "/Library/Graphics/ISF";
#elif defined(Q_OS_WIN)
    f << "C:/ProgramData/ISF";
#else
    f << "/usr/share/isf" << QDir::homePath() + "/.local/share/isf";
#endif
    return f;
}

QStringList IsfLibrary::allFolders() const
{
    QStringList all;
    const QString b = bundledFolder();
    if (!b.isEmpty()) all << b;
    for (const QString &s : systemFolders()) if (QFileInfo(s).isDir()) all << s;
    for (const QString &u : m_userFolders) if (QFileInfo(u).isDir() && !all.contains(u)) all << u;
    return all;
}

void IsfLibrary::scan()
{
    m_generators.clear();
    m_filters.clear();
    QSet<QString> seen;
    for (const QString &folder : allFolders()) {
        QDirIterator it(folder, {"*.fs", "*.frag"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = QFileInfo(it.next()).absoluteFilePath();
            if (seen.contains(path)) continue;
            seen.insert(path);
            IsfInstance::Header h = IsfInstance::readHeader(path);
            if (!h.ok || h.isTransition) continue;
            IsfEntry e;
            e.name = h.name;
            e.path = path;
            e.description = h.description;
            e.categories = h.categories;
            e.isFilter = h.isFilter;
            (e.isFilter ? m_filters : m_generators).push_back(e);
        }
    }
    auto byName = [](const IsfEntry &a, const IsfEntry &b) { return a.name.localeAwareCompare(b.name) < 0; };
    std::sort(m_generators.begin(), m_generators.end(), byName);
    std::sort(m_filters.begin(), m_filters.end(), byName);
}

QString IsfLibrary::findByFileName(const QString &fileName) const
{
    for (const IsfEntry &e : m_generators) if (QFileInfo(e.path).fileName() == fileName) return e.path;
    for (const IsfEntry &e : m_filters) if (QFileInfo(e.path).fileName() == fileName) return e.path;
    return {};
}
