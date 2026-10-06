#include "LayerTable.h"
#include "Widgets.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

enum Col { ColVisible, ColLock, ColName, ColEffects, ColOpacity, ColBlend, ColPlayback, ColCount };

static const char *kRowsMime = "application/x-fulskrin-layer-rows";
static constexpr int kArrowZone = 22;  // px at the left of a group's name: fold / unfold
static constexpr int kIndent = 26;     // px: members of a group

namespace {
// Rename in place: Enter validates, Esc cancels
class RenameEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    bool cancelled = false;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            cancelled = true;
            clearFocus();
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};

// Layer grid: files dropped onto a row load into it; rows dragged within the list move layers (between rows)
// or put them into a group (onto the group's row).
class LayerGrid : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;
    const std::vector<LayerTable::Row> *rows = nullptr;
    std::function<void(int, const QStringList &)> onDrop;
    std::function<void(const QList<int> &, int, int)> onMove;
    std::function<void(int)> onCollapse, onRename;

protected:
    struct Target {
        int before = -1, parent = -1;
        int into = -1;   // group row highlighted (drop into it)
        int lineY = -1;  // otherwise: insertion line
        int indent = 0;
    };

    static QStringList paths(const QMimeData *m)
    {
        QStringList out;
        for (const QUrl &u : m->urls())
            if (u.isLocalFile()) out << u.toLocalFile();
        return out;
    }

    // The fold arrow of a group, indented with it
    bool onArrow(int row, int x) const
    {
        const LayerTable::Row &r = (*rows)[size_t(row)];
        const int dx = x - columnViewportPosition(ColName) - r.depth * kIndent;
        return r.group && dx >= 0 && dx < kArrowZone;
    }

    int lastVisibleRow() const
    {
        for (int r = rowCount() - 1; r >= 0; --r)
            if (!isRowHidden(r)) return r;
        return -1;
    }

    // Where dragged rows would go: before which row, into which group (-1: top level)
    Target target(QPoint pos) const
    {
        Target t;
        const auto &R = *rows;
        const int n = int(R.size());
        auto depth = [&](int r) { return r >= 0 && r < n ? R[size_t(r)].depth : -1; };
        auto blockEnd = [&](int g) { // the row after a group and everything inside it
            int k = g + 1;
            while (k < n && depth(k) > depth(g)) ++k;
            return k;
        };
        auto parentAt = [&](int r, int level) { // the group at `level - 1` above r (r at depth >= level)
            int p = -1;
            for (int k = r - 1; k >= 0 && level > 0; --k)
                if (depth(k) == level - 1) {
                    p = k;
                    break;
                }
            return p;
        };
        const int nameX = columnViewportPosition(ColName);
        const int row = rowAt(pos.y());
        if (row < 0 || row >= n) {
            const int last = lastVisibleRow();
            t.lineY = last >= 0 ? rowViewportPosition(last) + rowHeight(last) : 0;
            return t; // at the end, top level
        }
        const int y0 = rowViewportPosition(row), h = rowHeight(row);
        const double rel = double(pos.y() - y0) / std::max(1, h);
        const LayerTable::Row &r = R[size_t(row)];
        const int d = r.depth;
        if (r.group && rel > 0.25 && rel < 0.75) { // onto a group: at the end of its contents
            const int end = blockEnd(row);
            t.into = row;
            t.parent = row;
            t.before = end < n ? end : -1;
            return t;
        }
        if (rel < (r.group ? 0.25 : 0.5)) { // above the row, at its level
            t.before = row;
            t.parent = parentAt(row, d);
            t.lineY = y0;
            t.indent = d * kIndent;
            return t;
        }
        t.lineY = y0 + h;
        if (r.group && !r.collapsed && blockEnd(row) > row + 1) { // below an open group: first in it
            t.before = row + 1;
            t.parent = row;
            t.indent = (d + 1) * kIndent;
            return t;
        }
        // Below the row (a folded group: below its contents). At the end of a group, the pointer's
        // horizontal position chooses the level: further left leaves the group.
        const int next = r.group ? blockEnd(row) : row + 1;
        const int nextDepth = next < n ? depth(next) : 0;
        int level = d;
        if (nextDepth < d) level = std::clamp((pos.x() - nameX) / kIndent, nextDepth, d);
        t.before = next < n ? next : -1;
        t.parent = parentAt(row + 1, level);
        t.indent = level * kIndent;
        return t;
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const QPoint p = e->position().toPoint();
        const int row = rowAt(p.y()), col = columnAt(p.x());
        m_pressRow = -1;
        if (row < 0) { // empty area: nothing selected (a new group is then created empty)
            clearSelection();
            setCurrentCell(-1, -1);
            return;
        }
        if (e->button() == Qt::LeftButton && col == ColName && rows && row < int(rows->size()) && onArrow(row, p.x())) {
            if (onCollapse) onCollapse(row);
            return;
        }
        QTableWidget::mousePressEvent(e);
        if (e->button() == Qt::LeftButton && col != ColVisible && col != ColLock) {
            m_pressRow = row;
            m_pressPos = p;
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_pressRow >= 0 && (e->buttons() & Qt::LeftButton) &&
            (e->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            QList<int> dragged;
            for (const QModelIndex &i : selectionModel()->selectedRows()) dragged << i.row();
            if (!dragged.contains(m_pressRow)) dragged = {m_pressRow};
            std::sort(dragged.begin(), dragged.end());
            QStringList list;
            for (int r : dragged) list << QString::number(r);
            auto *mime = new QMimeData;
            mime->setData(kRowsMime, list.join(',').toUtf8());
            auto *drag = new QDrag(this);
            drag->setMimeData(mime);
            m_pressRow = -1;
            drag->exec(Qt::MoveAction);
            return;
        }
        QTableWidget::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        m_pressRow = -1;
        QTableWidget::mouseReleaseEvent(e);
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        const QPoint p = e->position().toPoint();
        const int row = rowAt(p.y()), col = columnAt(p.x());
        if (row >= 0 && col == ColName && rows && row < int(rows->size())) {
            if (onArrow(row, p.x())) {
                if (onCollapse) onCollapse(row);
            } else if (onRename) {
                onRename(row);
            }
            return;
        }
        QTableWidget::mouseDoubleClickEvent(e);
    }

    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (e->mimeData()->hasFormat(kRowsMime) || !paths(e->mimeData()).isEmpty()) e->acceptProposedAction();
        else e->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *e) override
    {
        const QPoint p = e->position().toPoint();
        if (e->mimeData()->hasFormat(kRowsMime)) {
            m_target = target(p);
            m_internal = true;
            m_dropRow = -2;
        } else {
            m_internal = false;
            m_dropRow = rowAt(p.y());
        }
        viewport()->update();
        e->acceptProposedAction();
    }

    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_dropRow = -2;
        m_internal = false;
        viewport()->update();
    }

    void dropEvent(QDropEvent *e) override
    {
        const QPoint p = e->position().toPoint();
        const bool internal = e->mimeData()->hasFormat(kRowsMime);
        m_dropRow = -2;
        m_internal = false;
        viewport()->update();
        e->acceptProposedAction();
        if (internal) {
            QList<int> moved;
            for (const QByteArray &b : e->mimeData()->data(kRowsMime).split(',')) moved << b.toInt();
            const Target t = target(p);
            // Deferred: the drag is still running in this call
            QTimer::singleShot(0, this, [this, moved, t] {
                if (onMove) onMove(moved, t.before, t.parent);
            });
            return;
        }
        if (onDrop) onDrop(rowAt(p.y()), paths(e->mimeData()));
    }

    void paintEvent(QPaintEvent *e) override
    {
        QTableWidget::paintEvent(e);
        QPainter p(viewport());
        const QColor accent = theme::accent();
        // The viewports, a block of their own at the top
        if (rows) {
            int lastVp = -1;
            for (int r = 0; r < int(rows->size()) && (*rows)[size_t(r)].viewport; ++r) lastVp = r;
            if (lastVp >= 0 && lastVp + 1 < int(rows->size())) {
                const int y = rowViewportPosition(lastVp) + rowHeight(lastVp);
                p.setPen(QPen(QColor(120, 120, 130), 2));
                p.drawLine(0, y, viewport()->width(), y);
            }
        }
        if (m_internal) {
            if (m_target.into >= 0) {
                p.setPen(QPen(accent, 2));
                { QColor a = theme::accent(); a.setAlpha(40); p.setBrush(a); }
                const int y = rowViewportPosition(m_target.into);
                p.drawRect(QRect(1, y + 1, viewport()->width() - 3, rowHeight(m_target.into) - 3));
            } else if (m_target.lineY >= 0) {
                const int x = columnViewportPosition(ColName) + m_target.indent;
                p.setPen(QPen(accent, 3));
                p.drawLine(x, m_target.lineY, viewport()->width() - 4, m_target.lineY);
                p.setBrush(accent);
                p.drawEllipse(QPoint(x, m_target.lineY), 4, 4);
            }
            return;
        }
        if (m_dropRow < 0) return;
        p.setPen(QPen(accent, 2));
        { QColor a = theme::accent(); a.setAlpha(40); p.setBrush(a); }
        const int y = rowViewportPosition(m_dropRow);
        p.drawRect(QRect(1, y + 1, viewport()->width() - 3, rowHeight(m_dropRow) - 3));
    }

private:
    int m_dropRow = -2;
    bool m_internal = false;
    Target m_target;
    int m_pressRow = -1;
    QPoint m_pressPos;
};
} // namespace

static QToolButton *barButton(const QString &text, const QString &tip)
{
    auto *b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setMinimumSize(30, 24);
    return b;
}

LayerTable::LayerTable(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 4, 8, 4);
    v->setSpacing(4);

    auto *bar = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Layers</b>"));
    auto *add = barButton(QStringLiteral("+"), QStringLiteral("New Layer (empty: drop a media onto it)"));
    auto *group = barButton(QStringLiteral("⊞"), QStringLiteral("New Group (Ctrl+G): the selected layers go into it"));
    auto *vp = barButton(QStringLiteral("▭"), QStringLiteral("New Viewport: another window onto the composition, "
                                                             "with its own size, screen and publishing"));
    auto *remove = barButton(QStringLiteral("−"), QStringLiteral("Delete the selected layers (Del)"));
    auto *dup = barButton(QStringLiteral("⧉"), QStringLiteral("Duplicate Layer (Ctrl+D)"));
    auto *up = barButton(QStringLiteral("▲"), QStringLiteral("Move Up (Ctrl+])"));
    auto *down = barButton(QStringLiteral("▼"), QStringLiteral("Move Down (Ctrl+[)"));
    (void)title;
    bar->setSpacing(2);
    for (auto *b : {add, group, vp, remove, dup, up, down}) bar->addWidget(b);
    bar->addStretch();
    v->addLayout(bar);

    auto *grid = new LayerGrid(0, ColCount);
    grid->rows = &m_rows;
    grid->setAcceptDrops(true);
    grid->viewport()->setAcceptDrops(true);
    grid->setDragDropMode(QAbstractItemView::DragDrop);
    grid->setDragEnabled(false); // drags are started by the grid itself
    grid->onDrop = [this](int row, const QStringList &p) {
        if (!p.isEmpty()) emit filesDropped(row, p);
    };
    grid->onMove = [this](const QList<int> &rows, int before, int parent) { emit moveRequested(rows, before, parent); };
    grid->onCollapse = [this](int row) { emit collapseToggled(row); };
    grid->onRename = [this](int row) { startRename(row); };
    m_table = grid;
    m_table->setHorizontalHeaderLabels({QString(), QString(), QStringLiteral("Layer"), QStringLiteral("Fx"),
                                        QStringLiteral("Opacity"), QStringLiteral("Blend Mode"), QStringLiteral("Playback")});
    m_table->setToolTip(QStringLiteral("Top layer is drawn on top · drop a media onto a layer to load it · "
                                       "drag layers onto a group · double-click a name to rename · "
                                       "the viewports, at the top, are windows onto the composition"));
    m_table->horizontalHeaderItem(ColVisible)->setToolTip(QStringLiteral("Visible"));
    m_table->horizontalHeaderItem(ColLock)->setToolTip(QStringLiteral("Locked: no edit allowed"));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setFocusPolicy(Qt::StrongFocus);
    m_table->setIconSize(QSize(16, 16));
    auto *h = m_table->horizontalHeader();
    h->setHighlightSections(false);
    h->setSectionResizeMode(ColVisible, QHeaderView::Fixed);
    m_table->setColumnWidth(ColVisible, 30);
    h->setSectionResizeMode(ColLock, QHeaderView::Fixed);
    m_table->setColumnWidth(ColLock, 28);
    h->setSectionResizeMode(ColName, QHeaderView::Interactive);
    h->setSectionResizeMode(ColName, QHeaderView::Interactive);
    m_table->setColumnWidth(ColName, 170);
    h->setSectionResizeMode(ColEffects, QHeaderView::Fixed);
    m_table->setColumnWidth(ColEffects, 34);
    h->setSectionResizeMode(ColOpacity, QHeaderView::Fixed);
    m_table->setColumnWidth(ColOpacity, 104);
    h->setSectionResizeMode(ColBlend, QHeaderView::Fixed);
    m_table->setColumnWidth(ColBlend, 66);
    h->setSectionResizeMode(ColPlayback, QHeaderView::Interactive);
    m_table->setColumnWidth(ColPlayback, 150);
    h->setStretchLastSection(true);
    v->addWidget(m_table, 1);

    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int prev, int) {
        if (!m_updating && row != prev) emit currentRowChanged(row);
    });
    // The opacity value of a selected row is drawn on the selection color: it takes the color that reads there
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &LayerTable::restyleSelection);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_updating || it->column() != ColVisible) return;
        emit visibilityToggled(it->row(), it->checkState() == Qt::Checked);
    });
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (col == ColLock) emit lockToggled(row);
    });
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = m_table->rowAt(pos.y());
        // Right-clicking outside the selection acts on the row under the cursor, as everywhere else
        if (row >= 0 && !selectedRows().contains(row)) {
            setCurrentRow(row);
            emit currentRowChanged(row);
        }
        emit contextMenuRequested(row, m_table->viewport()->mapToGlobal(pos));
    });
    auto *f2 = new QShortcut(QKeySequence(Qt::Key_F2), m_table, nullptr, nullptr, Qt::WidgetShortcut);
    connect(f2, &QShortcut::activated, this, [this] { startRename(currentRow()); });
    connect(add, &QToolButton::clicked, this, &LayerTable::addClicked);
    connect(group, &QToolButton::clicked, this, &LayerTable::groupClicked);
    connect(vp, &QToolButton::clicked, this, &LayerTable::viewportClicked);
    connect(remove, &QToolButton::clicked, this, &LayerTable::removeClicked);
    connect(dup, &QToolButton::clicked, this, &LayerTable::duplicateClicked);
    connect(up, &QToolButton::clicked, this, [this] { emit moveClicked(-1); });
    connect(down, &QToolButton::clicked, this, [this] { emit moveClicked(+1); });
}

QWidget *LayerTable::table() const { return m_table; }

int LayerTable::currentRow() const { return m_table->currentRow(); }

QList<int> LayerTable::selectedRows() const
{
    QList<int> out;
    for (const QModelIndex &i : m_table->selectionModel()->selectedRows()) out << i.row();
    std::sort(out.begin(), out.end());
    return out;
}

void LayerTable::setCurrentRow(int row)
{
    m_updating = true;
    if (row < 0) {
        m_table->setCurrentCell(-1, -1);
        m_table->clearSelection();
    } else {
        // Explicit command: a held Ctrl (shortcut) must not toggle the row
        m_table->selectionModel()->setCurrentIndex(m_table->model()->index(row, ColName),
                                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_updating = false;
}

void LayerTable::startRename(int row)
{
    if (row < 0 || row >= int(m_rows.size())) return;
    const Row &r = m_rows[size_t(row)];
    if (r.locked || r.lockedByGroup) return;
    auto *edit = new RenameEdit(r.name);
    edit->selectAll();
    m_table->setCellWidget(row, ColName, edit);
    edit->setFocus();
    QPointer<RenameEdit> guard(edit);
    connect(edit, &QLineEdit::editingFinished, this, [this, guard, row] {
        if (!guard || guard->property("done").toBool()) return;
        guard->setProperty("done", true);
        const QString name = guard->text().trimmed();
        const bool ok = !guard->cancelled && !name.isEmpty() && row < int(m_rows.size()) && name != m_rows[size_t(row)].name;
        QTimer::singleShot(0, this, [this, guard, row] {
            if (guard && m_table->cellWidget(row, ColName) == guard) m_table->removeCellWidget(row, ColName);
            m_table->setFocus();
        });
        if (ok) emit renamed(row, name);
    });
}

void LayerTable::updateRow(int r, const Row &row)
{
    auto text = [&](int col, const QString &t, const QString &tip = {}) {
        QTableWidgetItem *it = m_table->item(r, col);
        if (!it) {
            it = new QTableWidgetItem;
            m_table->setItem(r, col, it);
        }
        if (it->text() != t) it->setText(t);
        it->setToolTip(tip.isEmpty() ? t : tip);
        return it;
    };
    QTableWidgetItem *vis = m_table->item(r, ColVisible);
    if (!vis) {
        vis = new QTableWidgetItem;
        vis->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        m_table->setItem(r, ColVisible, vis);
    }
    vis->setCheckState(row.visible ? Qt::Checked : Qt::Unchecked);
    vis->setToolTip(QStringLiteral("Visible"));

    QTableWidgetItem *lock = m_table->item(r, ColLock);
    if (!lock) {
        lock = new QTableWidgetItem;
        lock->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(r, ColLock, lock);
    }
    const int lockState = row.locked ? 2 : row.lockedByGroup ? 1 : 0;
    if (lock->data(Qt::UserRole).toInt() != lockState || lock->icon().isNull()) {
        lock->setIcon(padlockIcon(row.locked, row.lockedByGroup));
        lock->setData(Qt::UserRole, lockState);
    }
    lock->setToolTip(row.locked ? QStringLiteral("Locked — click to unlock")
                     : row.lockedByGroup ? QStringLiteral("Locked by its group")
                                         : QStringLiteral("Click to lock (no edit allowed)"));

    // Indented by depth (about kIndent px per level); a group starts with its fold arrow
    const QString prefix = QStringLiteral("        ").repeated(row.depth) +
                           (row.group ? (row.collapsed ? QStringLiteral("▸  ") : QStringLiteral("▾  ")) : QString());
    QTableWidgetItem *name = text(ColName, prefix + row.tag + QStringLiteral("  ") + row.name,
                                  row.name + (row.source.isEmpty() ? QString() : QStringLiteral("\n") + row.source));
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
    name->setForeground(row.error      ? QColor(255, 110, 95)
                        : row.viewport ? QColor(140, 200, 255)
                        : row.group    ? QColor(255, 200, 140)
                                       : QColor(230, 230, 233));
    // Effects: their number, the names on hover
    QTableWidgetItem *fx = text(ColEffects, row.effectCount ? QString::number(row.effectCount) : QStringLiteral("—"),
                                row.effectCount ? (row.effectsOn ? QString() : QStringLiteral("Effects off\n")) +
                                                      row.effects.split(QStringLiteral(" › ")).join('\n')
                                                : QStringLiteral("No effects"));
    fx->setTextAlignment(Qt::AlignCenter);
    fx->setForeground(row.effectsOn ? QColor(255, 200, 140) : QColor(110, 110, 115));
    text(ColBlend, row.noPicture || row.viewport ? QStringLiteral("—") : row.blend);
    text(ColPlayback, row.playback)->setFont(QFont(QStringLiteral("monospace")));
    const QBrush bg = row.viewport ? QBrush(QColor(38, 48, 60)) : row.group ? QBrush(QColor(50, 50, 58)) : QBrush();
    for (int c = 0; c < ColCount; ++c)
        if (QTableWidgetItem *it = m_table->item(r, c)) it->setBackground(bg);
    m_table->setRowHidden(r, row.hidden);

    // Opacity: slider right in the row
    auto *cell = m_table->cellWidget(r, ColOpacity);
    QSlider *slider = cell ? cell->findChild<QSlider *>() : nullptr;
    QLabel *label = cell ? cell->findChild<QLabel *>() : nullptr;
    if (!cell) {
        cell = new QWidget;
        auto *h = new QHBoxLayout(cell);
        h->setContentsMargins(4, 0, 6, 0);
        slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setFocusPolicy(Qt::NoFocus);
        label = new QLabel;
        label->setMinimumWidth(38);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(slider, 1);
        h->addWidget(label);
        m_table->setCellWidget(r, ColOpacity, cell);
        connect(slider, &QSlider::valueChanged, this, [this, slider, label](int v) {
            label->setText(QStringLiteral("%1%").arg(v));
            if (m_updating) return;
            // Find the slider's row (rows may have changed since it was created)
            for (int k = 0; k < m_table->rowCount(); ++k) {
                QWidget *c = m_table->cellWidget(k, ColOpacity);
                if (c && c->isAncestorOf(slider)) {
                    emit opacityEdited(k, v / 100.0);
                    break;
                }
            }
        });
    }
    slider->setVisible(!row.noPicture);
    slider->setEnabled(!row.locked && !row.lockedByGroup);
    if (row.noPicture) {
        label->setText(QStringLiteral("—"));
        return;
    }
    const int pct = int(std::lround(row.opacity * 100));
    if (!slider->isSliderDown() && slider->value() != pct) {
        QSignalBlocker b(slider);
        slider->setValue(pct);
    }
    label->setText(QStringLiteral("%1%").arg(slider->value()));
}

void LayerTable::setRows(const std::vector<Row> &rows)
{
    m_updating = true;
    const int keep = m_table->currentRow();
    m_rows = rows;
    if (m_table->rowCount() != int(rows.size())) {
        m_table->clearContents();
        m_table->setRowCount(int(rows.size()));
        if (keep >= 0 && keep < int(rows.size()))
            m_table->selectionModel()->setCurrentIndex(m_table->model()->index(keep, ColName),
                                                       QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    for (int r = 0; r < int(rows.size()); ++r) updateRow(r, rows[size_t(r)]);
    m_updating = false;
    restyleSelection();
}

void LayerTable::restyleSelection()
{
    const QList<int> sel = selectedRows();
    const QString on = QStringLiteral("color:%1;").arg(theme::onAccent().name());
    for (int r = 0; r < m_table->rowCount(); ++r)
        if (QWidget *c = m_table->cellWidget(r, ColOpacity))
            if (auto *label = c->findChild<QLabel *>()) {
                const QString want = sel.contains(r) ? on : QString();
                if (label->styleSheet() != want) label->setStyleSheet(want);
            }
}
