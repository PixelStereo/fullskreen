#pragma once
// ISF shader library: folder bundled with the application, standard system folders
// (/Library/Graphics/ISF on macOS) and folders added by the user.

#include <QString>
#include <QStringList>
#include <QVector>

struct IsfEntry {
    QString name, path, description;
    QStringList categories;
    bool isFilter = false;
};

class IsfLibrary
{
public:
    void setUserFolders(const QStringList &f) { m_userFolders = f; }
    QStringList userFolders() const { return m_userFolders; }
    QStringList allFolders() const;

    void scan();
    const QVector<IsfEntry> &generators() const { return m_generators; }
    const QVector<IsfEntry> &filters() const { return m_filters; }

    // Finds a shader by file name (project moved from one machine to another).
    QString findByFileName(const QString &fileName) const;

    static QString bundledFolder();
    static QStringList systemFolders();

private:
    QStringList m_userFolders;
    QVector<IsfEntry> m_generators, m_filters;
};
