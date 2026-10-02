#include "LayerTable.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

enum Col { ColVisible, ColName, ColSource, ColEffects, ColOpacity, ColBlend, ColPlayback, ColCount };

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
    auto *title = new QLabel(QStringLiteral("<b>Calques</b>"));
    auto *add = barButton(QStringLiteral("+"), QStringLiteral("Nouveau calque"));
    add->setPopupMode(QToolButton::InstantPopup);
    m_addMenu = new QMenu(add);
    add->setMenu(m_addMenu);
    auto *remove = barButton(QStringLiteral("−"), QStringLiteral("Supprimer le calque (Suppr)"));
    auto *dup = barButton(QStringLiteral("⧉"), QStringLiteral("Dupliquer le calque (Ctrl+D)"));
    auto *up = barButton(QStringLiteral("▲"), QStringLiteral("Monter (Ctrl+])"));
    auto *down = barButton(QStringLiteral("▼"), QStringLiteral("Descendre (Ctrl+[)"));
    bar->addWidget(title);
    bar->addSpacing(12);
    for (auto *b : {add, remove, dup, up, down}) bar->addWidget(b);
    bar->addStretch();
    auto *hint = new QLabel(QStringLiteral("Le calque du haut s'affiche au-dessus · glissez des fichiers ici pour créer des calques"));
    hint->setStyleSheet("color:#888; font-size:11px;");
    bar->addWidget(hint);
    v->addLayout(bar);

    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({QString(), QStringLiteral("Calque"), QStringLiteral("Source"),
                                        QStringLiteral("Effets"), QStringLiteral("Opacité"), QStringLiteral("Fusion"),
                                        QStringLiteral("Lecture")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setFocusPolicy(Qt::StrongFocus);
    auto *h = m_table->horizontalHeader();
    h->setHighlightSections(false);
    h->setSectionResizeMode(ColVisible, QHeaderView::Fixed);
    m_table->setColumnWidth(ColVisible, 30);
    h->setSectionResizeMode(ColName, QHeaderView::Interactive);
    m_table->setColumnWidth(ColName, 220);
    h->setSectionResizeMode(ColSource, QHeaderView::Stretch);
    h->setSectionResizeMode(ColEffects, QHeaderView::Interactive);
    m_table->setColumnWidth(ColEffects, 220);
    h->setSectionResizeMode(ColOpacity, QHeaderView::Fixed);
    m_table->setColumnWidth(ColOpacity, 170);
    h->setSectionResizeMode(ColBlend, QHeaderView::Fixed);
    m_table->setColumnWidth(ColBlend, 90);
    h->setSectionResizeMode(ColPlayback, QHeaderView::Fixed);
    m_table->setColumnWidth(ColPlayback, 160);
    v->addWidget(m_table, 1);

    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int prev, int) {
        if (!m_updating && row != prev) emit currentRowChanged(row);
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_updating || it->column() != ColVisible) return;
        emit visibilityToggled(it->row(), it->checkState() == Qt::Checked);
    });
    connect(add, &QToolButton::clicked, add, &QToolButton::showMenu);
    connect(remove, &QToolButton::clicked, this, &LayerTable::removeClicked);
    connect(dup, &QToolButton::clicked, this, &LayerTable::duplicateClicked);
    connect(up, &QToolButton::clicked, this, [this] { emit moveClicked(-1); });
    connect(down, &QToolButton::clicked, this, [this] { emit moveClicked(+1); });
}

QWidget *LayerTable::table() const { return m_table; }

int LayerTable::currentRow() const { return m_table->currentRow(); }

void LayerTable::setCurrentRow(int row)
{
    m_updating = true;
    if (row < 0) {
        m_table->setCurrentCell(-1, -1);
        m_table->clearSelection();
    } else {
        m_table->setCurrentCell(row, ColName);
    }
    m_updating = false;
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

    QTableWidgetItem *name = text(ColName, row.tag + QStringLiteral("  ") + row.name, row.name);
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
    QTableWidgetItem *src = text(ColSource, row.source);
    src->setForeground(row.error ? QColor(255, 110, 95) : QColor(170, 170, 175));
    name->setForeground(row.error ? QColor(255, 110, 95) : QColor(230, 230, 233));
    text(ColEffects, row.effects)->setForeground(QColor(170, 170, 175));
    text(ColBlend, row.blend);
    text(ColPlayback, row.playback)->setFont(QFont(QStringLiteral("monospace")));

    // Opacité : curseur directement dans la ligne
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
            label->setText(QStringLiteral("%1 %").arg(v));
            if (m_updating) return;
            // Retrouve la ligne du curseur (les lignes peuvent avoir changé depuis sa création)
            for (int k = 0; k < m_table->rowCount(); ++k) {
                QWidget *c = m_table->cellWidget(k, ColOpacity);
                if (c && c->isAncestorOf(slider)) {
                    emit opacityEdited(k, v / 100.0);
                    break;
                }
            }
        });
    }
    const int pct = int(std::lround(row.opacity * 100));
    if (!slider->isSliderDown() && slider->value() != pct) {
        QSignalBlocker b(slider);
        slider->setValue(pct);
    }
    label->setText(QStringLiteral("%1 %").arg(slider->value()));
}

void LayerTable::setRows(const std::vector<Row> &rows)
{
    m_updating = true;
    const int keep = m_table->currentRow();
    if (m_table->rowCount() != int(rows.size())) {
        m_table->clearContents();
        m_table->setRowCount(int(rows.size()));
    }
    for (int r = 0; r < int(rows.size()); ++r) updateRow(r, rows[size_t(r)]);
    if (keep >= 0 && keep < int(rows.size())) m_table->setCurrentCell(keep, ColName);
    m_updating = false;
}
