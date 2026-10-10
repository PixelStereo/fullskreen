#include "SnapshotPanel.h"
#include "Commands.h"
#include "Engine.h"
#include "Widgets.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

static const QSize kThumb(160, 90);

// Roles of the trees
enum { IdRole = Qt::UserRole, FieldRole = Qt::UserRole + 1, KeyRole = Qt::UserRole + 3, SnapshotRole = Qt::UserRole + 6 };
// Columns of the tree of what a snapshot holds
enum Column { ColName, ColValue, ColTime };

// How a value gets there at the recall: the time the snapshot gives it, or gives a group it is in (`from`: the key that
// gives it; none: it follows the snapshot's fade)
static QJsonValue timingValue(const QJsonObject &layer, const QString &key, QString *from = nullptr)
{
    const QJsonObject timing = layer.value("timing").toObject();
    const QString k = Engine::timingKeyOf(timing, key);
    if (from) *from = k;
    return k.isEmpty() ? QJsonValue() : timing.value(k);
}

static QString timeText(const QJsonValue &v, bool inherited = false)
{
    if (!v.isDouble()) return QStringLiteral("Follow");
    const QString t = v.toDouble() <= 0 ? QStringLiteral("Cut") : QStringLiteral("%1 s").arg(v.toDouble(), 0, 'g', 4);
    return inherited ? QStringLiteral("↑ ") + t : t;
}

static QString valueText(const MemField &f, const QJsonValue &value)
{
    switch (f.kind) {
    case MemField::Bool: return value.toBool() ? QStringLiteral("On") : QStringLiteral("Off");
    case MemField::Number: return QString::number(value.toDouble() * f.scale, 'f', f.decimals) + f.suffix;
    case MemField::Choice: {
        const int k = f.keys.indexOf(value.toString());
        return k >= 0 ? f.labels.value(k) : value.toString();
    }
    default: return value.toString();
    }
}

// The object a field's row refers to: a layer of the snapshot, or (-1) its composition
static QJsonObject rowObject(const Engine::Snapshot &m, int row)
{
    if (row < 0) return m.composition;
    return row < m.layers.size() ? m.layers.at(row).toObject() : QJsonObject();
}
static bool setRowObject(Engine::Snapshot &m, int row, const QJsonObject &o)
{
    if (row < 0) {
        m.composition = o;
        return true;
    }
    if (row >= m.layers.size()) return false;
    m.layers[row] = o;
    return true;
}

// Value at `path` inside a JSON tree; a numeric component addresses an array.
static QJsonValue jsonAt(const QJsonValue &root, const QStringList &path, int from = 0)
{
    if (from >= path.size()) return root;
    bool isIndex = false;
    const int idx = path[from].toInt(&isIndex);
    if (isIndex && root.isArray()) return jsonAt(root.toArray().at(idx), path, from + 1);
    return jsonAt(root.toObject().value(path[from]), path, from + 1);
}

// Copy of the tree with the value at `path` replaced
static QJsonValue jsonWith(const QJsonValue &root, const QStringList &path, const QJsonValue &v, int from = 0)
{
    if (from >= path.size()) return v;
    bool isIndex = false;
    const int idx = path[from].toInt(&isIndex);
    if (isIndex && root.isArray()) {
        QJsonArray a = root.toArray();
        if (idx < 0 || idx >= a.size()) return root;
        a[idx] = jsonWith(a.at(idx), path, v, from + 1);
        return a;
    }
    QJsonObject o = root.toObject();
    o[path[from]] = jsonWith(o.value(path[from]), path, v, from + 1);
    return o;
}

static QPixmap thumbnail(const QImage &thumb)
{
    QPixmap pm(kThumb);
    pm.fill(Qt::black);
    QPainter p(&pm);
    if (!thumb.isNull()) {
        const QImage s = thumb.scaled(kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage(QPoint((kThumb.width() - s.width()) / 2, (kThumb.height() - s.height()) / 2), s);
    }
    return pm;
}

namespace {
// The list of the snapshots; a snapshot dragged from it carries its id (onto a step of a sequence)
class SnapshotList : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(kSnapshotMime)}; }
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        if (items.isEmpty()) return nullptr;
        auto *m = new QMimeData;
        m->setData(kSnapshotMime, QByteArray::number(items.first()->data(0, SnapshotRole).toULongLong()));
        m->setText(items.first()->text(1));
        return m;
    }
};

QToolButton *barButton(const QString &text, const QString &tip)
{
    auto *b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setMinimumSize(30, 24);
    return b;
}
} // namespace

// The list of the snapshots: the one recalled last is marked apart from the selection — a green stripe on its left
// and ▶ before its number — and its fade fills the row's bottom while it runs.
namespace {
const QColor kLiveSnapshot(76, 217, 100);

class SnapshotRowDelegate : public QStyledItemDelegate
{
public:
    SnapshotRowDelegate(Engine *e, QObject *parent) : QStyledItemDelegate(parent), m_engine(e) {}
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(p, opt, index);
        const Engine::RecallProgress r = m_engine->recallProgress();
        if (!r.snapshot || index.siblingAtColumn(0).data(SnapshotRole).toULongLong() != r.snapshot) return;
        p->save();
        if (index.column() == 0) p->fillRect(QRectF(opt.rect.left(), opt.rect.top(), 4, opt.rect.height()), kLiveSnapshot);
        if (r.running()) { // the whole row's width is the snapshot's time
            const QAbstractItemView *view = qobject_cast<const QAbstractItemView *>(opt.widget);
            const int total = view ? view->viewport()->width() : opt.rect.width();
            const double fill = r.fraction() * total;
            const QRectF bar(opt.rect.left(), opt.rect.bottom() - 2, opt.rect.width(), 3);
            const QRectF done = bar.intersected(QRectF(0, bar.top(), fill, bar.height()));
            if (!done.isEmpty()) p->fillRect(done, kLiveSnapshot);
        }
        p->restore();
    }

private:
    Engine *m_engine;
};
} // namespace

SnapshotPanel::SnapshotPanel(Engine *engine, QUndoStack *undo, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo)
{
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(8, 4, 8, 4);
    auto *split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    h->addWidget(split);

    // --- The list: number, name, fade
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    auto *bar = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Snapshots</b>"));
    bar->addWidget(title);
    bar->addStretch();
    m_store = new QPushButton(QStringLiteral("+"));
    m_store->setToolTip(QStringLiteral("Store the current state of the layers in a new snapshot"));
    m_store->setFixedWidth(34);
    m_go = new QPushButton(QStringLiteral("GO"));
    m_go->setStyleSheet("QPushButton { font-weight:bold; background:#2f6b3a; color:white; padding:3px 14px; }");
    m_go->setToolTip(QStringLiteral("Recall the selected snapshot (double-click or Enter in the list)"));
    bar->addWidget(m_store);
    bar->addWidget(m_go);
    lv->addLayout(bar);
    m_list = new SnapshotList;
    m_list->setColumnCount(3);
    m_list->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Name"), QStringLiteral("Fade")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->setColumnWidth(0, 44);
    m_list->setColumnWidth(2, 58);
    m_list->setToolTip(QStringLiteral("Double-click or Enter: recall · drag onto a step of a sequence\n"
                                      "Green stripe and ▶: the snapshot recalled last; the green line under it: its fade"));
    m_list->setItemDelegate(new SnapshotRowDelegate(m_engine, m_list));
    { // the fade running moves the line under its snapshot
        auto *t = new QTimer(this);
        t->setInterval(33);
        connect(t, &QTimer::timeout, this, [this] {
            const Engine::RecallProgress r = m_engine->recallProgress();
            if (r.running() || m_wasRunning) m_list->viewport()->update();
            m_wasRunning = r.running();
        });
        t->start();
    }
    lv->addWidget(m_list, 1);
    left->setMinimumWidth(220);
    split->addWidget(left);

    // --- What the selected snapshot holds
    m_inspector = new QWidget;
    auto *iv = new QVBoxLayout(m_inspector);
    iv->setContentsMargins(6, 0, 0, 0);
    auto *head = new QHBoxLayout;
    m_thumb = new QLabel;
    m_thumb->setFixedSize(kThumb);
    m_thumb->setStyleSheet("background:#000;");
    auto *fields = new QVBoxLayout;
    m_title = new QLabel;
    m_name = new QLineEdit;
    m_name->setPlaceholderText(QStringLiteral("Name"));
    m_fade = new NumberBox;
    m_fade->setRange(0, 600);
    m_fade->setDecimals(1);
    m_fade->setSingleStep(0.5);
    m_fade->setSuffix(QStringLiteral(" s"));
    m_fade->setKeyboardTracking(false);
    m_fade->setToolTip(QStringLiteral("Fade: the values set to Follow get to the snapshot's in this time; sources and "
                                      "FX chains change at once (a new source comes in with its transition)"));
    auto *fadeRow = new QHBoxLayout;
    fadeRow->addWidget(new ResetLabel(QStringLiteral("Fade"), [this] { m_fade->setValue(1.0); }));
    fadeRow->addWidget(m_fade, 1);
    fields->addWidget(m_title);
    fields->addWidget(m_name);
    fields->addLayout(fadeRow);
    auto *buttons = new QHBoxLayout;
    m_update = new QPushButton(QStringLiteral("Update"));
    m_update->setToolTip(QStringLiteral("Store the current state of the layers into this snapshot"));
    m_delete = new QPushButton(QStringLiteral("Delete"));
    buttons->addWidget(m_update);
    buttons->addWidget(m_delete);
    fields->addLayout(buttons);
    fields->addStretch();
    head->addWidget(m_thumb);
    head->addLayout(fields, 1);
    iv->addLayout(head);
    m_layers = new QTreeWidget;
    m_layers->setColumnCount(3);
    m_layers->setHeaderLabels({QStringLiteral("Layer / value"), QStringLiteral("Value"), QStringLiteral("Time")});
    m_layers->setRootIsDecorated(true);
    m_layers->setIndentation(12);
    m_layers->setUniformRowHeights(true);
    m_layers->setAlternatingRowColors(true);
    m_layers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_layers->header()->setStretchLastSection(false);
    m_layers->setColumnWidth(ColValue, 96);
    m_layers->setColumnWidth(ColTime, 64);
    m_layers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_layers->setToolTip(QStringLiteral("Unchecked layers are left alone when the snapshot is recalled; layers the snapshot "
                                        "does not know are hidden.\nClick a value to see it, change it and choose how it "
                                        "gets there (right)."));
    iv->addWidget(m_layers, 1);
    m_inspector->setMinimumWidth(340);
    split->addWidget(m_inspector);

    // --- The selected value
    m_detail = new QWidget;
    m_detailLayout = new QVBoxLayout(m_detail);
    m_detailLayout->setContentsMargins(8, 0, 0, 0);
    m_detail->setMinimumWidth(240);
    split->addWidget(m_detail);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 1);
    split->setSizes({260, 520, 300});

    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] {
        if (!m_filling) showInspector(selected());
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        recall(m_list->indexOfTopLevelItem(it));
    });
    auto *enter = new QShortcut(QKeySequence(Qt::Key_Return), m_list, nullptr, nullptr, Qt::WidgetShortcut);
    connect(enter, &QShortcut::activated, this, [this] { recall(selected()); });
    auto *del = new QShortcut(QKeySequence::Delete, m_list, nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, [this] { removeSnapshot(selected()); });
    auto *bs = new QShortcut(QKeySequence(Qt::Key_Backspace), m_list, nullptr, nullptr, Qt::WidgetShortcut);
    connect(bs, &QShortcut::activated, this, [this] { removeSnapshot(selected()); });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *it = m_list->itemAt(pos);
        const int i = it ? m_list->indexOfTopLevelItem(it) : -1;
        QMenu menu;
        menu.addAction(QStringLiteral("Store Current State"), this, &SnapshotPanel::store);
        if (i >= 0) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Recall"), this, [this, i] { recall(i); });
            menu.addAction(QStringLiteral("Update with Current State"), this, [this, i] { updateSnapshot(i); });
            menu.addAction(QStringLiteral("Rename"), this, [this] {
                m_name->setFocus();
                m_name->selectAll();
            });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete"), this, [this, i] { removeSnapshot(i); });
        }
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_store, &QPushButton::clicked, this, &SnapshotPanel::store);
    connect(m_go, &QPushButton::clicked, this, [this] { recall(selected()); });
    connect(m_update, &QPushButton::clicked, this, [this] { updateSnapshot(selected()); });
    connect(m_delete, &QPushButton::clicked, this, [this] { removeSnapshot(selected()); });
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        const int i = selected();
        if (i < 0) return;
        Engine::Snapshot m = m_engine->snapshot(i);
        if (m.name == m_name->text()) return;
        m.name = m_name->text();
        m_engine->setSnapshot(i, m);
        emit edited();
    });
    connect(m_fade, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        const int i = selected();
        if (i < 0 || m_filling) return;
        Engine::Snapshot m = m_engine->snapshot(i);
        m.fade = v;
        m_applying = true;
        m_engine->setSnapshot(i, m);
        m_applying = false;
        if (QTreeWidgetItem *it = m_list->topLevelItem(i)) it->setText(2, QStringLiteral("%1 s").arg(v, 0, 'f', 1));
        emit edited();
    });
    connect(m_layers, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
        if (m_filling || col != 0) return;
        if (it->data(0, KeyRole).toString() == QLatin1String("composition")) {
            setCompositionIncluded(selected(), it->checkState(0) == Qt::Checked);
            return;
        }
        if (!it->data(0, IdRole).isValid()) return;
        setInclusion(selected(), it->data(0, IdRole).toULongLong(), it->checkState(0) == Qt::Checked);
    });
    connect(m_layers, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (m_filling) return;
        m_selectedKey = it ? it->data(0, KeyRole).toString() : QString();
        showDetail(it);
    });
    // Unfolded nodes are kept from one refresh to the next
    auto remember = [this](QTreeWidgetItem *it, bool on) {
        const QString key = it->data(0, KeyRole).toString();
        if (key.isEmpty() || m_filling) return;
        if (on) m_expanded.insert(key);
        else m_expanded.remove(key);
    };
    connect(m_layers, &QTreeWidget::itemExpanded, this, [remember](QTreeWidgetItem *it) { remember(it, true); });
    connect(m_layers, &QTreeWidget::itemCollapsed, this, [remember](QTreeWidgetItem *it) { remember(it, false); });
    connect(m_engine, &Engine::snapshotsChanged, this, &SnapshotPanel::refresh);
    connect(m_engine, &Engine::snapshotRecalled, this, [this](int i) {
        m_active = i;
        refresh();
    });
    refresh();
}

int SnapshotPanel::selected() const
{
    QTreeWidgetItem *it = m_list->currentItem();
    return it ? m_list->indexOfTopLevelItem(it) : -1;
}

void SnapshotPanel::refresh()
{
    if (m_applying) return; // our own edit: the panel already shows it
    const int keep = selected();
    m_filling = true;
    m_list->clear();
    const int n = m_engine->snapshotCount();
    if (m_active >= n) m_active = -1;
    for (int i = 0; i < n; ++i) {
        const Engine::Snapshot m = m_engine->snapshot(i);
        auto *it = new QTreeWidgetItem(m_list, {QString::number(i + 1), m.name, QStringLiteral("%1 s").arg(m.fade, 0, 'f', 1)});
        it->setData(0, SnapshotRole, m.id);
        it->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
        it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        if (i == m_active) { // last recalled: ▶, bold, green — not the accent, which is the selection's
            it->setText(0, QStringLiteral("▶ %1").arg(i + 1));
            QFont f = it->font(1);
            f.setBold(true);
            for (int c = 0; c < 3; ++c) {
                it->setFont(c, f);
                it->setForeground(c, kLiveSnapshot);
            }
        }
    }
    if (keep >= 0 && keep < n) m_list->setCurrentItem(m_list->topLevelItem(keep));
    m_filling = false;
    showInspector(selected());
}

void SnapshotPanel::showInspector(int i)
{
    m_filling = true;
    m_fields.clear();
    const bool valid = i >= 0 && i < m_engine->snapshotCount();
    for (QWidget *w : std::initializer_list<QWidget *>{m_name, m_fade, m_layers, m_go, m_update, m_delete}) w->setEnabled(valid);
    m_layers->clear();
    if (!valid) {
        m_title->setText(QStringLiteral("<span style='color:#888'>No snapshot selected.<br>+ stores the current state.</span>"));
        m_thumb->clear();
        m_name->clear();
        m_filling = false;
        showDetail(nullptr);
        return;
    }
    const Engine::Snapshot m = m_engine->snapshot(i);
    m_title->setText(QStringLiteral("<b>Snapshot %1</b>%2").arg(i + 1).arg(i == m_active ? QStringLiteral(" <span style='color:#4cd964'>▶ recalled last</span>") : QString()));
    m_thumb->setPixmap(thumbnail(m.thumbnail));
    m_name->setText(m.name);
    m_fade->setValue(m.fade);
    QTreeWidgetItem *current = nullptr;
    fillComposition(m.composition);
    for (int row = 0; row < m.layers.size(); ++row) {
        const QJsonObject o = m.layers[row].toObject();
        auto *it = new QTreeWidgetItem(m_layers, {(o.contains("parent") ? QStringLiteral("    ") : QString()) + o.value("name").toString(),
                                                  (o.value("enable").toBool(true) ? QStringLiteral("✓ ") : QStringLiteral("— ")) +
                                                      QStringLiteral("%1%").arg(std::lround(o.value("params").toObject().value("opacity").toDouble(1) * 100))});
        it->setData(0, IdRole, o.value("id").toString().toULongLong());
        it->setData(0, KeyRole, QStringLiteral("L") + o.value("id").toString());
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        it->setCheckState(0, o.value("included").toBool(true) ? Qt::Checked : Qt::Unchecked);
        if (m_engine->indexOfId(it->data(0, IdRole).toULongLong()) < 0) {
            it->setForeground(0, QColor(255, 180, 90));
            it->setToolTip(0, QStringLiteral("Not in the composition any more: recreated by the recall"));
        }
        fillLayer(it, row, o);
        it->setExpanded(m_expanded.contains(it->data(0, KeyRole).toString()));
    }
    // The value selected before the refresh, if it is still there
    if (!m_selectedKey.isEmpty()) {
        std::function<QTreeWidgetItem *(QTreeWidgetItem *)> find = [&](QTreeWidgetItem *p) -> QTreeWidgetItem * {
            for (int k = 0; k < p->childCount(); ++k) {
                QTreeWidgetItem *c = p->child(k);
                if (c->data(0, KeyRole).toString() == m_selectedKey) return c;
                if (QTreeWidgetItem *d = find(c)) return d;
            }
            return nullptr;
        };
        current = find(m_layers->invisibleRootItem());
        if (current) m_layers->setCurrentItem(current);
    }
    m_filling = false;
    showDetail(current);
}

QTreeWidgetItem *SnapshotPanel::addField(QTreeWidgetItem *parent, const MemField &f, const QJsonValue &value)
{
    auto *it = new QTreeWidgetItem(parent, {f.label, valueText(f, value)});
    it->setForeground(ColName, QColor(165, 165, 172));
    it->setData(0, FieldRole, int(m_fields.size()));
    it->setData(0, KeyRole, QStringLiteral("%1/%2").arg(f.row).arg(f.path.join('/')));
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (f.kind == MemField::Info) it->setForeground(ColValue, QColor(140, 140, 146));
    m_fields.push_back(f);
    if (!f.timeKey.isEmpty()) showTime(it, f);
    return it;
}

// The time of a value in its row: its own (bright), its group's (↑), or the snapshot's fade (Follow)
void SnapshotPanel::showTime(QTreeWidgetItem *it, const MemField &f)
{
    const Engine::Snapshot m = m_engine->snapshot(selected());
    QString from;
    const QJsonValue t = timingValue(rowObject(m, f.row), f.timeKey, &from);
    it->setText(ColTime, timeText(t, !from.isEmpty() && from != f.timeKey));
    it->setForeground(ColTime, from == f.timeKey ? QColor(230, 230, 233) : QColor(130, 130, 136));
}

// Values and times shown in the tree, after an edit of the selected snapshot (the tree is not rebuilt)
void SnapshotPanel::refreshRows()
{
    const int i = selected();
    if (i < 0) return;
    const Engine::Snapshot m = m_engine->snapshot(i);
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *p) {
        for (int k = 0; k < p->childCount(); ++k) {
            QTreeWidgetItem *c = p->child(k);
            const QVariant fi = c->data(0, FieldRole);
            if (fi.isValid()) {
                const MemField &f = m_fields[size_t(fi.toInt())];
                const QJsonObject o = rowObject(m, f.row);
                c->setText(ColValue, f.kind == MemField::Info ? c->text(ColValue) : valueText(f, jsonAt(o, f.path)));
                if (!f.timeKey.isEmpty()) showTime(c, f);
            }
            walk(c);
        }
    };
    walk(m_layers->invisibleRootItem());
}

// The selected value: what the snapshot holds (editable), and how it gets there at the recall
void SnapshotPanel::showDetail(QTreeWidgetItem *it)
{
    while (QLayoutItem *item = m_detailLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        else if (QLayout *l = item->layout()) {
            while (QLayoutItem *x = l->takeAt(0)) {
                if (x->widget()) x->widget()->deleteLater();
                delete x;
            }
        }
        delete item;
    }
    auto hint = [this](const QString &t) {
        auto *l = new QLabel(t);
        l->setWordWrap(true);
        l->setStyleSheet("color:#888; font-size:11px;");
        m_detailLayout->addWidget(l);
    };
    const int mi = selected();
    const QVariant fi = it ? it->data(0, FieldRole) : QVariant();
    if (mi < 0 || !fi.isValid()) {
        if (it && it->data(0, IdRole).isValid()) {
            auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(it->text(0).trimmed().toHtmlEscaped()));
            m_detailLayout->addWidget(title);
            hint(it->checkState(0) == Qt::Checked
                     ? QStringLiteral("In the snapshot: the recall takes this layer to the values below.")
                     : QStringLiteral("Left out: the recall leaves this layer as it is."));
        } else {
            hint(QStringLiteral("Click a value in the snapshot to see it, change it, and choose how it gets there when the "
                                "snapshot is recalled: CUT (at once), FOLLOW (the snapshot's fade) or a time of its own."));
        }
        m_detailLayout->addStretch();
        return;
    }
    const MemField f = m_fields[size_t(fi.toInt())];
    const Engine::Snapshot m = m_engine->snapshot(mi);
    const QJsonObject o = rowObject(m, f.row);
    const QJsonValue value = jsonAt(o, f.path);

    // Where it is: layer › section › value
    QStringList where{f.label};
    for (QTreeWidgetItem *p = it->parent(); p; p = p->parent()) where.prepend(p->text(0).trimmed());
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(where.join(QStringLiteral(" › ")).toHtmlEscaped()));
    title->setWordWrap(true);
    m_detailLayout->addWidget(title);

    // The value
    switch (f.kind) {
    case MemField::Number: {
        const bool bounded = f.max - f.min < 1e5;
        if (bounded) {
            auto *s = new SliderField;
            s->setRange(f.min * f.scale, f.max * f.scale);
            s->setDecimals(f.decimals);
            s->setSuffix(f.suffix);
            s->setSingleStep(f.step * f.scale);
            s->setValue(value.toDouble() * f.scale);
            m_detailLayout->addWidget(s);
            connect(s, &SliderField::editingFinished, this,
                    [this, f](double v) { applyField(f, QJsonValue(v / (f.scale != 0 ? f.scale : 1))); });
        } else {
            auto *b = new NumberBox;
            b->setRange(f.min * f.scale, f.max * f.scale);
            b->setDecimals(f.decimals);
            b->setSingleStep(f.step * f.scale);
            b->setSuffix(f.suffix);
            b->setKeyboardTracking(false);
            b->setValue(value.toDouble() * f.scale);
            m_detailLayout->addWidget(b);
            connect(b, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [this, f](double v) { applyField(f, QJsonValue(v / (f.scale != 0 ? f.scale : 1))); });
        }
        break;
    }
    case MemField::Bool: {
        auto *c = new FlagBox(f.label);
        c->setChecked(value.toBool());
        m_detailLayout->addWidget(c);
        connect(c, &QCheckBox::toggled, this, [this, f](bool on) { applyField(f, QJsonValue(on)); });
        break;
    }
    case MemField::Choice: {
        auto *c = new QComboBox;
        for (int k = 0; k < f.keys.size(); ++k) c->addItem(f.labels.value(k, f.keys[k]), f.keys[k]);
        c->setCurrentIndex(std::max(0, c->findData(value.toString())));
        m_detailLayout->addWidget(c);
        connect(c, qOverload<int>(&QComboBox::activated), this,
                [this, f, c](int k) { applyField(f, QJsonValue(c->itemData(k).toString())); });
        break;
    }
    default: {
        auto *l = new QLabel(it->text(ColValue));
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setToolTip(it->toolTip(ColValue));
        m_detailLayout->addWidget(l);
        break;
    }
    }
    hint(QStringLiteral("The snapshot changes, the composition does not, until the snapshot is recalled."));

    // How it gets there
    if (!f.timeKey.isEmpty()) {
        const QJsonObject timing = o.value("timing").toObject(), easing = o.value("easing").toObject();
        const QJsonValue t = timing.value(f.timeKey); // its own
        QString groupKey;
        const QJsonValue inherited = timingValue(o, f.timeKey, &groupKey);
        auto *box = new QWidget;
        auto *bv = new QVBoxLayout(box);
        bv->setContentsMargins(0, 10, 0, 0);
        bv->addWidget(new QLabel(f.timeKey == "file"           ? QStringLiteral("<b>Transition of the source</b>")
                                 : f.timeKey == "text/content" ? QStringLiteral("<b>Typing (typewriter)</b>")
                                                               : QStringLiteral("<b>Transition</b>")));
        auto *row = new QHBoxLayout;
        auto *group = new QButtonGroup(box);
        const char *names[] = {"CUT", "FOLLOW", "TIME"};
        const int mode = !t.isDouble() ? 1 : t.toDouble() <= 0 ? 0 : 2;
        for (int k = 0; k < 3; ++k) {
            auto *b = new QPushButton(QString::fromLatin1(names[k]));
            b->setCheckable(true);
            b->setChecked(k == mode);
            b->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:%2; font-weight:bold; }")
                                 .arg(theme::css(), theme::onAccent().name()));
            group->addButton(b, k);
            row->addWidget(b);
        }
        auto *secs = new NumberBox;
        secs->setRange(0.1, 600);
        secs->setDecimals(1);
        secs->setSingleStep(0.5);
        secs->setSuffix(QStringLiteral(" s"));
        secs->setKeyboardTracking(false);
        secs->setValue(mode == 2 ? t.toDouble() : std::max(0.1, m.fade > 0 ? m.fade : 1.0));
        secs->setEnabled(mode == 2);
        row->addWidget(secs);
        bv->addLayout(row);
        // FOLLOW: the time of the group it is in when it has one, else the snapshot's fade
        const QString follows = !groupKey.isEmpty() && groupKey != f.timeKey
                                    ? QStringLiteral("%1's time (%2)").arg(groupKey, timeText(inherited))
                                    : QStringLiteral("the snapshot's fade (%1 s)").arg(m.fade, 0, 'f', 1);
        auto *note = new QLabel(QStringLiteral("CUT: at once · FOLLOW: %1 · TIME: its own time%2")
                                    .arg(follows, f.kind == MemField::Info && f.timeKey != "file" && f.timeKey != "text/content" &&
                                                          f.timeKey != "mesh" && f.timeKey != "viewports"
                                                      ? QStringLiteral(", for all its values that have none")
                                                      : QString()));
        note->setWordWrap(true);
        note->setStyleSheet("color:#888; font-size:11px;");
        bv->addWidget(note);
        m_detailLayout->addWidget(box);
        const int row0 = f.row;
        const QString key = f.timeKey;
        connect(group, &QButtonGroup::idClicked, this, [this, row0, key, secs](int id) {
            secs->setEnabled(id == 2);
            applyTime(row0, key, id == 0 ? 0.0 : id == 1 ? -1.0 : secs->value());
        });
        connect(secs, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, row0, key](double v) { applyTime(row0, key, v); });

        // Its easing: its own, its group's, or the default one
        auto *curveBox = new QWidget;
        auto *curveLay = new QHBoxLayout(curveBox);
        curveLay->setContentsMargins(0, 0, 0, 0);
        auto *curveCombo = new QComboBox;
        const QStringList curves = Engine::easingKeys(), curveNames = Engine::easingNames();
        for (int k = 0; k < curves.size(); ++k) curveCombo->addItem(curveNames[k], curves[k]);
        const QString easingKey = Engine::timingKeyOf(easing, f.timeKey);
        const QString stored = easing.value(easingKey).toString();
        const QString shown = curves.contains(stored) ? stored : Engine::defaultEasing(f.timeKey);
        curveCombo->setCurrentIndex(std::max(0, int(curves.indexOf(shown))));
        curveLay->addWidget(new ResetLabel(QStringLiteral("Easing"), [this, row0 = f.row, key = f.timeKey] {
            applyEasingCurve(row0, key, QString()); // back to its group's, or the default one
        }));
        curveLay->addWidget(curveCombo);
        curveLay->addStretch();
        bv->addWidget(curveBox);

        connect(curveCombo, qOverload<int>(&QComboBox::activated), this, [this, row0, key, curveCombo]() {
            applyEasingCurve(row0, key, curveCombo->currentData().toString());
        });
    }
    m_detailLayout->addStretch();
}

// Everything the snapshot stores for this layer, as editable rows: what it is (its source, its mesh, its routing), then
// its parameters by category (as their labels name them), the numbers of one address together (x y, r g b a…). A
// value that fades has a time; so has a group of them, for all of its values that have none of their own.
void SnapshotPanel::fillLayer(QTreeWidgetItem *parent, int row, const QJsonObject &o)
{
    const QString lkey = QStringLiteral("L") + o.value("id").toString();
    const quint64 id = o.value("id").toString().toULongLong();
    const bool group = o.value("group").toBool();
    const QJsonObject src = o.value("source").toObject();
    const QJsonObject params = o.value("params").toObject();
    const QString type = src.value("type").toString();

    auto expand = [&](QTreeWidgetItem *it) { it->setExpanded(m_expanded.contains(it->data(0, KeyRole).toString())); };
    // A node of the tree: a section, or a group of values with a time of its own
    auto node = [&](QTreeWidgetItem *p, const QString &label, const QString &key, const QString &timeKey) {
        QTreeWidgetItem *it;
        if (timeKey.isEmpty()) {
            it = new QTreeWidgetItem(p, {label});
            it->setFlags(Qt::ItemIsEnabled);
        } else {
            MemField f;
            f.row = row;
            f.label = label;
            f.timeKey = timeKey;
            it = addField(p, f, QJsonValue(QString()));
            it->setForeground(ColName, QColor(200, 200, 206));
        }
        it->setData(0, KeyRole, lkey + "/" + key);
        QFont font = it->font(0);
        font.setItalic(true);
        it->setFont(0, font);
        if (timeKey.isEmpty()) it->setForeground(0, QColor(200, 200, 206));
        return it;
    };
    auto info = [&](QTreeWidgetItem *p, const QString &label, const QString &text, const QString &tip = QString(),
                    const QString &timeKey = QString()) {
        MemField f; // Info: shown, not editable
        f.row = row;
        f.label = label;
        f.timeKey = timeKey;
        auto *it = addField(p, f, QJsonValue(text));
        if (!tip.isEmpty()) it->setToolTip(1, tip);
        return it;
    };
    auto flag = [&](QTreeWidgetItem *p, const QString &label, const QStringList &path) {
        MemField f;
        f.kind = MemField::Bool;
        f.row = row;
        f.path = path;
        f.label = label;
        return addField(p, f, jsonAt(o, path));
    };

    flag(parent, QStringLiteral("Visible"), {"enable"});
    flag(parent, QStringLiteral("Locked"), {"locked"});
    if (!group) {
        QTreeWidgetItem *what = nullptr;
        if (type == "none" || type.isEmpty()) {
            what = info(parent, QStringLiteral("Source"), QStringLiteral("—"));
        } else if (type == "layer") {
            const Layer *from = m_engine->layer(m_engine->indexOfId(src.value("layer").toString().toULongLong()));
            what = info(parent, QStringLiteral("Source"), from ? from->name : QStringLiteral("(gone)"));
        } else if (type == "text") {
            what = info(parent, QStringLiteral("Source"), QStringLiteral("Text"));
        } else {
            what = info(parent, type == "isf" ? QStringLiteral("Shader") : QStringLiteral("File"),
                        QFileInfo(src.value("path").toString()).fileName(), src.value("path").toString());
        }
        // Another source than the layer's at the recall: its transition, over this time
        m_fields[size_t(what->data(0, FieldRole).toInt())].timeKey = QStringLiteral("file");
        showTime(what, m_fields[size_t(what->data(0, FieldRole).toInt())]);
        const QString tr = src.value("transition").toString();
        info(parent, QStringLiteral("Transition"), tr.isEmpty() ? QStringLiteral("Default") : QFileInfo(tr).completeBaseName(), tr);
    }
    if (o.contains("viewports"))
        info(parent, QStringLiteral("Viewports"),
             o.value("viewports").toObject().isEmpty() ? QStringLiteral("All") : QStringLiteral("Routed"),
             QStringLiteral("How much of it each viewport shows"), QStringLiteral("viewports"));
    if (o.contains("mesh")) {
        const QJsonObject mesh = o.value("mesh").toObject();
        info(parent, QStringLiteral("Mesh"), QStringLiteral("%1 × %2").arg(mesh.value("cols").toInt(4)).arg(mesh.value("rows").toInt(4)),
             QStringLiteral("Its warp's points"), QStringLiteral("mesh"));
    }

    // Its parameters: what they are, from the layer when it is still there (else from their values)
    std::vector<ParamInfo> list;
    {
        QSet<QString> known;
        for (const ParamInfo &i : m_engine->parameters(id))
            if (params.contains(i.path)) {
                list.push_back(i);
                known.insert(i.path);
            }
        QStringList rest;
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            if (!known.contains(it.key())) rest << it.key();
        std::sort(rest.begin(), rest.end());
        for (const QString &path : rest) {
            ParamInfo i;
            i.path = i.label = path;
            const QJsonValue v = params.value(path);
            i.type = v.isBool() ? ParamInfo::Type::Bool : v.isDouble() ? ParamInfo::Type::Float : ParamInfo::Type::Text;
            i.ramp = v.isDouble() ? ParamInfo::Ramp::Linear : ParamInfo::Ramp::Cut;
            i.min = i.lo = -1e6;
            i.max = i.hi = 1e6;
            list.push_back(i);
        }
    }
    static const QString kSep = QStringLiteral(" › ");
    auto categoryOf = [](const ParamInfo &i) {
        const int cut = i.label.lastIndexOf(kSep);
        return cut < 0 ? QString() : i.label.left(cut);
    };
    auto parentOf = [](const QString &path) {
        const int cut = path.lastIndexOf('/');
        return cut < 0 ? QString() : path.left(cut);
    };
    // The address all the values of a category share ("roi", "fx/blur"), if they share one: the key of its time
    QHash<QString, QStringList> pathsIn; // category (and the categories above it) → addresses
    for (const ParamInfo &i : list) {
        QString c = categoryOf(i);
        while (!c.isEmpty()) {
            pathsIn[c] << i.path;
            const int cut = c.lastIndexOf(kSep);
            c = cut < 0 ? QString() : c.left(cut);
        }
    }
    auto sharedAddress = [&](const QStringList &paths) {
        if (paths.size() < 2) return QString();
        QStringList common = paths.front().split('/');
        common.removeLast();
        for (const QString &p : paths) {
            const QStringList seg = p.split('/');
            int k = 0;
            while (k < common.size() && k < seg.size() - 1 && common[k] == seg[k]) ++k;
            common = common.mid(0, k);
        }
        return common.join('/');
    };
    QHash<QString, QTreeWidgetItem *> categories;
    std::function<QTreeWidgetItem *(const QString &)> categoryNode = [&](const QString &c) -> QTreeWidgetItem * {
        if (c.isEmpty()) return parent;
        if (QTreeWidgetItem *it = categories.value(c)) return it;
        const int cut = c.lastIndexOf(kSep);
        QTreeWidgetItem *up = categoryNode(cut < 0 ? QString() : c.left(cut));
        QTreeWidgetItem *it = node(up, cut < 0 ? c : c.mid(cut + kSep.size()), QStringLiteral("cat/") + c, sharedAddress(pathsIn.value(c)));
        categories.insert(c, it);
        return it;
    };
    // The values of one address together ("position": x y), unless that is their whole category's
    QHash<QString, int> siblings; // category + address → how many values
    for (const ParamInfo &i : list) ++siblings[categoryOf(i) + '\n' + parentOf(i.path)];
    QHash<QString, QTreeWidgetItem *> groups;
    for (const ParamInfo &i : list) {
        const QString c = categoryOf(i), up = parentOf(i.path);
        QTreeWidgetItem *p = categoryNode(c);
        const QString gk = c + '\n' + up;
        if (!up.isEmpty() && siblings.value(gk) >= 2 && up != sharedAddress(pathsIn.value(c))) {
            QTreeWidgetItem *&g = groups[gk];
            if (!g) {
                // Its name: what its values' names share ("Position X", "Position Y": "Position")
                QString name;
                bool first = true;
                for (const ParamInfo &x : list)
                    if (categoryOf(x) == c && parentOf(x.path) == up) {
                        if (first) name = x.name();
                        first = false;
                        int k = 0;
                        while (k < name.size() && k < x.name().size() && name[k] == x.name()[k]) ++k;
                        name.truncate(k);
                    }
                name = name.trimmed();
                g = node(p, name.isEmpty() ? up.section('/', -1) : name, QStringLiteral("addr/") + up, up);
            }
            p = g;
        }
        MemField f;
        f.row = row;
        f.path = {QStringLiteral("params"), i.path};
        f.label = i.name();
        const QJsonValue v = params.value(i.path);
        switch (i.type) {
        case ParamInfo::Type::Float:
        case ParamInfo::Type::Int:
            f.kind = MemField::Number;
            f.min = i.min;
            f.max = i.max;
            f.step = i.step > 0 ? i.step : 0.01;
            f.decimals = f.step >= 1 ? 0 : std::clamp(int(std::ceil(-std::log10(f.step))), 0, 6);
            break;
        case ParamInfo::Type::Bool: f.kind = MemField::Bool; break;
        case ParamInfo::Type::Choice:
            f.kind = MemField::Choice;
            f.keys = f.labels = i.choices;
            break;
        default: f.kind = MemField::Info; break; // a text: shown
        }
        if ((i.isNumber() && i.ramp != ParamInfo::Ramp::Cut) || i.ramp == ParamInfo::Ramp::Typed) f.timeKey = i.path;
        QTreeWidgetItem *it = addField(p, f, v);
        it->setToolTip(0, i.path); // its address
    }
    for (QTreeWidgetItem *it : std::as_const(groups)) expand(it);
    for (QTreeWidgetItem *it : std::as_const(categories)) expand(it);
}

// Writes a value into the selected snapshot: what the recall will apply changes, the composition does not move.
void SnapshotPanel::applyField(const MemField &f, const QJsonValue &value)
{
    const int i = selected();
    if (i < 0) return;
    Engine::Snapshot m = m_engine->snapshot(i);
    if (!setRowObject(m, f.row, jsonWith(rowObject(m, f.row), f.path, value).toObject())) return;
    m_applying = true;
    m_engine->setSnapshot(i, m);
    m_applying = false;
    emit edited();
    refreshRows();
}

// The time of a stored value, in the selected snapshot (< 0: FOLLOW, the snapshot's fade; 0: CUT)
void SnapshotPanel::applyTime(int row, const QString &key, double seconds)
{
    const int i = selected();
    if (i < 0 || key.isEmpty()) return;
    Engine::Snapshot m = m_engine->snapshot(i);
    if (row >= m.layers.size()) return;
    QJsonObject o = rowObject(m, row);
    QJsonObject timing = o.value("timing").toObject();
    if (seconds < 0) timing.remove(key);
    else timing[key] = seconds;
    if (timing.isEmpty()) o.remove("timing");
    else o["timing"] = timing;
    setRowObject(m, row, o);
    m_applying = true;
    m_engine->setSnapshot(i, m);
    m_applying = false;
    emit edited();
    refreshRows();
}

// The easing of a value (or of a group of values) in the selected snapshot (empty: none of its own)
void SnapshotPanel::applyEasingCurve(int row, const QString &key, const QString &curveKey)
{
    const int i = selected();
    if (i < 0 || key.isEmpty()) return;
    Engine::Snapshot m = m_engine->snapshot(i);
    if (row >= m.layers.size()) return;
    QJsonObject o = rowObject(m, row);
    QJsonObject easing = o.value("easing").toObject();
    if (curveKey.isEmpty()) easing.remove(key);
    else easing[key] = curveKey;
    if (easing.isEmpty()) o.remove("easing");
    else o["easing"] = easing;
    setRowObject(m, row, o);
    m_applying = true;
    m_engine->setSnapshot(i, m);
    m_applying = false;
    emit edited();
    refreshRows();
    showDetail(m_layers->currentItem());
}

void SnapshotPanel::store()
{
    Engine::Snapshot m;
    m.name = QStringLiteral("Snapshot %1").arg(m_engine->snapshotCount() + 1);
    m.layers = m_engine->captureLayers();
    m.composition = m_engine->captureComposition();
    m.thumbnail = m_engine->grabOutput().scaled(kThumb * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int i = m_engine->addSnapshot(m);
    m_list->setCurrentItem(m_list->topLevelItem(i));
    emit edited();
}

void SnapshotPanel::recall(int i)
{
    if (i < 0 || i >= m_engine->snapshotCount()) return;
    m_undo->push(new cmd::RecallSnapshot(m_engine, i));
    emit recalled();
}

void SnapshotPanel::updateSnapshot(int i)
{
    if (i < 0 || i >= m_engine->snapshotCount()) return;
    Engine::Snapshot m = m_engine->snapshot(i);
    // Layers left out stay out; the times given to values are kept
    QSet<quint64> excluded;
    QHash<quint64, QJsonValue> timing, easings;
    for (const QJsonValue &v : m.layers) {
        const QJsonObject o = v.toObject();
        const quint64 id = o.value("id").toString().toULongLong();
        if (!o.value("included").toBool(true)) excluded.insert(id);
        if (o.contains("timing")) timing.insert(id, o.value("timing"));
        if (o.contains("easing")) easings.insert(id, o.value("easing"));
    }
    QJsonArray layers = m_engine->captureLayers();
    for (int k = 0; k < layers.size(); ++k) {
        QJsonObject o = layers[k].toObject();
        const quint64 id = o.value("id").toString().toULongLong();
        if (excluded.contains(id)) o["included"] = false;
        if (timing.contains(id)) o["timing"] = timing.value(id);
        if (easings.contains(id)) o["easing"] = easings.value(id);
        layers[k] = o;
    }
    m.layers = layers;
    // The composition: left out stays out, its times are kept
    QJsonObject comp = m_engine->captureComposition();
    if (!m.composition.isEmpty()) {
        comp["included"] = m.composition.value("included").toBool(true);
        if (m.composition.contains("timing")) comp["timing"] = m.composition.value("timing");
        if (m.composition.contains("easing")) comp["easing"] = m.composition.value("easing");
    }
    m.composition = comp;
    m.thumbnail = m_engine->grabOutput().scaled(kThumb * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_engine->setSnapshot(i, m);
    emit edited();
}

void SnapshotPanel::removeSnapshot(int i)
{
    if (i < 0 || i >= m_engine->snapshotCount()) return;
    if (m_active == i) m_active = -1;
    else if (m_active > i) --m_active;
    m_engine->removeSnapshot(i);
    emit edited();
}

void SnapshotPanel::setCompositionIncluded(int i, bool included)
{
    if (i < 0) return;
    Engine::Snapshot m = m_engine->snapshot(i);
    if (m.composition.isEmpty()) m.composition = m_engine->captureComposition();
    m.composition["included"] = included;
    m_applying = true;
    m_engine->setSnapshot(i, m);
    m_applying = false;
    emit edited();
}

// The composition in the snapshot: its opacity and the sound volume (a snapshot stored before they were kept:
// nothing, the recall leaves them)
void SnapshotPanel::fillComposition(const QJsonObject &c)
{
    auto *top = new QTreeWidgetItem(m_layers, {QStringLiteral("Composition"),
                                               c.isEmpty() ? QStringLiteral("—")
                                                           : QStringLiteral("%1%").arg(std::lround(c.value("opacity").toDouble(1) * 100))});
    top->setData(0, KeyRole, QStringLiteral("composition"));
    QFont bold = top->font(0);
    bold.setBold(true);
    top->setFont(0, bold);
    if (c.isEmpty()) {
        top->setFlags(Qt::ItemIsEnabled);
        top->setToolTip(0, QStringLiteral("Stored before snapshots kept the composition: Update to add it"));
        return;
    }
    top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    top->setCheckState(0, c.value("included").toBool(true) ? Qt::Checked : Qt::Unchecked);
    auto num = [&](const QString &label, const QString &key, double hi, const QString &tip) {
        MemField f;
        f.kind = MemField::Number;
        f.row = -1;
        f.path = {key};
        f.min = 0;
        f.max = hi;
        f.scale = 100;
        f.decimals = 0;
        f.suffix = QStringLiteral(" %");
        f.step = 0.01;
        f.label = label;
        f.timeKey = key;
        addField(top, f, c.value(key))->setToolTip(0, tip);
    };
    num(QStringLiteral("Opacity"), QStringLiteral("opacity"), 1, QStringLiteral("The composition fader (the blackout stays apart)"));
    num(QStringLiteral("Volume"), QStringLiteral("volume"), 2, QStringLiteral("Volume of the composition sound"));
    top->setExpanded(m_expanded.contains(QStringLiteral("composition")));
}

void SnapshotPanel::setInclusion(int i, quint64 id, bool included)
{
    if (i < 0) return;
    Engine::Snapshot m = m_engine->snapshot(i);
    for (int k = 0; k < m.layers.size(); ++k) {
        QJsonObject o = m.layers[k].toObject();
        if (o.value("id").toString().toULongLong() != id) continue;
        o["included"] = included;
        m.layers[k] = o;
    }
    m_applying = true;
    m_engine->setSnapshot(i, m);
    m_applying = false;
    emit edited();
}
