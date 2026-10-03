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
#include <QMap>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QThreadPool>
#include <QLineEdit>
#include <functional>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

namespace {
enum Role { PathRole = Qt::UserRole + 1, MissingRole, UsedRole, IsfRole, CategoryRole };

// Media Bin tree: drag items onto layers, drop files to import them.
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
    head->addWidget(new QLabel(QStringLiteral("<b>Media Bin</b>")));
    head->addStretch();
    auto *import = new QPushButton(QStringLiteral("Import…"));
    import->setToolTip(QStringLiteral("Add videos, images or sounds to the Media Bin"));
    head->addWidget(import);
    v->addLayout(head);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search…"));
    m_search->setClearButtonEnabled(true);
    m_search->setToolTip(QStringLiteral("Shows only the media and generators whose name contains this text"));
    v->addWidget(m_search);
    connect(m_search, &QLineEdit::textChanged, this, &MediaBin::applyFilter);

    auto *tree = new BinTree;
    m_tree = tree;
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Info"), QStringLiteral("Layers")});
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setUniformRowHeights(true);
    m_tree->setIndentation(12); // ISF > Generators > category > shader: keep room for the names
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_tree->setColumnWidth(1, 120);
    m_tree->setColumnWidth(2, 56);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setToolTip(QStringLiteral("Drag an item onto a layer (layer list, or the Source tab of the layer) to load it.\n"
                                      "ISF effects dragged onto a layer join its effect chain.\n"
                                      "Double-click: load into the selected layer."));
    tree->onDrop = [this](const QStringList &p) { importFiles(p); };
    m_videos = new QTreeWidgetItem(m_tree, {QStringLiteral("Videos")});
    m_images = new QTreeWidgetItem(m_tree, {QStringLiteral("Images")});
    m_audios = new QTreeWidgetItem(m_tree, {QStringLiteral("Audio")});
    m_isf = new QTreeWidgetItem(m_tree, {QStringLiteral("ISF")});
    m_isfGenerators = new QTreeWidgetItem(m_isf, {QStringLiteral("Generators")});
    for (QTreeWidgetItem *cat : {m_videos, m_images, m_audios, m_isf, m_isfGenerators}) {
        QFont f = cat->font(0);
        f.setBold(true);
        cat->setFont(0, f);
        cat->setFlags(Qt::ItemIsEnabled);
        cat->setExpanded(true);
        cat->setFirstColumnSpanned(true);
    }
    v->addWidget(m_tree, 1);

    auto *buttons = new QHBoxLayout;
    m_relink = new QPushButton(QStringLiteral("Replace…"));
    m_relink->setToolTip(QStringLiteral("Replace this file with another everywhere it is used (moved file, new version)"));
    m_remove = new QPushButton(QStringLiteral("Remove"));
    m_remove->setToolTip(QStringLiteral("Remove from the Media Bin (only files not used by any layer)"));
#if defined(Q_OS_MACOS)
    m_reveal = new QPushButton(QStringLiteral("Finder"));
#elif defined(Q_OS_WIN)
    m_reveal = new QPushButton(QStringLiteral("Explorer"));
#else
    m_reveal = new QPushButton(QStringLiteral("Folder"));
#endif
    m_reveal->setToolTip(QStringLiteral("Show the file in its folder"));
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

    int nv = 0, ni = 0, na = 0, missing = 0;
    // Update in place: keep selection and scroll position.
    QHash<QString, QTreeWidgetItem *> existing;
    for (QTreeWidgetItem *cat : {m_videos, m_images, m_audios})
        for (int i = 0; i < cat->childCount(); ++i) existing.insert(cat->child(i)->data(0, PathRole).toString(), cat->child(i));
    QSet<QString> seen;
    for (const auto &r : usage) {
        QTreeWidgetItem *cat = r.video ? m_videos : r.audio ? m_audios : m_images;
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
        (r.video ? nv : r.audio ? na : ni)++;
        if (r.missing) ++missing;
        it->setData(0, MissingRole, r.missing);
        it->setData(0, UsedRole, !r.users.isEmpty());
        it->setText(0, QFileInfo(r.path).fileName());
        if (r.missing) {
            it->setText(1, QStringLiteral("missing"));
            m_info.remove(r.path);
        } else {
            if (!m_info.contains(r.path)) probe(r.path);
            const QString info = m_info.value(r.path, QStringLiteral("…"));
            it->setText(1, info.section('\t', 0, 0));
            it->setToolTip(1, info.contains('\t') ? info.section('\t', 0, 0) + QStringLiteral(" · ") + info.section('\t', 1) : info);
        }
        if (r.missing) it->setToolTip(1, QStringLiteral("File not found: ") + r.path);
        // Layer count; details are in the tooltip
        it->setText(2, r.users.isEmpty() ? QStringLiteral("—") : QString::number(r.users.size()));
        it->setTextAlignment(2, Qt::AlignCenter);
        const QString usedBy = r.users.isEmpty() ? QStringLiteral("Unused (imported to the Media Bin)")
                                                 : QStringLiteral("Used by:\n") + r.users.join('\n');
        it->setToolTip(0, r.path + QStringLiteral("\n\n") + usedBy);
        it->setToolTip(2, usedBy);
        const QColor fg = r.missing ? QColor(255, 110, 95) : (r.users.isEmpty() ? QColor(140, 140, 145) : QColor(225, 225, 228));
        for (int c = 0; c < 3; ++c) it->setForeground(c, fg);
    }
    for (auto e = existing.begin(); e != existing.end(); ++e)
        if (!seen.contains(e.key())) delete e.value();
    for (QTreeWidgetItem *cat : {m_videos, m_images, m_audios}) cat->sortChildren(0, Qt::AscendingOrder);
    m_videos->setText(0, QStringLiteral("Videos (%1)").arg(nv));
    m_images->setText(0, QStringLiteral("Images (%1)").arg(ni));
    m_audios->setText(0, QStringLiteral("Audio (%1)").arg(na));
    m_summary->setText(missing ? QStringLiteral("<span style='color:#ff6e5f'>%1 missing file(s): "
                                                "select them, then \"Replace…\"</span>").arg(missing)
                               : QStringLiteral("%1 file(s)").arg(nv + ni + na));
    refreshIsf();
    if (!selected.isEmpty())
        for (QTreeWidgetItemIterator it(m_tree); *it; ++it) // ISF items are one level deeper (category)
            if ((*it)->data(0, PathRole).toString() == selected) {
                m_tree->setCurrentItem(*it);
                break;
            }
    updateButtons();
    applyFilter();
}

// ISF > Generators: the shaders of the library (bundled, system and added folders) and those used by layers.
// Category a generator is filed under: its first ISF category, "Generator" aside (they all are).
static QString isfCategory(const QStringList &categories)
{
    for (const QString &c : categories) {
        const QString t = c.trimmed();
        if (!t.isEmpty() && t.compare(QStringLiteral("Generator"), Qt::CaseInsensitive) != 0 &&
            t.compare(QStringLiteral("Generators"), Qt::CaseInsensitive) != 0)
            return t;
    }
    return QStringLiteral("Other");
}

// ISF > Generators > <category>: the shaders of the library (bundled, system and added folders) and those used
// by layers, filed by their ISF category.
void MediaBin::refreshIsf()
{
    struct Gen {
        QString name, category, description;
        QStringList users;
        bool inLibrary = false;
    };
    QMap<QString, Gen> gens;
    for (const IsfEntry &e : m_engine->library().generators())
        gens[QDir::cleanPath(e.path)] = {e.name, isfCategory(e.categories), e.description, {}, true};
    {
        Engine::Lock lk(&m_engine->mutex());
        for (int i = 0; i < m_engine->layerCount(); ++i) {
            const Layer *l = m_engine->layer(i);
            if (l->type != SourceType::Isf || l->sourcePath.isEmpty()) continue;
            Gen &g = gens[QDir::cleanPath(QFileInfo(l->sourcePath).absoluteFilePath())];
            if (g.name.isEmpty()) {
                const IsfInstance::Header h = IsfInstance::readHeader(l->sourcePath);
                g.name = QFileInfo(l->sourcePath).completeBaseName();
                g.category = isfCategory(h.categories);
                g.description = h.description;
            }
            g.users << l->name;
        }
    }
    // Existing items (kept: selection, expanded categories), by path and by category
    QHash<QString, QTreeWidgetItem *> existing, categories;
    for (int c = 0; c < m_isfGenerators->childCount(); ++c) {
        QTreeWidgetItem *cat = m_isfGenerators->child(c);
        categories.insert(cat->data(0, CategoryRole).toString(), cat);
        for (int i = 0; i < cat->childCount(); ++i) existing.insert(cat->child(i)->data(0, PathRole).toString(), cat->child(i));
    }
    QSet<QString> usedCategories;
    for (auto g = gens.cbegin(); g != gens.cend(); ++g) {
        QTreeWidgetItem *cat = categories.value(g->category);
        if (!cat) {
            cat = new QTreeWidgetItem(m_isfGenerators);
            cat->setData(0, CategoryRole, g->category);
            cat->setFlags(Qt::ItemIsEnabled);
            cat->setFirstColumnSpanned(true);
            cat->setExpanded(true);
            QFont f = cat->font(0);
            f.setItalic(true);
            cat->setFont(0, f);
            cat->setForeground(0, QColor(200, 200, 205));
            categories.insert(g->category, cat);
        }
        usedCategories.insert(g->category);
        QTreeWidgetItem *it = existing.take(g.key());
        if (it && it->parent() != cat) { // category changed
            delete it;
            it = nullptr;
        }
        if (!it) {
            it = new QTreeWidgetItem(cat);
            it->setData(0, PathRole, g.key());
            it->setData(0, IsfRole, true);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        const bool missing = !QFileInfo::exists(g.key());
        it->setData(0, MissingRole, missing);
        it->setData(0, UsedRole, true); // library shaders are not removed from the Media Bin
        it->setText(0, g->name);
        it->setText(1, missing ? QStringLiteral("missing") : g->inLibrary ? QString() : QStringLiteral("outside library"));
        it->setText(2, g->users.isEmpty() ? QStringLiteral("—") : QString::number(g->users.size()));
        it->setTextAlignment(2, Qt::AlignCenter);
        const QString usedBy = g->users.isEmpty() ? QStringLiteral("Not used by any layer")
                                                  : QStringLiteral("Used by:\n") + g->users.join('\n');
        it->setToolTip(0, g.key() + (g->description.isEmpty() ? QString() : QStringLiteral("\n\n") + g->description) +
                              QStringLiteral("\n\n") + usedBy);
        it->setToolTip(1, g->description);
        it->setToolTip(2, usedBy);
        const QColor fg = missing ? QColor(255, 110, 95) : (g->users.isEmpty() ? QColor(170, 170, 175) : QColor(225, 225, 228));
        for (int c = 0; c < 3; ++c) it->setForeground(c, fg);
    }
    qDeleteAll(existing);
    for (auto c = categories.begin(); c != categories.end(); ++c)
        if (!usedCategories.contains(c.key())) delete c.value();
    // Opened at the first fill: an item expanded while still empty may stay folded
    if (!m_isfOpened && m_isfGenerators->childCount() > 0) {
        m_isfOpened = true;
        m_isf->setExpanded(true);
        m_isfGenerators->setExpanded(true);
        for (int c = 0; c < m_isfGenerators->childCount(); ++c) m_isfGenerators->child(c)->setExpanded(true);
    }
    int total = 0;
    for (int c = 0; c < m_isfGenerators->childCount(); ++c) {
        QTreeWidgetItem *cat = m_isfGenerators->child(c);
        cat->sortChildren(0, Qt::AscendingOrder);
        cat->setText(0, QStringLiteral("%1 (%2)").arg(cat->data(0, CategoryRole).toString()).arg(cat->childCount()));
        total += cat->childCount();
    }
    m_isfGenerators->sortChildren(0, Qt::AscendingOrder);
    m_isfGenerators->setText(0, QStringLiteral("Generators (%1)").arg(total));
}

void MediaBin::probe(const QString &path)
{
    if (m_probing.value(path)) return;
    m_probing[path] = true;
    QPointer<MediaBin> self(this);
    const bool image = Engine::isImageFile(path);
    // Read metadata off the UI thread (files on network drives, etc.)
    QThreadPool::globalInstance()->start([self, path, image] {
        QString info;
        AudioStream::Info ai;
        const bool sound = !image && AudioStream::probe(path, &ai);
        const QString soundText = sound ? QStringLiteral("%1 %2 kHz %3")
                                              .arg(ai.codec)
                                              .arg(ai.sampleRate / 1000.0, 0, 'g', 3)
                                              .arg(ai.channels == 1 ? QStringLiteral("mono")
                                                   : ai.channels == 2 ? QStringLiteral("stereo")
                                                                      : QStringLiteral("%1 ch").arg(ai.channels))
                                        : QString();
        if (!image) {
            VideoDecoder::Info vi;
            if (Engine::isAudioFile(path) || !VideoDecoder::probe(path, &vi)) {
                // Sound only
                info = sound ? (ai.duration > 0 ? fmtDuration(ai.duration) + QStringLiteral(" · ") : QString()) +
                                   QStringLiteral("%1 kHz").arg(ai.sampleRate / 1000.0, 0, 'g', 3) + QStringLiteral("\t") +
                                   soundText
                             : QStringLiteral("unreadable");
            } else {
                // Short column (resolution · duration); details are in the tooltip
                info = QStringLiteral("%1×%2").arg(vi.width).arg(vi.height);
                if (vi.duration > 0) info += QStringLiteral(" · ") + fmtDuration(vi.duration);
                QStringList more;
                if (vi.fps > 0) more << QStringLiteral("%1 fps").arg(vi.fps, 0, 'g', 4);
                if (!vi.codec.isEmpty()) more << vi.codec;
                more << (sound ? QStringLiteral("sound: ") + soundText : QStringLiteral("no sound"));
                info += QStringLiteral("\t") + more.join(QStringLiteral(" · "));
            }
        } else {
            QImageReader r(path);
            const QSize s = r.size();
            info = s.isValid() ? QStringLiteral("%1×%2 · %3").arg(s.width()).arg(s.height()).arg(QString::fromLatin1(r.format()))
                               : QStringLiteral("unreadable");
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
    bool any = false, removable = false, isf = false;
    for (QTreeWidgetItem *it : m_tree->selectedItems()) {
        if (it->data(0, PathRole).toString().isEmpty()) continue;
        any = true;
        isf |= it->data(0, IsfRole).toBool();
        removable |= !it->data(0, UsedRole).toBool();
    }
    m_relink->setEnabled(m_tree->selectedItems().size() == 1 && any && !isf);
    m_remove->setEnabled(removable);
    m_reveal->setEnabled(any && !m_tree->selectedItems().first()->data(0, MissingRole).toBool());
}

static bool isMedia(const QString &p) { return Engine::isVideoFile(p) || Engine::isImageFile(p) || Engine::isAudioFile(p); }

void MediaBin::importFiles(const QStringList &paths)
{
    QStringList ok;
    for (const QString &p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            // A dropped folder: import its videos, images and audio files (top level only)
            for (const QFileInfo &f : QDir(p).entryInfoList(QDir::Files, QDir::Name))
                if (isMedia(f.filePath())) ok << f.absoluteFilePath();
        } else if (isMedia(p)) {
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
    for (const QString &e : Engine::videoExtensions() + Engine::imageExtensions() + Engine::audioExtensions()) ext << "*." + e;
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import to Media Bin"),
                                                            s.value("dirs/video").toString(),
                                                            QStringLiteral("Videos, Images and Audio (%1);;All Files (*)").arg(ext.join(' ')));
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
    const QString to = QFileDialog::getOpenFileName(this, QStringLiteral("Replace \"%1\" with…").arg(QFileInfo(from).fileName()),
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
        const bool isf = it->data(0, IsfRole).toBool();
        QAction *a = menu.addAction(QStringLiteral("Load into Selected Layer"), this, [this, p] { emit useAsSourceRequested(p); });
        a->setEnabled(!missing);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Relink File…"), this, &MediaBin::relinkSelected)->setEnabled(!isf);
        menu.addAction(m_reveal->text(), this, &MediaBin::revealSelected)->setEnabled(!missing);
        menu.addAction(QStringLiteral("Remove from Media Bin"), this, &MediaBin::removeSelected)->setEnabled(!it->data(0, UsedRole).toBool());
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("Import…"), this, &MediaBin::importDialog);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MediaBin::applyFilter()
{
    const QString f = m_search ? m_search->text().trimmed() : QString();
    // Leaves (media, shaders) match by name; a category stays visible if one of its items does
    std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) -> bool {
        if (it->childCount() == 0 && (it->data(0, PathRole).isValid() || it->parent())) {
            const bool show = f.isEmpty() || it->text(0).contains(f, Qt::CaseInsensitive) ||
                              QFileInfo(it->data(0, PathRole).toString()).fileName().contains(f, Qt::CaseInsensitive);
            it->setHidden(!show);
            return show;
        }
        bool any = false;
        for (int c = 0; c < it->childCount(); ++c) any |= visit(it->child(c));
        it->setHidden(!f.isEmpty() && !any);
        if (!f.isEmpty() && any) it->setExpanded(true);
        return any || f.isEmpty();
    };
    for (int t = 0; t < m_tree->topLevelItemCount(); ++t) visit(m_tree->topLevelItem(t));
}
