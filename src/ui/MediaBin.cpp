#include "MediaBin.h"
#include "Engine.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QSet>
#include <functional>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QThreadPool>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
enum Role { PathRole = Qt::UserRole + 1, MissingRole, UsedRole };

// Arbre du chutier : glisser des éléments vers les calques, déposer des fichiers pour les importer.
class BinTree : public QTreeWidget
{
public:
    std::function<void(const QStringList &)> onDrop;

protected:
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        auto *m = new QMimeData;
        QList<QUrl> urls;
        for (QTreeWidgetItem *it : items) {
            const QString p = it->data(0, PathRole).toString();
            if (!p.isEmpty() && !it->data(0, MissingRole).toBool()) urls << QUrl::fromLocalFile(p);
        }
        m->setUrls(urls);
        return m;
    }
    QStringList mimeTypes() const override { return {QStringLiteral("text/uri-list")}; }
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (e->source() == this || !e->mimeData()->hasUrls()) {
            e->ignore();
            return;
        }
        e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *e) override
    {
        if (e->source() == this) e->ignore();
        else e->acceptProposedAction();
    }
    void dropEvent(QDropEvent *e) override
    {
        QStringList paths;
        for (const QUrl &u : e->mimeData()->urls())
            if (u.isLocalFile()) paths << u.toLocalFile();
        if (onDrop) onDrop(paths);
        e->acceptProposedAction();
    }
};

QString fmtDuration(double s)
{
    const int t = int(s + 0.5);
    return t >= 3600 ? QStringLiteral("%1:%2:%3").arg(t / 3600).arg((t / 60) % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'))
                     : QStringLiteral("%1:%2").arg(t / 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
}
} // namespace

MediaBin::MediaBin(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 4, 4);
    v->setSpacing(6);
    auto *head = new QHBoxLayout;
    head->addWidget(new QLabel(QStringLiteral("<b>Chutier</b>")));
    head->addStretch();
    auto *import = new QPushButton(QStringLiteral("Importer…"));
    import->setToolTip(QStringLiteral("Ajouter des images ou des vidéos au chutier (sans créer de calque)"));
    head->addWidget(import);
    v->addLayout(head);

    auto *tree = new BinTree;
    m_tree = tree;
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("Fichier"), QStringLiteral("Infos"), QStringLiteral("Calques")});
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_tree->setColumnWidth(1, 120);
    m_tree->setColumnWidth(2, 56);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setToolTip(QStringLiteral("Glissez un fichier vers la liste des calques ou l'aperçu pour créer un calque.\n"
                                      "Double-clic : remplacer la source du calque sélectionné."));
    tree->onDrop = [this](const QStringList &p) { importFiles(p); };
    m_videos = new QTreeWidgetItem(m_tree, {QStringLiteral("Vidéos")});
    m_images = new QTreeWidgetItem(m_tree, {QStringLiteral("Images")});
    for (QTreeWidgetItem *cat : {m_videos, m_images}) {
        QFont f = cat->font(0);
        f.setBold(true);
        cat->setFont(0, f);
        cat->setFlags(Qt::ItemIsEnabled);
        cat->setExpanded(true);
        cat->setFirstColumnSpanned(true);
    }
    v->addWidget(m_tree, 1);

    auto *buttons = new QHBoxLayout;
    m_relink = new QPushButton(QStringLiteral("Remplacer…"));
    m_relink->setToolTip(QStringLiteral("Remplacer ce fichier par un autre partout où il est utilisé (fichier déplacé, nouvelle version)"));
    m_remove = new QPushButton(QStringLiteral("Retirer"));
    m_remove->setToolTip(QStringLiteral("Retirer du chutier (seulement les fichiers qu'aucun calque n'utilise)"));
#if defined(Q_OS_MACOS)
    m_reveal = new QPushButton(QStringLiteral("Finder"));
#elif defined(Q_OS_WIN)
    m_reveal = new QPushButton(QStringLiteral("Explorateur"));
#else
    m_reveal = new QPushButton(QStringLiteral("Dossier"));
#endif
    m_reveal->setToolTip(QStringLiteral("Afficher le fichier dans son dossier"));
    for (auto *b : {m_relink, m_remove, m_reveal}) buttons->addWidget(b);
    v->addLayout(buttons);
    m_summary = new QLabel;
    m_summary->setStyleSheet("color:#888; font-size:11px;");
    m_summary->setWordWrap(true);
    v->addWidget(m_summary);

    connect(import, &QPushButton::clicked, this, &MediaBin::importDialog);
    connect(m_relink, &QPushButton::clicked, this, &MediaBin::relinkSelected);
    connect(m_remove, &QPushButton::clicked, this, &MediaBin::removeSelected);
    connect(m_reveal, &QPushButton::clicked, this, &MediaBin::revealSelected);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &MediaBin::updateButtons);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &MediaBin::contextMenu);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        const QString p = it->data(0, PathRole).toString();
        if (!p.isEmpty() && !it->data(0, MissingRole).toBool()) emit useAsSourceRequested(p);
    });
    refresh();
}

QString MediaBin::selectedPath() const
{
    const auto items = m_tree->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, PathRole).toString();
}

void MediaBin::refresh()
{
    const auto usage = m_engine->mediaUsage();
    const QString selected = selectedPath();

    int nv = 0, ni = 0, missing = 0;
    // Mise à jour sur place : on garde la sélection et le défilement.
    QHash<QString, QTreeWidgetItem *> existing;
    for (QTreeWidgetItem *cat : {m_videos, m_images})
        for (int i = 0; i < cat->childCount(); ++i) existing.insert(cat->child(i)->data(0, PathRole).toString(), cat->child(i));
    QSet<QString> seen;
    for (const auto &r : usage) {
        QTreeWidgetItem *cat = r.video ? m_videos : m_images;
        QTreeWidgetItem *it = existing.value(r.path);
        if (it && it->parent() != cat) {
            delete it;
            it = nullptr;
        }
        if (!it) {
            it = new QTreeWidgetItem(cat);
            it->setData(0, PathRole, r.path);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        seen.insert(r.path);
        (r.video ? nv : ni)++;
        if (r.missing) ++missing;
        it->setData(0, MissingRole, r.missing);
        it->setData(0, UsedRole, !r.users.isEmpty());
        it->setText(0, QFileInfo(r.path).fileName());
        if (r.missing) {
            it->setText(1, QStringLiteral("introuvable"));
            m_info.remove(r.path);
        } else {
            if (!m_info.contains(r.path)) probe(r.path);
            const QString info = m_info.value(r.path, QStringLiteral("…"));
            it->setText(1, info.section('\t', 0, 0));
            it->setToolTip(1, info.contains('\t') ? info.section('\t', 0, 0) + QStringLiteral(" · ") + info.section('\t', 1) : info);
        }
        if (r.missing) it->setToolTip(1, QStringLiteral("Fichier introuvable : ") + r.path);
        // Nombre de calques ; le détail est dans l'infobulle
        it->setText(2, r.users.isEmpty() ? QStringLiteral("—") : QString::number(r.users.size()));
        it->setTextAlignment(2, Qt::AlignCenter);
        const QString usedBy = r.users.isEmpty() ? QStringLiteral("Non utilisé (importé dans le chutier)")
                                                 : QStringLiteral("Utilisé par :\n") + r.users.join('\n');
        it->setToolTip(0, r.path + QStringLiteral("\n\n") + usedBy);
        it->setToolTip(2, usedBy);
        const QColor fg = r.missing ? QColor(255, 110, 95) : (r.users.isEmpty() ? QColor(140, 140, 145) : QColor(225, 225, 228));
        for (int c = 0; c < 3; ++c) it->setForeground(c, fg);
    }
    for (auto e = existing.begin(); e != existing.end(); ++e)
        if (!seen.contains(e.key())) delete e.value();
    for (QTreeWidgetItem *cat : {m_videos, m_images}) cat->sortChildren(0, Qt::AscendingOrder);
    m_videos->setText(0, QStringLiteral("Vidéos (%1)").arg(nv));
    m_images->setText(0, QStringLiteral("Images (%1)").arg(ni));
    m_summary->setText(missing ? QStringLiteral("<span style='color:#ff6e5f'>%1 fichier(s) introuvable(s) : "
                                                "sélectionnez-les puis « Remplacer… »</span>").arg(missing)
                               : QStringLiteral("%1 fichier(s)").arg(nv + ni));
    if (!selected.isEmpty())
        for (QTreeWidgetItem *cat : {m_videos, m_images})
            for (int i = 0; i < cat->childCount(); ++i)
                if (cat->child(i)->data(0, PathRole).toString() == selected) m_tree->setCurrentItem(cat->child(i));
    updateButtons();
}

void MediaBin::probe(const QString &path)
{
    if (m_probing.value(path)) return;
    m_probing[path] = true;
    QPointer<MediaBin> self(this);
    const bool video = Engine::isVideoFile(path) || !Engine::isImageFile(path);
    // Lecture des métadonnées hors du fil de l'interface (fichiers sur disque réseau, etc.)
    QThreadPool::globalInstance()->start([self, path, video] {
        QString info;
        if (video) {
            VideoDecoder::Info vi;
            if (VideoDecoder::probe(path, &vi)) {
                // Colonne courte (résolution · durée) ; le détail est dans l'infobulle
                info = QStringLiteral("%1×%2").arg(vi.width).arg(vi.height);
                if (vi.duration > 0) info += QStringLiteral(" · ") + fmtDuration(vi.duration);
                QStringList more;
                if (vi.fps > 0) more << QStringLiteral("%1 i/s").arg(vi.fps, 0, 'g', 4);
                if (!vi.codec.isEmpty()) more << vi.codec;
                if (!more.isEmpty()) info += QStringLiteral("\t") + more.join(QStringLiteral(" · "));
            } else {
                info = QStringLiteral("illisible");
            }
        } else {
            QImageReader r(path);
            const QSize s = r.size();
            info = s.isValid() ? QStringLiteral("%1×%2 · %3").arg(s.width()).arg(s.height()).arg(QString::fromLatin1(r.format()))
                               : QStringLiteral("illisible");
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, path, info] {
                if (!self) return;
                self->m_info[path] = info;
                self->m_probing.remove(path);
                self->refresh();
            },
            Qt::QueuedConnection);
    });
}

void MediaBin::updateButtons()
{
    bool any = false, removable = false;
    for (QTreeWidgetItem *it : m_tree->selectedItems()) {
        if (it->data(0, PathRole).toString().isEmpty()) continue;
        any = true;
        removable |= !it->data(0, UsedRole).toBool();
    }
    m_relink->setEnabled(m_tree->selectedItems().size() == 1 && any);
    m_remove->setEnabled(removable);
    m_reveal->setEnabled(any && !m_tree->selectedItems().first()->data(0, MissingRole).toBool());
}

void MediaBin::importFiles(const QStringList &paths)
{
    QStringList ok;
    for (const QString &p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            // Un dossier déposé : on importe ses images et vidéos (premier niveau)
            for (const QFileInfo &f : QDir(p).entryInfoList(QDir::Files, QDir::Name))
                if (Engine::isVideoFile(f.filePath()) || Engine::isImageFile(f.filePath())) ok << f.absoluteFilePath();
        } else if (Engine::isVideoFile(p) || Engine::isImageFile(p)) {
            ok << fi.absoluteFilePath();
        }
    }
    if (ok.isEmpty()) return;
    m_engine->addBinItems(ok);
    refresh();
    emit binEdited();
}

void MediaBin::importDialog()
{
    QSettings s;
    QStringList ext;
    for (const QString &e : {"mov", "mp4", "m4v", "avi", "mkv", "webm", "mxf", "mpg", "png", "jpg", "jpeg", "tif", "tiff", "bmp", "gif", "webp", "tga"})
        ext << "*." + QString(e);
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Importer dans le chutier"),
                                                            s.value("dirs/video").toString(),
                                                            QStringLiteral("Images et vidéos (%1);;Tous les fichiers (*)").arg(ext.join(' ')));
    if (files.isEmpty()) return;
    s.setValue("dirs/video", QFileInfo(files.first()).absolutePath());
    importFiles(files);
}

void MediaBin::relinkSelected()
{
    const QString from = selectedPath();
    if (from.isEmpty()) return;
    QString start = QFileInfo(from).absolutePath();
    if (!QFileInfo(start).isDir()) start = QSettings().value("dirs/video").toString();
    const QString to = QFileDialog::getOpenFileName(this, QStringLiteral("Remplacer « %1 » par…").arg(QFileInfo(from).fileName()),
                                                    start + "/" + QFileInfo(from).fileName());
    if (to.isEmpty() || to == from) return;
    emit relinkRequested(from, QFileInfo(to).absoluteFilePath());
}

void MediaBin::removeSelected()
{
    for (QTreeWidgetItem *it : m_tree->selectedItems())
        if (!it->data(0, UsedRole).toBool()) m_engine->removeBinItem(it->data(0, PathRole).toString());
    refresh();
    emit binEdited();
}

void MediaBin::revealSelected()
{
    const QString p = selectedPath();
    if (p.isEmpty()) return;
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), p});
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"), {QStringLiteral("/select,"), QDir::toNativeSeparators(p)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath()));
#endif
}

void MediaBin::contextMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_tree->itemAt(pos);
    const QString p = it ? it->data(0, PathRole).toString() : QString();
    QMenu menu(this);
    if (!p.isEmpty()) {
        const bool missing = it->data(0, MissingRole).toBool();
        QAction *a = menu.addAction(QStringLiteral("Nouveau calque avec ce fichier"), this, [this, p] { emit newLayerRequested(p); });
        a->setEnabled(!missing);
        a = menu.addAction(QStringLiteral("Source du calque sélectionné"), this, [this, p] { emit useAsSourceRequested(p); });
        a->setEnabled(!missing);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remplacer le fichier…"), this, &MediaBin::relinkSelected);
        menu.addAction(m_reveal->text(), this, &MediaBin::revealSelected)->setEnabled(!missing);
        menu.addAction(QStringLiteral("Retirer du chutier"), this, &MediaBin::removeSelected)->setEnabled(!it->data(0, UsedRole).toBool());
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("Importer…"), this, &MediaBin::importDialog);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}
