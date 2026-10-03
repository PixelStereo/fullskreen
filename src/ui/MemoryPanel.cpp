#include "MemoryPanel.h"
#include "Commands.h"
#include "Engine.h"
#include "Widgets.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

static constexpr int kPlus = -1;
static const QSize kThumb(128, 72);

static QIcon plusIcon()
{
    QPixmap pm(kThumb);
    pm.fill(QColor(40, 40, 44));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(120, 120, 128), 1, Qt::DashLine));
    p.drawRoundedRect(QRectF(1, 1, kThumb.width() - 2, kThumb.height() - 2), 4, 4);
    p.setPen(QPen(theme::accent(), 4, Qt::SolidLine, Qt::RoundCap));
    const QPointF c(kThumb.width() / 2.0, kThumb.height() / 2.0);
    p.drawLine(c - QPointF(12, 0), c + QPointF(12, 0));
    p.drawLine(c - QPointF(0, 12), c + QPointF(0, 12));
    return QIcon(pm);
}

// Roles of the inspector tree
enum { IdRole = Qt::UserRole, FieldRole = Qt::UserRole + 1, ValueRole = Qt::UserRole + 2, KeyRole = Qt::UserRole + 3,
       TimeKeyRole = Qt::UserRole + 4, TimeRole = Qt::UserRole + 5 };
enum Column { ColLayer, ColShown, ColValue, ColTime, ColSource };

// Time of a stored value: the memory's fade (none stored), a cut (0) or its own time
static QString timeText(const QJsonValue &v)
{
    if (!v.isDouble()) return QStringLiteral("Transition");
    return v.toDouble() <= 0 ? QStringLiteral("Cut") : QStringLiteral("%1 s").arg(v.toDouble(), 0, 'g', 4);
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

// Editor of a stored value: spin box for a number, combo box for a choice (booleans are check boxes).
class FieldDelegate : public QStyledItemDelegate
{
public:
    std::function<const MemField *(const QModelIndex &)> field;
    std::function<void(const MemField &, const QJsonValue &)> commit;
    std::function<void(const QModelIndex &, double)> commitTime; // < 0: back to the memory's fade

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        if (index.column() == ColTime) {
            if (!index.data(TimeKeyRole).isValid()) return nullptr;
            auto *b = new QDoubleSpinBox(parent);
            b->setRange(-0.1, 600);
            b->setDecimals(1);
            b->setSingleStep(0.5);
            b->setSuffix(QStringLiteral(" s"));
            b->setSpecialValueText(QStringLiteral("Transition")); // the lowest value: the memory's fade
            b->setKeyboardTracking(false);
            b->setToolTip(QStringLiteral("Transition: the memory's fade · 0: a cut · or a time of its own"));
            return b;
        }
        const MemField *f = field(index);
        if (!f) return nullptr;
        if (f->kind == MemField::Number) {
            auto *b = new QDoubleSpinBox(parent);
            b->setRange(f->min * f->scale, f->max * f->scale);
            b->setDecimals(f->decimals);
            b->setSingleStep(f->step * f->scale);
            b->setSuffix(f->suffix);
            b->setKeyboardTracking(false);
            return b;
        }
        if (f->kind == MemField::Choice) {
            auto *c = new QComboBox(parent);
            for (int k = 0; k < f->keys.size(); ++k) c->addItem(f->labels.value(k, f->keys[k]), f->keys[k]);
            return c;
        }
        return nullptr;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (index.column() == ColTime) {
            if (auto *b = qobject_cast<QDoubleSpinBox *>(editor)) {
                const QVariant t = index.data(TimeRole);
                b->setValue(t.isValid() ? t.toDouble() : b->minimum());
            }
            return;
        }
        const MemField *f = field(index);
        if (!f) return;
        if (auto *b = qobject_cast<QDoubleSpinBox *>(editor)) b->setValue(index.data(ValueRole).toDouble() * f->scale);
        else if (auto *c = qobject_cast<QComboBox *>(editor))
            c->setCurrentIndex(std::max(0, c->findData(index.data(ValueRole).toString())));
    }
    void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const override
    {
        if (index.column() == ColTime) {
            if (auto *b = qobject_cast<QDoubleSpinBox *>(editor))
                commitTime(index, b->value() <= b->minimum() ? -1.0 : b->value());
            return;
        }
        const MemField *f = field(index);
        if (!f) return;
        if (auto *b = qobject_cast<QDoubleSpinBox *>(editor))
            commit(*f, QJsonValue(b->value() / (f->scale != 0 ? f->scale : 1)));
        else if (auto *c = qobject_cast<QComboBox *>(editor))
            commit(*f, QJsonValue(c->currentData().toString()));
    }
};

static QPixmap tile(const QImage &thumb, bool active)
{
    QPixmap pm(kThumb);
    pm.fill(Qt::black);
    QPainter p(&pm);
    if (!thumb.isNull()) {
        const QImage s = thumb.scaled(kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage(QPoint((kThumb.width() - s.width()) / 2, (kThumb.height() - s.height()) / 2), s);
    }
    if (active) { // last recalled
        p.setPen(QPen(theme::accent(), 4));
        p.drawRect(QRect(QPoint(2, 2), kThumb - QSize(4, 4)));
    }
    return pm;
}

MemoryPanel::MemoryPanel(Engine *engine, QUndoStack *undo, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo)
{
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(8, 4, 8, 4);

    auto *left = new QVBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Memories</b>"));
    title->setToolTip(QStringLiteral("+ stores the current state of the layers · click: inspect · "
                                     "double-click or Enter: recall"));
    left->addWidget(title);
    m_grid = new QListWidget;
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setIconSize(kThumb);
    m_grid->setGridSize(kThumb + QSize(18, 30));
    m_grid->setMovement(QListView::Static);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setWrapping(true);
    m_grid->setWordWrap(false);
    m_grid->setTextElideMode(Qt::ElideRight);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    left->addWidget(m_grid, 1);
    h->addLayout(left, 1);

    // Inspector of the selected memory
    m_inspector = new QWidget;
    m_inspector->setFixedWidth(540);
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
    m_fade = new QDoubleSpinBox;
    m_fade->setRange(0, 600);
    m_fade->setDecimals(1);
    m_fade->setSingleStep(0.5);
    m_fade->setSuffix(QStringLiteral(" s"));
    m_fade->setKeyboardTracking(false);
    m_fade->setToolTip(QStringLiteral("Fade: opacity, volume, ROI, color, mapping and ISF numbers move to the memory's "
                                      "values in this time (unless the Time column gives one of them its own); "
                                      "sources and effect chains change at once"));
    auto *fadeRow = new QHBoxLayout;
    fadeRow->addWidget(new ResetLabel(QStringLiteral("Fade"), [this] { m_fade->setValue(1.0); }));
    fadeRow->addWidget(m_fade, 1);
    fields->addWidget(m_title);
    fields->addWidget(m_name);
    fields->addLayout(fadeRow);
    head->addWidget(m_thumb);
    head->addLayout(fields, 1);
    iv->addLayout(head);
    m_layers = new QTreeWidget;
    m_layers->setHeaderLabels({QStringLiteral("Layer"), QStringLiteral("Shown"), QStringLiteral("Value"), QStringLiteral("Time"),
                               QStringLiteral("Source")});
    m_layers->headerItem()->setToolTip(ColTime, QStringLiteral("How long each value takes when the memory is recalled:\n"
                                                               "Transition (the memory's fade), Cut, or a time of its own.\n"
                                                               "Double-click to change it."));
    m_layers->setRootIsDecorated(true);
    m_layers->setIndentation(12);
    m_layers->setUniformRowHeights(true);
    m_layers->setAlternatingRowColors(true);
    m_layers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_layers->header()->setStretchLastSection(false);
    m_layers->setColumnWidth(1, 46);
    m_layers->setColumnWidth(ColValue, 92);
    m_layers->setColumnWidth(ColTime, 76);
    m_layers->setColumnWidth(ColSource, 100);
    m_layers->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
                              QAbstractItemView::EditKeyPressed);
    m_layers->setToolTip(QStringLiteral("Unchecked layers are left alone when the memory is recalled.\n"
                                        "Unfold a layer to see everything it stores; double-click a value to change it\n"
                                        "(the memory changes, the composition does not)."));
    auto *delegate = new FieldDelegate;
    delegate->field = [this](const QModelIndex &i) -> const MemField * {
        if (i.column() != 2) return nullptr;
        const QVariant v = i.data(FieldRole);
        if (!v.isValid()) return nullptr;
        const int k = v.toInt();
        return k >= 0 && k < int(m_fields.size()) ? &m_fields[size_t(k)] : nullptr;
    };
    delegate->commit = [this](const MemField &f, const QJsonValue &v) { applyField(f, v); };
    delegate->commitTime = [this](const QModelIndex &i, double seconds) {
        const QVariant fi = i.siblingAtColumn(ColValue).data(FieldRole);
        if (!fi.isValid()) return;
        applyTime(m_fields[size_t(fi.toInt())].row, i.data(TimeKeyRole).toString(), seconds);
    };
    delegate->setParent(m_layers);
    m_layers->setItemDelegate(delegate);
    iv->addWidget(m_layers, 1);
    auto *buttons = new QHBoxLayout;
    m_go = new QPushButton(QStringLiteral("GO"));
    m_go->setStyleSheet("QPushButton { font-weight:bold; background:#2f6b3a; color:white; padding:4px 16px; }");
    m_go->setToolTip(QStringLiteral("Recall this memory (double-click or Enter in the grid)"));
    m_update = new QPushButton(QStringLiteral("Update"));
    m_update->setToolTip(QStringLiteral("Store the current state of the layers into this memory"));
    m_delete = new QPushButton(QStringLiteral("Delete"));
    buttons->addWidget(m_go);
    buttons->addStretch();
    buttons->addWidget(m_update);
    buttons->addWidget(m_delete);
    iv->addLayout(buttons);
    h->addWidget(m_inspector);

    connect(m_grid, &QListWidget::currentRowChanged, this, [this] {
        if (!m_filling) showInspector(selected());
    });
    connect(m_grid, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        if (it->data(Qt::UserRole).toInt() == kPlus) store();
    });
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        const int i = it->data(Qt::UserRole).toInt();
        if (i != kPlus) recall(i);
    });
    auto *enter = new QShortcut(QKeySequence(Qt::Key_Return), m_grid, nullptr, nullptr, Qt::WidgetShortcut);
    connect(enter, &QShortcut::activated, this, [this] {
        if (selected() >= 0) recall(selected());
    });
    auto *del = new QShortcut(QKeySequence::Delete, m_grid, nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, [this] { removeMemory(selected()); });
    auto *bs = new QShortcut(QKeySequence(Qt::Key_Backspace), m_grid, nullptr, nullptr, Qt::WidgetShortcut);
    connect(bs, &QShortcut::activated, this, [this] { removeMemory(selected()); });
    connect(m_grid, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *it = m_grid->itemAt(pos);
        const int i = it ? it->data(Qt::UserRole).toInt() : kPlus;
        QMenu menu;
        if (i == kPlus) {
            menu.addAction(QStringLiteral("Store Current State"), this, &MemoryPanel::store);
        } else {
            menu.addAction(QStringLiteral("Recall"), this, [this, i] { recall(i); });
            menu.addAction(QStringLiteral("Update with Current State"), this, [this, i] { updateMemory(i); });
            menu.addAction(QStringLiteral("Rename"), this, [this] {
                m_name->setFocus();
                m_name->selectAll();
            });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete"), this, [this, i] { removeMemory(i); });
        }
        menu.exec(m_grid->viewport()->mapToGlobal(pos));
    });
    connect(m_go, &QPushButton::clicked, this, [this] {
        if (selected() >= 0) recall(selected());
    });
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
        m_engine->setMemory(i, m);
        emit edited();
    });
    connect(m_layers, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
        if (m_filling) return;
        if (col == 0 && it->data(0, IdRole).isValid()) { // a layer left in or out of the memory
            setInclusion(selected(), it->data(0, IdRole).toULongLong(), it->checkState(0) == Qt::Checked);
            return;
        }
        if (col != 2) return;
        const QVariant fi = it->data(2, FieldRole);
        if (!fi.isValid()) return;
        const MemField &f = m_fields[size_t(fi.toInt())];
        if (f.kind == MemField::Bool) applyField(f, it->checkState(2) == Qt::Checked);
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
    QListWidgetItem *it = m_grid->currentItem();
    const int i = it ? it->data(Qt::UserRole).toInt() : kPlus;
    return i == kPlus ? -1 : i;
}

void MemoryPanel::refresh()
{
    if (m_applying) return; // our own edit: the inspector is rebuilt on its own
    const int keep = selected();
    m_filling = true;
    m_grid->clear();
    const int n = m_engine->memoryCount();
    if (m_active >= n) m_active = -1;
    for (int i = 0; i < n; ++i) {
        const Engine::Memory m = m_engine->memory(i);
        auto *it = new QListWidgetItem(QIcon(tile(m.thumbnail, i == m_active)),
                                       QStringLiteral("%1  %2").arg(i + 1).arg(m.name), m_grid);
        it->setData(Qt::UserRole, i);
        it->setToolTip(QStringLiteral("%1 — fade %2 s\nDouble-click or Enter to recall").arg(m.name).arg(m.fade));
    }
    auto *plus = new QListWidgetItem(plusIcon(), QStringLiteral("Store"), m_grid);
    plus->setData(Qt::UserRole, kPlus);
    plus->setToolTip(QStringLiteral("Store the current state of the layers in a new memory"));
    m_grid->setCurrentRow(keep >= 0 && keep < n ? keep : -1);
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
        return;
    }
    const Engine::Memory m = m_engine->memory(i);
    m_title->setText(QStringLiteral("<b>Memory %1</b>%2").arg(i + 1).arg(i == m_active ? QStringLiteral(" — active") : QString()));
    m_thumb->setPixmap(tile(m.thumbnail, false));
    m_name->setText(m.name);
    m_fade->setValue(m.fade);
    for (int row = 0; row < m.layers.size(); ++row) {
        const QJsonObject o = m.layers[row].toObject();
        const QJsonObject src = o.value("source").toObject();
        const bool group = o.value("group").toBool();
        const QString source = group ? QStringLiteral("group")
                               : src.value("type").toString() == "none" ? QStringLiteral("—")
                                                                         : QFileInfo(src.value("path").toString()).fileName();
        auto *it = new QTreeWidgetItem(m_layers, {(o.contains("parent") ? QStringLiteral("    ") : QString()) + o.value("name").toString(),
                                                  o.value("visible").toBool(true) ? QStringLiteral("✓") : QStringLiteral("—"),
                                                  QStringLiteral("%1%").arg(std::lround(o.value("opacity").toDouble(1) * 100)),
                                                  QString(), source});
        it->setData(0, IdRole, o.value("id").toString().toULongLong());
        it->setData(0, KeyRole, QStringLiteral("L") + o.value("id").toString());
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        it->setCheckState(0, o.value("included").toBool(true) ? Qt::Checked : Qt::Unchecked);
        it->setToolTip(ColSource, src.value("path").toString());
        if (m_engine->indexOfId(it->data(0, IdRole).toULongLong()) < 0) {
            it->setForeground(0, QColor(255, 180, 90));
            it->setToolTip(0, QStringLiteral("Not in the composition any more: recreated by the recall"));
        }
        fillLayer(it, row, o);
        it->setExpanded(m_expanded.contains(it->data(0, KeyRole).toString()));
    }
    m_filling = false;
}

QTreeWidgetItem *MemoryPanel::addField(QTreeWidgetItem *parent, const QString &label, const MemField &f,
                                       const QJsonValue &value)
{
    auto *it = new QTreeWidgetItem(parent, {label});
    it->setForeground(0, QColor(165, 165, 172));
    it->setData(2, FieldRole, int(m_fields.size()));
    it->setData(2, ValueRole, value.toVariant());
    m_fields.push_back(f);
    switch (f.kind) {
    case MemField::Bool:
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        it->setCheckState(2, value.toBool() ? Qt::Checked : Qt::Unchecked);
        break;
    case MemField::Number:
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        it->setText(2, QString::number(value.toDouble() * f.scale, 'f', f.decimals) + f.suffix);
        break;
    case MemField::Choice: {
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        const int k = f.keys.indexOf(value.toString());
        it->setText(2, k >= 0 ? f.labels.value(k) : value.toString());
        break;
    }
    default:
        it->setFlags(Qt::ItemIsEnabled);
        it->setText(2, value.toString());
        it->setForeground(2, QColor(140, 140, 146));
        break;
    }
    return it;
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
    std::function<void(QTreeWidgetItem *, const QString &)> timeCell;
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
        QTreeWidgetItem *it = addField(p, label, f, jsonAt(o, path));
        timeCell(it, Engine::timingKey(path)); // a value that fades: its time can be chosen
        return it;
    };
    auto timeCellImpl = [&](QTreeWidgetItem *it, const QString &key) {
        if (!key.isEmpty()) {
            it->setFlags(it->flags() | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            const QJsonValue t = o.value("timing").toObject().value(key);
            it->setData(ColTime, TimeKeyRole, key);
            if (t.isDouble()) it->setData(ColTime, TimeRole, t.toDouble());
            it->setText(ColTime, timeText(t));
            it->setForeground(ColTime, t.isDouble() ? QColor(230, 230, 233) : QColor(130, 130, 136));
            it->setToolTip(ColTime, QStringLiteral("Double-click: Transition (the memory's fade), 0 for a cut, or a time "
                                                   "of its own%1")
                                        .arg(key.contains('/') || key == "roi" || key == "mapping"
                                                 ? QStringLiteral(" — shared by every value of %1").arg(key)
                                                 : QString()));
        }
    };
    timeCell = timeCellImpl;
    auto flag = [&](QTreeWidgetItem *p, const QString &label, const QStringList &path) {
        MemField f;
        f.kind = MemField::Bool;
        f.row = row;
        f.path = path;
        return addField(p, label, f, jsonAt(o, path));
    };
    auto choice = [&](QTreeWidgetItem *p, const QString &label, const QStringList &path, const QStringList &keys,
                      const QStringList &labels) {
        MemField f;
        f.kind = MemField::Choice;
        f.row = row;
        f.path = path;
        f.keys = keys;
        f.labels = labels;
        return addField(p, label, f, jsonAt(o, path));
    };
    auto info = [&](QTreeWidgetItem *p, const QString &label, const QString &text, const QString &tip = QString()) {
        MemField f; // Info: shown, not editable
        f.row = row;
        auto *it = addField(p, label, f, QJsonValue(text));
        if (!tip.isEmpty()) it->setToolTip(2, tip);
        return it;
    };

    flag(parent, QStringLiteral("Visible"), {"visible"});
    flag(parent, QStringLiteral("Locked"), {"locked"});
    num(parent, QStringLiteral("Opacity"), {"opacity"}, 0, 1, 100, 0, QStringLiteral(" %"), 0.01);
    choice(parent, QStringLiteral("Blend"), {"blend"}, {"normal", "add", "screen", "multiply"},
           {QStringLiteral("Normal"), QStringLiteral("Add"), QStringLiteral("Screen"), QStringLiteral("Multiply")});
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
        timeCell(what, QStringLiteral("source"));
        what->setToolTip(ColTime, QStringLiteral("When this memory gives the layer another source: how long the "
                                                 "transition takes (Transition: the memory's fade, 0: a cut)"));
        const QString tr = src.value("transition").toString();
        info(sec, QStringLiteral("Transition"),
             tr.isEmpty() ? QStringLiteral("Default") : QFileInfo(tr).completeBaseName(), tr);
        if (hasSound) {
            choice(sec, QStringLiteral("Play mode"), {"source", "playMode"}, {"oneshot", "loop", "pingpong", "stop"},
                   {QStringLiteral("One-shot"), QStringLiteral("Loop"), QStringLiteral("Ping-pong"), QStringLiteral("Stop")});
            num(sec, QStringLiteral("In"), {"source", "in"}, 0, 1e6, 1, 2, QStringLiteral(" s"), 0.1);
            num(sec, QStringLiteral("Out"), {"source", "out"}, -1, 1e6, 1, 2, QStringLiteral(" s"), 0.1)
                ->setToolTip(2, QStringLiteral("−1: end of the media"));
            num(sec, QStringLiteral("Speed"), {"source", "speed"}, -8, 8, 1, 2, QStringLiteral(" ×"), 0.05);
            flag(sec, QStringLiteral("Playing"), {"source", "playing"});
        }
        if (type == "isf") {
            num(sec, QStringLiteral("Width"), {"source", "width"}, 1, 16384, 1, 0, QStringLiteral(" px"), 1);
            num(sec, QStringLiteral("Height"), {"source", "height"}, 1, 16384, 1, 0, QStringLiteral(" px"), 1);
            isfParams(sec, {"source", "params"}, -1);
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
        flag(sec, QStringLiteral("Enabled"), {"color", "enabled"});
        flag(sec, QStringLiteral("Temperature on"), {"color", "tempOn"});
        num(sec, QStringLiteral("Temperature"), {"color", "temp"}, -ColorAdjust::kTempRange, ColorAdjust::kTempRange, 1, 0,
            QStringLiteral(" K"), 10);
        flag(sec, QStringLiteral("Tint on"), {"color", "tintOn"});
        num(sec, QStringLiteral("Tint"), {"color", "tint"}, -ColorAdjust::kTintRange, ColorAdjust::kTintRange, 1, 0,
            QString(), 1);
        static const char *kRgb[] = {"Red", "Green", "Blue"};
        flag(sec, QStringLiteral("Add on"), {"color", "addOn"});
        for (int c = 0; c < 3; ++c)
            num(sec, QStringLiteral("Add ") + QString::fromLatin1(kRgb[c]), {"color", "add", QString::number(c)}, 0, 1,
                100, 0, QStringLiteral(" %"), 0.01);
        flag(sec, QStringLiteral("Remove on"), {"color", "removeOn"});
        for (int c = 0; c < 3; ++c)
            num(sec, QStringLiteral("Remove ") + QString::fromLatin1(kRgb[c]), {"color", "remove", QString::number(c)}, 0,
                1, 100, 0, QStringLiteral(" %"), 0.01);
        expand(sec);
    }

    if (o.contains("mapping")) {
        const QJsonObject map = o.value("mapping").toObject();
        QTreeWidgetItem *sec = section(parent, QStringLiteral("Mapping"), QStringLiteral("mapping"));
        flag(sec, QStringLiteral("Mesh mode"), {"mapping", "meshMode"});
        static const char *kCorners[] = {"Top-left", "Top-right", "Bottom-right", "Bottom-left"};
        for (int c = 0; c < 4 && c < map.value("corners").toArray().size(); ++c) {
            num(sec, QString::fromLatin1(kCorners[c]) + " X", {"mapping", "corners", QString::number(c), "0"}, -10, 10, 1,
                4, QString(), 0.001);
            num(sec, QString::fromLatin1(kCorners[c]) + " Y", {"mapping", "corners", QString::number(c), "1"}, -10, 10, 1,
                4, QString(), 0.001);
        }
        const int cols = map.value("cols").toInt(4), rows = map.value("rows").toInt(4);
        const QJsonArray offsets = map.value("offsets").toArray();
        info(sec, QStringLiteral("Mesh"), QStringLiteral("%1 × %2").arg(cols).arg(rows));
        if (map.value("meshMode").toBool() && offsets.size() == cols * rows && offsets.size() <= 64) {
            QTreeWidgetItem *pts = section(sec, QStringLiteral("Mesh points"), QStringLiteral("mesh"));
            for (int k = 0; k < offsets.size(); ++k) {
                const QString label = QStringLiteral("(%1, %2)").arg(k % cols + 1).arg(k / cols + 1);
                num(pts, label + " X", {"mapping", "offsets", QString::number(k), "0"}, -10, 10, 1, 4, QString(), 0.001);
                num(pts, label + " Y", {"mapping", "offsets", QString::number(k), "1"}, -10, 10, 1, 4, QString(), 0.001);
            }
            expand(pts);
        }
        expand(sec);
    }

    const QJsonArray effects = o.value("effects").toArray();
    QTreeWidgetItem *fxSec = section(parent, QStringLiteral("Effects (%1)").arg(effects.size()), QStringLiteral("effects"));
    flag(fxSec, QStringLiteral("All effects"), {"effectsEnabled"});
    for (int k = 0; k < effects.size(); ++k) {
        const QJsonObject fx = effects[k].toObject();
        QTreeWidgetItem *one = section(fxSec, QStringLiteral("%1. %2").arg(k + 1).arg(QFileInfo(fx.value("path").toString()).completeBaseName()),
                                       QStringLiteral("fx%1").arg(k));
        one->setToolTip(0, fx.value("path").toString());
        flag(one, QStringLiteral("Enabled"), {"effects", QString::number(k), "enabled"});
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
    if (f.row < 0 || f.row >= m.layers.size()) return;
    m.layers[f.row] = jsonWith(m.layers[f.row].toObject(), f.path, value).toObject();
    m_applying = true;
    m_engine->setMemory(i, m);
    m_applying = false;
    emit edited();
    // Rebuilt once the edit is over (an editor or a check box is still live at this point)
    QMetaObject::invokeMethod(this, [this] { showInspector(selected()); }, Qt::QueuedConnection);
}

// The time of a stored value, in the selected memory (< 0: back to the memory's fade)
void MemoryPanel::applyTime(int row, const QString &key, double seconds)
{
    const int i = selected();
    if (i < 0 || key.isEmpty()) return;
    Engine::Memory m = m_engine->memory(i);
    if (row < 0 || row >= m.layers.size()) return;
    QJsonObject o = m.layers[row].toObject();
    QJsonObject timing = o.value("timing").toObject();
    if (seconds < 0) timing.remove(key);
    else timing[key] = seconds;
    if (timing.isEmpty()) o.remove("timing");
    else o["timing"] = timing;
    m.layers[row] = o;
    m_applying = true;
    m_engine->setMemory(i, m);
    m_applying = false;
    emit edited();
    QMetaObject::invokeMethod(this, [this] { showInspector(selected()); }, Qt::QueuedConnection);
}

void MemoryPanel::store()
{
    Engine::Memory m;
    m.name = QStringLiteral("Memory %1").arg(m_engine->memoryCount() + 1);
    m.layers = m_engine->captureLayers();
    m.thumbnail = m_engine->grabOutput().scaled(kThumb * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const int i = m_engine->addMemory(m);
    m_grid->setCurrentRow(i);
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
    m_engine->setMemory(i, m);
    emit edited();
}
