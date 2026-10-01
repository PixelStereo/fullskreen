#pragma once
// Bibliothèque de shaders ISF : dossier fourni avec l'application, dossiers système
// standards (/Library/Graphics/ISF sur macOS) et dossiers ajoutés par l'utilisateur.

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

    // Retrouve un shader par nom de fichier (projet déplacé d'une machine à l'autre).
    QString findByFileName(const QString &fileName) const;

    static QString bundledFolder();
    static QStringList systemFolders();

private:
    QStringList m_userFolders;
    QVector<IsfEntry> m_generators, m_filters;
};
