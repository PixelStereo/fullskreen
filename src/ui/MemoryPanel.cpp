#include "MemoryPanel.h"
#include "Commands.h"
#include "Engine.h"
#include "Widgets.h"

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
    p.setPen(QPen(QColor(255, 160, 40), 4, Qt::SolidLine, Qt::RoundCap));
    const QPointF c(kThumb.width() / 2.0, kThumb.height() / 2.0);
    p.drawLine(c - QPointF(12, 0), c + QPointF(12, 0));
    p.drawLine(c - QPointF(0, 12), c + QPointF(0, 12));
    return QIcon(pm);
}

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
        p.setPen(QPen(QColor(255, 160, 40), 4));
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
    m_inspector->setFixedWidth(400);
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
    m_fade->setToolTip(QStringLiteral("Fade: opacity, volume, crop, color, mapping and ISF numbers move to the memory's "
                                      "values in this time; sources and effect chains change at once"));
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
    m_layers->setHeaderLabels({QStringLiteral("Layer"), QStringLiteral("Shown"), QStringLiteral("Opacity"), QStringLiteral("Source")});
    m_layers->setRootIsDecorated(false);
    m_layers->setIndentation(10);
    m_layers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_layers->header()->setStretchLastSection(false);
    m_layers->setColumnWidth(1, 50);
    m_layers->setColumnWidth(2, 60);
    m_layers->setColumnWidth(3, 120);
    m_layers->setToolTip(QStringLiteral("Unchecked layers are left alone when the memory is recalled"));
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
        if (m_filling || col != 0) return;
        setInclusion(selected(), it->data(0, Qt::UserRole).toULongLong(), it->checkState(0) == Qt::Checked);
    });
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
    for (const QJsonValue &v : m.layers) {
        const QJsonObject o = v.toObject();
        const QJsonObject src = o.value("source").toObject();
        const bool group = o.value("group").toBool();
        const QString source = group ? QStringLiteral("group")
                               : src.value("type").toString() == "none" ? QStringLiteral("—")
                                                                         : QFileInfo(src.value("path").toString()).fileName();
        auto *it = new QTreeWidgetItem(m_layers, {(o.contains("parent") ? QStringLiteral("    ") : QString()) + o.value("name").toString(),
                                                  o.value("visible").toBool(true) ? QStringLiteral("✓") : QStringLiteral("—"),
                                                  QStringLiteral("%1%").arg(std::lround(o.value("opacity").toDouble(1) * 100)),
                                                  source});
        it->setData(0, Qt::UserRole, o.value("id").toString().toULongLong());
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        it->setCheckState(0, o.value("included").toBool(true) ? Qt::Checked : Qt::Unchecked);
        it->setToolTip(3, src.value("path").toString());
        if (m_engine->indexOfId(it->data(0, Qt::UserRole).toULongLong()) < 0) {
            it->setForeground(0, QColor(255, 180, 90));
            it->setToolTip(0, QStringLiteral("Not in the composition any more: recreated by the recall"));
        }
    }
    m_filling = false;
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
    QSet<quint64> excluded;
    for (const QJsonValue &v : m.layers)
        if (!v.toObject().value("included").toBool(true)) excluded.insert(v.toObject().value("id").toString().toULongLong());
    QJsonArray layers = m_engine->captureLayers();
    for (int k = 0; k < layers.size(); ++k) {
        QJsonObject o = layers[k].toObject();
        if (excluded.contains(o.value("id").toString().toULongLong())) {
            o["included"] = false;
            layers[k] = o;
        }
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
