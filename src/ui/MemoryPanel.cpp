#include "MemoryPanel.h"
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
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>
#include <functional>

static const QSize kThumb(160, 90);

// Roles of the trees
enum { IdRole = Qt::UserRole, FieldRole = Qt::UserRole + 1, KeyRole = Qt::UserRole + 3, MemoryRole = Qt::UserRole + 6 };
// Columns of the tree of what a memory holds
enum Column { ColName, ColValue, ColTime };

// How a value gets there at the recall
static QString timeText(const QJsonValue &v)
{
    if (!v.isDouble()) return QStringLiteral("Follow");
    return v.toDouble() <= 0 ? QStringLiteral("Cut") : QStringLiteral("%1 s").arg(v.toDouble(), 0, 'g', 4);
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

// Easing curve names for UI (these match the engine's curve types)
static QStringList easingCurveLabels()
{
    return {QStringLiteral("Linear"), QStringLiteral("Ease In"), QStringLiteral("Ease Out"),
            QStringLiteral("Ease In-Out"), QStringLiteral("Ease In Cubic"), QStringLiteral("Ease Out Cubic")};
}

static QStringList easingCurveKeys()
{
    return {QStringLiteral("linear"), QStringLiteral("ease_in"), QStringLiteral("ease_out"),
            QStringLiteral("ease_in_out"), QStringLiteral("ease_in_cubic"), QStringLiteral("ease_out_cubic")};
}

// The object a field's row refers to: a layer of the memory, or (-1) its composition
static QJsonObject rowObject(const Engine::Memory &m, int row)
{
    if (row < 0) return m.composition;
    return row < m.layers.size() ? m.layers.at(row).toObject() : QJsonObject();
}
static bool setRowObject(Engine::Memory &m, int row, const QJsonObject &o)
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

// Labels and ranges of a shader's parameters, read from the layer if it is still in the composition.
struct IsfMeta {
    QString label;
    double min = 0, max = 1;
    bool hasRange = false;
};

static QHash<QString, IsfMeta> isfMeta(Engine *e, quint64 id, int slot, QStringList *order)
{
    QHash<QString, IsfMeta> out;
    Engine::Lock lk(&e->mutex());
    const int li = e->indexOfId(id);
    Layer *l = li >= 0 ? e->layer(li) : nullptr;
    if (!l) return out;
    const IsfInstance *inst = slot < 0 ? l->generator.get()
                                       : (slot < int(l->effects.size()) ? l->effects[size_t(slot)].get() : nullptr);
    if (!inst) return out;
    for (const IsfInput &in : inst->inputs()) {
        if (in.isInputImage) continue;
        IsfMeta m;
        m.label = in.label.isEmpty() ? in.name : in.label;
        if (in.type == IsfInput::Float) {
            m.min = in.fMin;
            m.max = in.fMax;
            m.hasRange = in.fMax > in.fMin;
        } else if (in.type == IsfInput::Point2D && in.hasPointRange) {
            m.min = std::min(in.pMin.x(), in.pMin.y());
            m.max = std::max(in.pMax.x(), in.pMax.y());
            m.hasRange = m.max > m.min;
        }
        out.insert(in.name, m);
        if (order) *order << in.name;
    }
    return out;
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
// The list of the memories; a memory dragged from it carries its id (onto a step of a sequence)
class MemoryList : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(kMemoryMime)}; }
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        if (items.isEmpty()) return nullptr;
        auto *m = new QMimeData;
        m->setData(kMemoryMime, QByteArray::number(items.first()->data(0, MemoryRole).toULongLong()));
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

// The list of the memories: the one recalled last is marked apart from the selection — a green stripe on its left
// and ▶ before its number — and its fade fills the row's bottom while it runs.
namespace {
const QColor kLiveMemory(76, 217, 100);

class MemoryRowDelegate : public QStyledItemDelegate
{
public:
    MemoryRowDelegate(Engine *e, QObject *parent) : QStyledItemDelegate(parent), m_engine(e) {}
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(p, opt, index);
        const Engine::RecallProgress r = m_engine->recallProgress();
        if (!r.memory || index.siblingAtColumn(0).data(MemoryRole).toULongLong() != r.memory) return;
        p->save();
        if (index.column() == 0) p->fillRect(QRectF(opt.rect.left(), opt.rect.top(), 4, opt.rect.height()), kLiveMemory);
        if (r.running()) { // the whole row's width is the memory's time
            const QAbstractItemView *view = qobject_cast<const QAbstractItemView *>(opt.widget);
            const int total = view ? view->viewport()->width() : opt.rect.width();
            const double fill = r.fraction() * total;
            const QRectF bar(opt.rect.left(), opt.rect.bottom() - 2, opt.rect.width(), 3);
            const QRectF done = bar.intersected(QRectF(0, bar.top(), fill, bar.height()));
            if (!done.isEmpty()) p->fillRect(done, kLiveMemory);
        }
        p->restore();
    }

private:
    Engine *m_engine;
};
} // namespace

MemoryPanel::MemoryPanel(Engine *engine, QUndoStack *undo, QWidget *parent)
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
    auto *title = new QLabel(QStringLiteral("<b>Memories</b>"));
    bar->addWidget(title);
    bar->addStretch();
    m_store = new QPushButton(QStringLiteral("+"));
    m_store->setToolTip(QStringLiteral("Store the current state of the layers in a new memory"));
    m_store->setFixedWidth(34);
    m_go = new QPushButton(QStringLiteral("GO"));
    m_go->setStyleSheet("QPushButton { font-weight:bold; background:#2f6b3a; color:white; padding:3px 14px; }");
    m_go->setToolTip(QStringLiteral("Recall the selected memory (double-click or Enter in the list)"));
    bar->addWidget(m_store);
    bar->addWidget(m_go);
    lv->addLayout(bar);
    m_list = new MemoryList;
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
                                      "Green stripe and ▶: the memory recalled last; the green line under it: its fade"));
    m_list->setItemDelegate(new MemoryRowDelegate(m_engine, m_list));
    { // the fade running moves the line under its memory
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

    // --- What the selected memory holds
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
    m_fade->setToolTip(QStringLiteral("Fade: the values set to Follow get to the memory's in this time; sources and "
                                      "effect chains change at once (a new source comes in with its transition)"));
    auto *fadeRow = new QHBoxLayout;
    fadeRow->addWidget(new ResetLabel(QStringLiteral("Fade"), [this] { m_fade->setValue(1.0); }));
    fadeRow->addWidget(m_fade, 1);
    fields->addWidget(m_title);
    fields->addWidget(m_name);
    fields->addLayout(fadeRow);
    auto *buttons = new QHBoxLayout;
    m_update = new QPushButton(QStringLiteral("Update"));
    m_update->setToolTip(QStringLiteral("Store the current state of the layers into this memory"));
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
    m_layers->setToolTip(QStringLiteral("Unchecked layers are left alone when the memory is recalled; layers the memory "
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
    connect(del, &QShortcut::activated, this, [this] { removeMemory(selected()); });
    auto *bs = new QShortcut(QKeySequence(Qt::Key_Backspace), m_list, nullptr, nullptr, Qt::WidgetShortcut);
    connect(bs, &QShortcut::activated, this, [this] { removeMemory(selected()); });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *it = m_list->itemAt(pos);
        const int i = it ? m_list->indexOfTopLevelItem(it) : -1;
        QMenu menu;
        menu.addAction(QStringLiteral("Store Current State"), this, &MemoryPanel::store);
        if (i >= 0) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("Recall"), this, [this, i] { recall(i); });
            menu.addAction(QStringLiteral("Update with Current State"), this, [this, i] { updateMemory(i); });
            menu.addAction(QStringLiteral("Rename"), this, [this] {
                m_name->setFocus();
                m_name->selectAll();
            });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete"), this, [this, i] { removeMemory(i); });
        }
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_store, &QPushButton::clicked, this, &MemoryPanel::store);
    connect(m_go, &QPushButton::clicked, this, [this] { recall(selected()); });
    connect(m_update, &QPushButton::clicked, this, [this] { updateMemory(selected()); });
    connect(m_delete, &QPushButton::clicked, this, [this] { removeMemory(selected()); });
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        const int i = selected();
        if (i < 0) return;
        Engine::Memory m = m_engine->memory(i);
        if (m.name == m_name->text()) return;
        m.name = m_name->text();
        m_engine->setMemory(i, m);
        emit edited();
    });
    connect(m_fade, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        const int i = selected();
        if (i < 0 || m_filling) return;
        Engine::Memory m = m_engine->memory(i);
        m.fade = v;
        m_applying = true;
        m_engine->setMemory(i, m);
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
    connect(m_engine, &Engine::memoriesChanged, this, &MemoryPanel::refresh);
    connect(m_engine, &Engine::memoryRecalled, this, [this](int i) {
        m_active = i;
        refresh();
    });
    refresh();
}

int MemoryPanel::selected() const
{
    QTreeWidgetItem *it = m_list->currentItem();
    return it ? m_list->indexOfTopLevelItem(it) : -1;
}

void MemoryPanel::refresh()
{
    if (m_applying) return; // our own edit: the panel already shows it
    const int keep = selected();
    m_filling = true;
    m_list->clear();
    const int n = m_engine->memoryCount();
    if (m_active >= n) m_active = -1;
    for (int i = 0; i < n; ++i) {
        const Engine::Memory m = m_engine->memory(i);
        auto *it = new QTreeWidgetItem(m_list, {QString::number(i + 1), m.name, QStringLiteral("%1 s").arg(m.fade, 0, 'f', 1)});
        it->setData(0, MemoryRole, m.id);
        it->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
        it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        if (i == m_active) { // last recalled: ▶, bold, green — not the accent, which is the selection's
            it->setText(0, QStringLiteral("▶ %1").arg(i + 1));
            QFont f = it->font(1);
            f.setBold(true);
            for (int c = 0; c < 3; ++c) {
                it->setFont(c, f);
                it->setForeground(c, kLiveMemory);
            }
        }
    }
    if (keep >= 0 && keep < n) m_list->setCurrentItem(m_list->topLevelItem(keep));
    m_filling = false;
    showInspector(selected());
}

void MemoryPanel::showInspector(int i)
{
    m_filling = true;
    m_fields.clear();
    const bool valid = i >= 0 && i < m_engine->memoryCount();
    for (QWidget *w : std::initializer_list<QWidget *>{m_name, m_fade, m_layers, m_go, m_update, m_delete}) w->setEnabled(valid);
    m_layers->clear();
    if (!valid) {
        m_title->setText(QStringLiteral("<span style='color:#888'>No memory selected.<br>+ stores the current state.</span>"));
        m_thumb->clear();
        m_name->clear();
        m_filling = false;
        showDetail(nullptr);
        return;
    }
    const Engine::Memory m = m_engine->memory(i);
    m_title->setText(QStringLiteral("<b>Memory %1</b>%2").arg(i + 1).arg(i == m_active ? QStringLiteral(" <span style='color:#4cd964'>▶ recalled last</span>") : QString()));
    m_thumb->setPixmap(thumbnail(m.thumbnail));
    m_name->setText(m.name);
    m_fade->setValue(m.fade);
    QTreeWidgetItem *current = nullptr;
    fillComposition(m.composition);
    for (int row = 0; row < m.layers.size(); ++row) {
        const QJsonObject o = m.layers[row].toObject();
        auto *it = new QTreeWidgetItem(m_layers, {(o.contains("parent") ? QStringLiteral("    ") : QString()) + o.value("name").toString(),
                                                  (o.value("visible").toBool(true) ? QStringLiteral("✓ ") : QStringLiteral("— ")) +
                                                      QStringLiteral("%1%").arg(std::lround(o.value("opacity").toDouble(1) * 100))});
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

QTreeWidgetItem *MemoryPanel::addField(QTreeWidgetItem *parent, const MemField &f, const QJsonValue &value)
{
    auto *it = new QTreeWidgetItem(parent, {f.label, valueText(f, value)});
    it->setForeground(ColName, QColor(165, 165, 172));
    it->setData(0, FieldRole, int(m_fields.size()));
    it->setData(0, KeyRole, QStringLiteral("%1/%2").arg(f.row).arg(f.path.join('/')));
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (f.kind == MemField::Info) it->setForeground(ColValue, QColor(140, 140, 146));
    m_fields.push_back(f);
    if (!f.timeKey.isEmpty()) {
        const Engine::Memory m = m_engine->memory(selected());
        const QJsonValue t = rowObject(m, f.row).value("timing").toObject().value(f.timeKey);
        it->setText(ColTime, timeText(t));
        it->setForeground(ColTime, t.isDouble() ? QColor(230, 230, 233) : QColor(130, 130, 136));
    }
    return it;
}

// Values and times shown in the tree, after an edit of the selected memory (the tree is not rebuilt)
void MemoryPanel::refreshRows()
{
    const int i = selected();
    if (i < 0) return;
    const Engine::Memory m = m_engine->memory(i);
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *p) {
        for (int k = 0; k < p->childCount(); ++k) {
            QTreeWidgetItem *c = p->child(k);
            const QVariant fi = c->data(0, FieldRole);
            if (fi.isValid()) {
                const MemField &f = m_fields[size_t(fi.toInt())];
                const QJsonObject o = rowObject(m, f.row);
                c->setText(ColValue, f.kind == MemField::Info ? c->text(ColValue) : valueText(f, jsonAt(o, f.path)));
                if (!f.timeKey.isEmpty()) {
                    const QJsonValue t = o.value("timing").toObject().value(f.timeKey);
                    c->setText(ColTime, timeText(t));
                    c->setForeground(ColTime, t.isDouble() ? QColor(230, 230, 233) : QColor(130, 130, 136));
                }
            }
            walk(c);
        }
    };
    walk(m_layers->invisibleRootItem());
}

// The selected value: what the memory holds (editable), and how it gets there at the recall
void MemoryPanel::showDetail(QTreeWidgetItem *it)
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
                     ? QStringLiteral("In the memory: the recall takes this layer to the values below.")
                     : QStringLiteral("Left out: the recall leaves this layer as it is."));
        } else {
            hint(QStringLiteral("Click a value in the memory to see it, change it, and choose how it gets there when the "
                                "memory is recalled: CUT (at once), FOLLOW (the memory's fade) or a time of its own."));
        }
        m_detailLayout->addStretch();
        return;
    }
    const MemField f = m_fields[size_t(fi.toInt())];
    const Engine::Memory m = m_engine->memory(mi);
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
    hint(QStringLiteral("The memory changes, the composition does not, until the memory is recalled."));

    // How it gets there
    if (!f.timeKey.isEmpty()) {
        const QJsonValue t = o.value("timing").toObject().value(f.timeKey);
        auto *box = new QWidget;
        auto *bv = new QVBoxLayout(box);
        bv->setContentsMargins(0, 10, 0, 0);
        bv->addWidget(new QLabel(f.timeKey == "source/file"         ? QStringLiteral("<b>Transition of the source</b>")
                                 : f.timeKey == "source/text/content" ? QStringLiteral("<b>Typing (typewriter)</b>")
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
        auto *note = new QLabel(QStringLiteral("CUT: at once · FOLLOW: the memory's fade (%1 s) · TIME: this value "
                                               "only, in its own time%2")
                                    .arg(m.fade, 0, 'f', 1)
                                    .arg(f.timeKey == "source/roi" || f.timeKey == "spatial" || f.timeKey.startsWith("color/") ||
                                                     (f.timeKey.startsWith("source/text/") && f.timeKey.endsWith("/color"))
                                             ? QStringLiteral(" (shared by the whole %1)").arg(f.timeKey.section('/', -1))
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

        // Easing curve selector for the parameter
        auto *curveBox = new QWidget;
        auto *curveLay = new QHBoxLayout(curveBox);
        curveLay->setContentsMargins(0, 0, 0, 0);
        curveLay->addWidget(new QLabel(QStringLiteral("Easing:")));
        auto *curveCombo = new QComboBox;
        const auto curves = easingCurveKeys();
        const auto labels = easingCurveLabels();
        for (int k = 0; k < curves.size(); ++k) curveCombo->addItem(labels[k], curves[k]);

        // Read current curve from JSON
        const QString curveKey = f.timeKey + "/curve";
        const QString currentCurve = o.value("timing").toObject().value(curveKey).toString();
        curveCombo->setCurrentIndex(std::max(0, int(curves.indexOf(currentCurve))));

        curveLay->addWidget(curveCombo);
        curveLay->addStretch();
        bv->addWidget(curveBox);

        connect(curveCombo, qOverload<int>(&QComboBox::activated), this, [this, row0, key, curveCombo]() {
            applyEasingCurve(row0, key, curveCombo->currentData().toString());
        });
    }
    m_detailLayout->addStretch();
}

// Everything the memory stores for this layer, as editable rows grouped in sections.
void MemoryPanel::fillLayer(QTreeWidgetItem *parent, int row, const QJsonObject &o)
{
    const QString lkey = QStringLiteral("L") + o.value("id").toString();
    const quint64 id = o.value("id").toString().toULongLong();
    const bool group = o.value("group").toBool();
    const QJsonObject src = o.value("source").toObject();
    const QString type = src.value("type").toString();
    const bool hasSound = type == "video" || type == "audio";

    auto section = [&](QTreeWidgetItem *p, const QString &label, const QString &key) {
        auto *it = new QTreeWidgetItem(p, {label});
        it->setFlags(Qt::ItemIsEnabled);
        it->setData(0, KeyRole, lkey + "/" + key);
        QFont f = it->font(0);
        f.setItalic(true);
        it->setFont(0, f);
        it->setForeground(0, QColor(200, 200, 206));
        return it;
    };
    auto expand = [&](QTreeWidgetItem *it) { it->setExpanded(m_expanded.contains(it->data(0, KeyRole).toString())); };
    auto num = [&](QTreeWidgetItem *p, const QString &label, const QStringList &path, double lo, double hi, double scale,
                   int decimals, const QString &suffix, double step) {
        MemField f;
        f.kind = MemField::Number;
        f.row = row;
        f.path = path;
        f.min = lo;
        f.max = hi;
        f.scale = scale;
        f.decimals = decimals;
        f.suffix = suffix;
        f.step = step;
        f.label = label;
        f.timeKey = Engine::timingKey(path, o); // a value that fades: its time can be chosen
        return addField(p, f, jsonAt(o, path));
    };
    auto flag = [&](QTreeWidgetItem *p, const QString &label, const QStringList &path) {
        MemField f;
        f.kind = MemField::Bool;
        f.row = row;
        f.path = path;
        f.label = label;
        return addField(p, f, jsonAt(o, path));
    };
    auto choice = [&](QTreeWidgetItem *p, const QString &label, const QStringList &path, const QStringList &keys,
                      const QStringList &labels) {
        MemField f;
        f.kind = MemField::Choice;
        f.row = row;
        f.path = path;
        f.keys = keys;
        f.labels = labels;
        f.label = label;
        return addField(p, f, jsonAt(o, path));
    };
    auto info = [&](QTreeWidgetItem *p, const QString &label, const QString &text, const QString &tip = QString()) {
        MemField f; // Info: shown, not editable
        f.row = row;
        f.label = label;
        auto *it = addField(p, f, QJsonValue(text));
        if (!tip.isEmpty()) it->setToolTip(1, tip);
        return it;
    };

    flag(parent, QStringLiteral("Visible"), {"visible"});
    flag(parent, QStringLiteral("Locked"), {"locked"});
    num(parent, QStringLiteral("Opacity"), {"opacity"}, 0, 1, 100, 0, QStringLiteral(" %"), 0.01);
    choice(parent, QStringLiteral("Blend Mode"), {"blend_mode"}, {"normal", "add", "screen", "multiply", "subtract", "difference"},
           {QStringLiteral("Normal"), QStringLiteral("Add"), QStringLiteral("Screen"), QStringLiteral("Multiply"), QStringLiteral("Subtract"),
            QStringLiteral("Difference")});
    if (hasSound) {
        num(parent, QStringLiteral("Volume"), {"volume"}, 0, 2, 100, 0, QStringLiteral(" %"), 0.01);
        flag(parent, QStringLiteral("Muted"), {"muted"});
    }

    // Parameters of a shader (generator or effect), from the values the memory holds
    auto isfParams = [&](QTreeWidgetItem *p, const QStringList &base, int slot) {
        const QJsonObject params = jsonAt(o, base).toObject();
        if (params.isEmpty()) return;
        QStringList order;
        const QHash<QString, IsfMeta> meta = isfMeta(m_engine, id, slot, &order);
        QStringList names = params.keys();
        std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
            const int ia = order.indexOf(a), ib = order.indexOf(b);
            if (ia != ib) return (ia < 0 ? order.size() : ia) < (ib < 0 ? order.size() : ib);
            return a < b;
        });
        for (const QString &name : names) {
            const IsfMeta mt = meta.value(name);
            const QString label = mt.label.isEmpty() ? name : mt.label;
            const double lo = mt.hasRange ? mt.min : -1e6, hi = mt.hasRange ? mt.max : 1e6;
            const double stp = mt.hasRange ? std::max(1e-4, (hi - lo) / 100) : 0.01;
            const QJsonValue v = params.value(name);
            if (v.isBool()) {
                flag(p, label, base + QStringList{name});
            } else if (v.isDouble()) {
                num(p, label, base + QStringList{name}, lo, hi, 1, 3, QString(), stp);
            } else if (v.isArray()) {
                const QJsonArray a = v.toArray();
                static const char *kXY[] = {"X", "Y"};
                static const char *kRGBA[] = {"R", "G", "B", "A"};
                for (int c = 0; c < a.size() && c < 4; ++c) {
                    const QString sub = a.size() == 2 ? QString::fromLatin1(kXY[c]) : QString::fromLatin1(kRGBA[c]);
                    const bool color = a.size() == 4;
                    num(p, label + " " + sub, base + QStringList{name, QString::number(c)}, color ? 0 : lo,
                        color ? 1 : hi, 1, 3, QString(), color ? 0.01 : stp);
                }
            } else if (v.isString()) {
                info(p, label, QFileInfo(v.toString()).fileName(), v.toString());
            }
        }
    };

    if (!group) {
        QTreeWidgetItem *sec = section(parent, QStringLiteral("Source"), QStringLiteral("source"));
        QTreeWidgetItem *what = nullptr;
        if (type == "none") {
            what = info(sec, QStringLiteral("Source"), QStringLiteral("—"));
        } else if (type == "layer") {
            const Layer *from = m_engine->layer(m_engine->indexOfId(src.value("layer").toString().toULongLong()));
            what = info(sec, QStringLiteral("Layer"), from ? from->name : QStringLiteral("(gone)"));
            choice(sec, QStringLiteral("Tap"), {"source", "tap"}, {"prefx", "postfx"},
                   {QStringLiteral("Pre-FX"), QStringLiteral("Post-FX")});
        } else {
            what = info(sec, type == "isf" ? QStringLiteral("Shader") : QStringLiteral("File"),
                        QFileInfo(src.value("path").toString()).fileName(), src.value("path").toString());
        }
        // Another source than the layer's at the recall: its transition, over this time
        m_fields[size_t(what->data(0, FieldRole).toInt())].timeKey = QStringLiteral("source/file");
        const QString tr = src.value("transition").toString();
        info(sec, QStringLiteral("Transition"),
             tr.isEmpty() ? QStringLiteral("Default") : QFileInfo(tr).completeBaseName(), tr);
        if (hasSound) {
            choice(sec, QStringLiteral("Play mode"), {"source", "play_mode"}, {"oneshot", "loop", "pingpong", "stop"},
                   {QStringLiteral("One-shot"), QStringLiteral("Loop"), QStringLiteral("Ping-pong"), QStringLiteral("Stop")});
            num(sec, QStringLiteral("In"), {"source", "in"}, 0, 1e6, 1, 2, QStringLiteral(" s"), 0.1);
            num(sec, QStringLiteral("Out"), {"source", "out"}, -1, 1e6, 1, 2, QStringLiteral(" s"), 0.1)
                ->setToolTip(1, QStringLiteral("−1: end of the media"));
            num(sec, QStringLiteral("Speed"), {"source", "speed"}, -8, 8, 1, 2, QStringLiteral(" ×"), 0.05);
            flag(sec, QStringLiteral("Playing"), {"source", "playing"});
        }
        if (type == "isf") {
            num(sec, QStringLiteral("Width"), {"source", "width"}, 1, 16384, 1, 0, QStringLiteral(" px"), 1);
            num(sec, QStringLiteral("Height"), {"source", "height"}, 1, 16384, 1, 0, QStringLiteral(" px"), 1);
            isfParams(sec, {"source", "params"}, -1);
        }
        if (type == "text") {
            // Text generator: the text (its typing time: the typewriter), the words and switches set at once, the
            // numbers that fade, each with its time and easing
            QTreeWidgetItem *content = info(sec, QStringLiteral("Text"), jsonAt(o, {"source", "content"}).toString());
            m_fields[size_t(content->data(0, FieldRole).toInt())].timeKey = Engine::timingKey({"source", "content"}, o);
            info(sec, QStringLiteral("Font"), jsonAt(o, {"source", "font"}).toString());
            num(sec, QStringLiteral("Size"), {"source", "size"}, 1, 1000, 1, 0, QStringLiteral(" px"), 1);
            static const char *kRgba[] = {"R", "G", "B", "A"};
            auto rgba = [&](QTreeWidgetItem *p, const QString &label, const QString &key) {
                for (int c = 0; c < 4; ++c)
                    num(p, label + " " + QString::fromLatin1(kRgba[c]), {"source", key, QString::number(c)}, 0, 1, 255, 0,
                        QString(), 1.0 / 255);
            };
            rgba(sec, QStringLiteral("Color"), QStringLiteral("color"));
            flag(sec, QStringLiteral("Bold"), {"source", "bold"});
            flag(sec, QStringLiteral("Italic"), {"source", "italic"});
            flag(sec, QStringLiteral("Underline"), {"source", "underline"});
            flag(sec, QStringLiteral("Strikethrough"), {"source", "strike"});
            choice(sec, QStringLiteral("Align"), {"source", "h_align"}, {"left", "center", "right", "justify"},
                   {QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right"), QStringLiteral("Justified")});
            choice(sec, QStringLiteral("Vertical"), {"source", "v_align"}, {"top", "middle", "bottom"},
                   {QStringLiteral("Top"), QStringLiteral("Middle"), QStringLiteral("Bottom")});
            num(sec, QStringLiteral("Line spacing"), {"source", "line_height"}, 0.1, 10, 1, 2, QStringLiteral(" ×"), 0.05);
            num(sec, QStringLiteral("Letter spacing"), {"source", "letter_spacing"}, -200, 500, 1, 1, QStringLiteral(" px"), 0.5);
            QTreeWidgetItem *outline = section(sec, QStringLiteral("Outline"), QStringLiteral("textOutline"));
            num(outline, QStringLiteral("Width"), {"source", "outline"}, 0, 200, 1, 1, QStringLiteral(" px"), 0.5);
            rgba(outline, QStringLiteral("Color"), QStringLiteral("outline_color"));
            expand(outline);
            QTreeWidgetItem *shadow = section(sec, QStringLiteral("Shadow"), QStringLiteral("textShadow"));
            flag(shadow, QStringLiteral("On"), {"source", "shadow"});
            rgba(shadow, QStringLiteral("Color"), QStringLiteral("shadow_color"));
            num(shadow, QStringLiteral("X"), {"source", "shadow_x"}, -2000, 2000, 1, 0, QStringLiteral(" px"), 1);
            num(shadow, QStringLiteral("Y"), {"source", "shadow_y"}, -2000, 2000, 1, 0, QStringLiteral(" px"), 1);
            expand(shadow);
            num(sec, QStringLiteral("Width"), {"source", "width"}, 1, 16384, 1, 0, QStringLiteral(" px"), 1);
            num(sec, QStringLiteral("Height"), {"source", "height"}, 1, 16384, 1, 0, QStringLiteral(" px"), 1);
        }
        expand(sec);
    }

    if (jsonAt(o, {"source", "roi"}).toArray().size() == 4) {
        QTreeWidgetItem *sec = section(parent, QStringLiteral("ROI"), QStringLiteral("roi"));
        static const char *kSides[] = {"Left", "Top", "Right", "Bottom"};
        for (int c = 0; c < 4; ++c)
            num(sec, QString::fromLatin1(kSides[c]), {"source", "roi", QString::number(c)}, 0, 1, 100, 1,
                QStringLiteral(" %"), 0.01);
        expand(sec);
    }

    if (o.contains("color")) {
        QTreeWidgetItem *sec = section(parent, QStringLiteral("Color"), QStringLiteral("color"));
        flag(sec, QStringLiteral("Enable"), {"color", "enable"});
        flag(sec, QStringLiteral("Temperature Enable"), {"color", "temp_enable"});
        num(sec, QStringLiteral("Temperature"), {"color", "temp"}, -ColorAdjust::kTempRange, ColorAdjust::kTempRange, 1, 0,
            QStringLiteral(" K"), 10);
        flag(sec, QStringLiteral("Tint Enable"), {"color", "tint_enable"});
        num(sec, QStringLiteral("Tint"), {"color", "tint"}, -ColorAdjust::kTintRange, ColorAdjust::kTintRange, 1, 0,
            QString(), 1);
        static const char *kRgb[] = {"Red", "Green", "Blue"};
        flag(sec, QStringLiteral("Add Enable"), {"color", "add_enable"});
        for (int c = 0; c < 3; ++c)
            num(sec, QStringLiteral("Add ") + QString::fromLatin1(kRgb[c]), {"color", "add", QString::number(c)}, 0, 1,
                100, 0, QStringLiteral(" %"), 0.01);
        flag(sec, QStringLiteral("Remove Enable"), {"color", "remove_enable"});
        for (int c = 0; c < 3; ++c)
            num(sec, QStringLiteral("Remove ") + QString::fromLatin1(kRgb[c]), {"color", "remove", QString::number(c)}, 0,
                1, 100, 0, QStringLiteral(" %"), 0.01);
        expand(sec);
    }

    if (o.contains("spatial")) {
        const QJsonObject map = o.value("spatial").toObject();
        QTreeWidgetItem *sec = section(parent, QStringLiteral("Spatial"), QStringLiteral("spatial"));
        flag(sec, QStringLiteral("Mesh mode"), {"spatial", "mesh_mode"});
        static const char *kCorners[] = {"Top-left", "Top-right", "Bottom-right", "Bottom-left"};
        for (int c = 0; c < 4 && c < map.value("corners").toArray().size(); ++c) {
            num(sec, QString::fromLatin1(kCorners[c]) + " X", {"spatial", "corners", QString::number(c), "0"}, -10, 10, 1,
                4, QString(), 0.001);
            num(sec, QString::fromLatin1(kCorners[c]) + " Y", {"spatial", "corners", QString::number(c), "1"}, -10, 10, 1,
                4, QString(), 0.001);
        }
        const int cols = map.value("cols").toInt(4), rows = map.value("rows").toInt(4);
        const QJsonArray offsets = map.value("offsets").toArray();
        info(sec, QStringLiteral("Mesh"), QStringLiteral("%1 × %2").arg(cols).arg(rows));
        if (map.value("mesh_mode").toBool() && offsets.size() == cols * rows && offsets.size() <= 64) {
            QTreeWidgetItem *pts = section(sec, QStringLiteral("Mesh points"), QStringLiteral("mesh"));
            for (int k = 0; k < offsets.size(); ++k) {
                const QString label = QStringLiteral("(%1, %2)").arg(k % cols + 1).arg(k / cols + 1);
                num(pts, label + " X", {"spatial", "offsets", QString::number(k), "0"}, -10, 10, 1, 4, QString(), 0.001);
                num(pts, label + " Y", {"spatial", "offsets", QString::number(k), "1"}, -10, 10, 1, 4, QString(), 0.001);
            }
            expand(pts);
        }
        expand(sec);
    }

    const QJsonArray effects = o.value("effects").toArray();
    QTreeWidgetItem *fxSec = section(parent, QStringLiteral("Effects (%1)").arg(effects.size()), QStringLiteral("effects"));
    flag(fxSec, QStringLiteral("Enable"), {"effects_enable"});
    for (int k = 0; k < effects.size(); ++k) {
        const QJsonObject fx = effects[k].toObject();
        QTreeWidgetItem *one = section(fxSec, QStringLiteral("%1. %2").arg(k + 1).arg(QFileInfo(fx.value("path").toString()).completeBaseName()),
                                       QStringLiteral("fx%1").arg(k));
        one->setToolTip(0, fx.value("path").toString());
        flag(one, QStringLiteral("Enable"), {"effects", QString::number(k), "enable"});
        isfParams(one, {"effects", QString::number(k), "params"}, k);
        expand(one);
    }
    expand(fxSec);
}

// Writes a value into the selected memory: what the recall will apply changes, the composition does not move.
void MemoryPanel::applyField(const MemField &f, const QJsonValue &value)
{
    const int i = selected();
    if (i < 0) return;
    Engine::Memory m = m_engine->memory(i);
    if (!setRowObject(m, f.row, jsonWith(rowObject(m, f.row), f.path, value).toObject())) return;
    m_applying = true;
    m_engine->setMemory(i, m);
    m_applying = false;
    emit edited();
    refreshRows();
}

// The time of a stored value, in the selected memory (< 0: FOLLOW, the memory's fade; 0: CUT)
void MemoryPanel::applyTime(int row, const QString &key, double seconds)
{
    const int i = selected();
    if (i < 0 || key.isEmpty()) return;
    Engine::Memory m = m_engine->memory(i);
    if (row >= m.layers.size()) return;
    QJsonObject o = rowObject(m, row);
    QJsonObject timing = o.value("timing").toObject();
    if (seconds < 0) timing.remove(key);
    else timing[key] = seconds;
    if (timing.isEmpty()) o.remove("timing");
    else o["timing"] = timing;
    setRowObject(m, row, o);
    m_applying = true;
    m_engine->setMemory(i, m);
    m_applying = false;
    emit edited();
    refreshRows();
}

// The easing curve of a parameter in the selected memory's timing
void MemoryPanel::applyEasingCurve(int row, const QString &paramKey, const QString &curveKey)
{
    const int i = selected();
    if (i < 0 || paramKey.isEmpty() || curveKey.isEmpty()) return;
    Engine::Memory m = m_engine->memory(i);
    if (row >= m.layers.size()) return;
    QJsonObject o = rowObject(m, row);
    QJsonObject timing = o.value("timing").toObject();
    const QString key = paramKey + "/curve";
    timing[key] = curveKey;
    o["timing"] = timing;
    setRowObject(m, row, o);
    m_applying = true;
    m_engine->setMemory(i, m);
    m_applying = false;
    emit edited();
    refreshRows();
}

void MemoryPanel::store()
{
    Engine::Memory m;
    m.name = QStringLiteral("Memory %1").arg(m_engine->memoryCount() + 1);
    m.layers = m_engine->captureLayers();
    m.composition = m_engine->captureComposition();
    m.thumbnail = m_engine->grabOutput().scaled(kThumb * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int i = m_engine->addMemory(m);
    m_list->setCurrentItem(m_list->topLevelItem(i));
    emit edited();
}

void MemoryPanel::recall(int i)
{
    if (i < 0 || i >= m_engine->memoryCount()) return;
    m_undo->push(new cmd::RecallMemory(m_engine, i));
    emit recalled();
}

void MemoryPanel::updateMemory(int i)
{
    if (i < 0 || i >= m_engine->memoryCount()) return;
    Engine::Memory m = m_engine->memory(i);
    // Layers left out stay out; the times given to values are kept
    QSet<quint64> excluded;
    QHash<quint64, QJsonValue> timing;
    for (const QJsonValue &v : m.layers) {
        const QJsonObject o = v.toObject();
        const quint64 id = o.value("id").toString().toULongLong();
        if (!o.value("included").toBool(true)) excluded.insert(id);
        if (o.contains("timing")) timing.insert(id, o.value("timing"));
    }
    QJsonArray layers = m_engine->captureLayers();
    for (int k = 0; k < layers.size(); ++k) {
        QJsonObject o = layers[k].toObject();
        const quint64 id = o.value("id").toString().toULongLong();
        if (excluded.contains(id)) o["included"] = false;
        if (timing.contains(id)) o["timing"] = timing.value(id);
        layers[k] = o;
    }
    m.layers = layers;
    // The composition: left out stays out, its times are kept
    QJsonObject comp = m_engine->captureComposition();
    if (!m.composition.isEmpty()) {
        comp["included"] = m.composition.value("included").toBool(true);
        if (m.composition.contains("timing")) comp["timing"] = m.composition.value("timing");
    }
    m.composition = comp;
    m.thumbnail = m_engine->grabOutput().scaled(kThumb * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_engine->setMemory(i, m);
    emit edited();
}

void MemoryPanel::removeMemory(int i)
{
    if (i < 0 || i >= m_engine->memoryCount()) return;
    if (m_active == i) m_active = -1;
    else if (m_active > i) --m_active;
    m_engine->removeMemory(i);
    emit edited();
}

void MemoryPanel::setCompositionIncluded(int i, bool included)
{
    if (i < 0) return;
    Engine::Memory m = m_engine->memory(i);
    if (m.composition.isEmpty()) m.composition = m_engine->captureComposition();
    m.composition["included"] = included;
    m_applying = true;
    m_engine->setMemory(i, m);
    m_applying = false;
    emit edited();
}

// The composition in the memory: its opacity and the sound volume (a memory stored before they were kept:
// nothing, the recall leaves them)
void MemoryPanel::fillComposition(const QJsonObject &c)
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
        top->setToolTip(0, QStringLiteral("Stored before memories kept the composition: Update to add it"));
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

void MemoryPanel::setInclusion(int i, quint64 id, bool included)
{
    if (i < 0) return;
    Engine::Memory m = m_engine->memory(i);
    for (int k = 0; k < m.layers.size(); ++k) {
        QJsonObject o = m.layers[k].toObject();
        if (o.value("id").toString().toULongLong() != id) continue;
        o["included"] = included;
        m.layers[k] = o;
    }
    m_applying = true;
    m_engine->setMemory(i, m);
    m_applying = false;
    emit edited();
}
