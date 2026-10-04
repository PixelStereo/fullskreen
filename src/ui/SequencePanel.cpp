#include "SequencePanel.h"
#include "Engine.h"
#include "MemoryPanel.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

static QString memoryName(Engine *e, quint64 id)
{
    if (!id) return QStringLiteral("—");
    const int i = e->indexOfMemory(id);
    if (i < 0) return QStringLiteral("(memory gone)");
    const Engine::Memory m = e->memory(i);
    return QStringLiteral("%1  %2").arg(i + 1).arg(m.name);
}

// ---------------------------------------------------------------------------
// SequenceBar
// ---------------------------------------------------------------------------

SequenceBar::SequenceBar(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    setObjectName("sequenceBar");
    setStyleSheet("#sequenceBar { background:#202024; border-top:1px solid #34343a; }");
    setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 4, 8, 4);
    v->setSpacing(2);
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    m_sequence = new QComboBox;
    m_sequence->setToolTip(QStringLiteral("Sequence played by GO"));
    m_sequence->setMinimumWidth(110);
    m_back = new QPushButton(QStringLiteral("◀ BACK"));
    m_back->setToolTip(QStringLiteral("GO BACK: the previous step (Shift+Space)"));
    m_go = new QPushButton(QStringLiteral("GO ▶"));
    m_go->setToolTip(QStringLiteral("GO: the next step (Space)"));
    m_go->setStyleSheet("QPushButton { font-weight:bold; background:#2f6b3a; color:white; padding:4px 18px; }"
                        "QPushButton:disabled { background:#2a2f2b; color:#666; }");
    m_prev = new QLabel;
    m_current = new QLabel;
    m_next = new QLabel;
    m_prev->setStyleSheet("color:#8a8a90;");
    m_next->setStyleSheet("color:#8a8a90;");
    for (QLabel *l : {m_prev, m_current, m_next}) {
        l->setMinimumWidth(40);
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long names give way, not the buttons
        l->setTextFormat(Qt::RichText);
    }
    m_open = new QPushButton(QStringLiteral("Sequences…"));
    m_open->setToolTip(QStringLiteral("All the sequences and their steps, in a window that stays in front"));
    row->addWidget(m_sequence);
    row->addWidget(m_back);
    row->addWidget(m_go);
    row->addSpacing(6);
    row->addWidget(m_prev, 2);
    row->addWidget(new QLabel(QStringLiteral("›")));
    row->addWidget(m_current, 3);
    row->addWidget(new QLabel(QStringLiteral("›")));
    row->addWidget(m_next, 2);
    row->addWidget(m_open);
    v->addLayout(row);
    m_text = new QLabel;
    m_text->setWordWrap(true);
    m_text->setStyleSheet("color:#d8d8dc; font-style:italic;");
    v->addWidget(m_text);

    connect(m_go, &QPushButton::clicked, this, &SequenceBar::goRequested);
    connect(m_back, &QPushButton::clicked, this, &SequenceBar::backRequested);
    connect(m_open, &QPushButton::clicked, this, &SequenceBar::windowRequested);
    connect(m_sequence, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        if (!m_filling) m_engine->setCurrentSequence(i);
    });
    connect(m_engine, &Engine::sequencesChanged, this, &SequenceBar::refresh);
    connect(m_engine, &Engine::sequencePositionChanged, this, &SequenceBar::refresh);
    connect(m_engine, &Engine::memoriesChanged, this, &SequenceBar::refresh);
    refresh();
}

void SequenceBar::setModified(bool on)
{
    if (on == m_modified) return;
    m_modified = on;
    refresh();
}

void SequenceBar::refresh()
{
    m_filling = true;
    const int n = m_engine->sequenceCount(), cur = m_engine->currentSequence();
    m_sequence->clear();
    for (int i = 0; i < n; ++i) m_sequence->addItem(m_engine->sequence(i).name);
    if (n == 0) m_sequence->addItem(QStringLiteral("No sequence"));
    m_sequence->setCurrentIndex(std::max(0, cur));
    m_sequence->setEnabled(n > 0);
    m_filling = false;

    const Engine::Sequence s = m_engine->sequence(cur);
    const int pos = m_engine->sequencePosition(), next = m_engine->sequenceNext(), prev = m_engine->sequencePrevious();
    auto stepName = [&](int k) {
        if (k < 0 || k >= int(s.steps.size())) return QString();
        const quint64 id = s.steps[size_t(k)].memory;
        const int mi = m_engine->indexOfMemory(id);
        const QString name = !id ? QStringLiteral("—") : mi < 0 ? QStringLiteral("(memory gone)") : m_engine->memory(mi).name;
        return QStringLiteral("%1. %2").arg(k + 1).arg(name.toHtmlEscaped());
    };
    m_prev->setText(prev >= 0 ? stepName(prev) : QStringLiteral("—"));
    m_next->setText(next >= 0 ? stepName(next) : (s.steps.empty() ? QStringLiteral("—") : QStringLiteral("end")));
    if (pos >= 0) {
        // The step played; in another color once something was changed since
        const QString color = m_modified ? QStringLiteral("#ffb347") : theme::accent().name();
        m_current->setText(QStringLiteral("<span style='font-size:15px; font-weight:bold; color:%1'>%2</span>%3")
                               .arg(color, stepName(pos),
                                    m_modified ? QStringLiteral(" <span style='color:#ffb347'>(changed)</span>") : QString()));
        m_text->setText(s.steps[size_t(pos)].text.toHtmlEscaped());
    } else {
        m_current->setText(QStringLiteral("<span style='color:#8a8a90'>%1</span>")
                               .arg(s.steps.empty() ? QStringLiteral("no step — add them in Sequences…")
                                                    : QStringLiteral("ready: GO plays the first step")));
        m_text->setText(next >= 0 ? s.steps[size_t(next)].text.toHtmlEscaped() : QString());
    }
    m_text->setVisible(!m_text->text().isEmpty());
    m_go->setEnabled(next >= 0);
    m_back->setEnabled(prev >= 0);
}

// ---------------------------------------------------------------------------
// SequenceWindow
// ---------------------------------------------------------------------------

enum StepCol { ColNum, ColMemory, ColText };

SequenceWindow::SequenceWindow(Engine *engine, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint), m_engine(engine)
{
    setWindowTitle(QStringLiteral("Sequences"));
    resize(760, 460);
    auto *h = new QHBoxLayout(this);

    // Sequences
    auto *left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral("<b>Sequences</b>")));
    m_sequences = new QListWidget;
    m_sequences->setToolTip(QStringLiteral("The selected sequence is the one GO plays · double-click to rename"));
    left->addWidget(m_sequences, 1);
    auto *sb = new QHBoxLayout;
    m_add = new QPushButton(QStringLiteral("+"));
    m_add->setToolTip(QStringLiteral("New sequence"));
    m_dup = new QPushButton(QStringLiteral("Duplicate"));
    m_del = new QPushButton(QStringLiteral("−"));
    m_del->setToolTip(QStringLiteral("Delete the sequence"));
    for (QPushButton *b : {m_add, m_dup, m_del}) sb->addWidget(b);
    left->addLayout(sb);
    m_loop = new QCheckBox(QStringLiteral("Loop: GO on the last step plays the first"));
    left->addWidget(m_loop);
    h->addLayout(left, 1);

    // Steps
    auto *right = new QVBoxLayout;
    right->addWidget(new QLabel(QStringLiteral("<b>Steps</b> — drag a memory onto a step (or below the last one to add one)")));
    m_steps = new QTableWidget(0, 3);
    m_steps->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Memory"), QStringLiteral("Text")});
    m_steps->verticalHeader()->setVisible(false);
    m_steps->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_steps->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_steps->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    m_steps->setColumnWidth(ColNum, 40);
    m_steps->setColumnWidth(ColMemory, 200);
    m_steps->setAcceptDrops(true);
    m_steps->viewport()->setAcceptDrops(true);
    m_steps->viewport()->installEventFilter(this);
    m_steps->setContextMenuPolicy(Qt::CustomContextMenu);
    m_steps->setToolTip(QStringLiteral("Double-click a number to play that step · double-click a text to edit it · "
                                       "right-click: choose the memory"));
    right->addWidget(m_steps, 1);
    auto *stb = new QHBoxLayout;
    m_stepAdd = new QPushButton(QStringLiteral("+ Step"));
    m_stepDel = new QPushButton(QStringLiteral("− Delete"));
    m_up = new QPushButton(QStringLiteral("▲"));
    m_down = new QPushButton(QStringLiteral("▼"));
    for (QPushButton *b : {m_stepAdd, m_stepDel, m_up, m_down}) stb->addWidget(b);
    stb->addStretch();
    right->addLayout(stb);
    h->addLayout(right, 3);

    auto current = [this] { return m_engine->currentSequence(); };
    connect(m_sequences, &QListWidget::currentRowChanged, this, [this](int r) {
        if (!m_filling && r >= 0) m_engine->setCurrentSequence(r);
    });
    connect(m_sequences, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_filling) return;
        const int i = m_sequences->row(it);
        Engine::Sequence s = m_engine->sequence(i);
        if (s.name == it->text()) return;
        s.name = it->text();
        m_engine->setSequence(i, s);
        emit edited();
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        Engine::Sequence s;
        s.name = QStringLiteral("Sequence %1").arg(m_engine->sequenceCount() + 1);
        m_engine->setCurrentSequence(m_engine->addSequence(s));
        emit edited();
    });
    connect(m_dup, &QPushButton::clicked, this, [this, current] {
        if (current() < 0) return;
        Engine::Sequence s = m_engine->sequence(current());
        s.name += QStringLiteral(" copy");
        m_engine->setCurrentSequence(m_engine->addSequence(s, current() + 1));
        emit edited();
    });
    connect(m_del, &QPushButton::clicked, this, [this, current] {
        if (current() < 0) return;
        m_engine->removeSequence(current());
        emit edited();
    });
    connect(m_loop, &QCheckBox::toggled, this, [this, current](bool on) {
        if (m_filling || current() < 0) return;
        Engine::Sequence s = m_engine->sequence(current());
        s.loop = on;
        m_engine->setSequence(current(), s);
        emit edited();
    });
    connect(m_steps, &QTableWidget::itemChanged, this, [this, current](QTableWidgetItem *it) {
        if (m_filling || it->column() != ColText || current() < 0) return;
        Engine::Sequence s = m_engine->sequence(current());
        if (it->row() >= int(s.steps.size())) return;
        s.steps[size_t(it->row())].text = it->text();
        m_engine->setSequence(current(), s);
        emit edited();
    });
    connect(m_steps, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (col == ColNum) emit goToRequested(row);
    });
    connect(m_steps, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = m_steps->rowAt(pos.y());
        QMenu menu;
        QMenu *mem = menu.addMenu(row >= 0 ? QStringLiteral("Memory of this step") : QStringLiteral("New step with memory"));
        for (int i = 0; i < m_engine->memoryCount(); ++i) {
            const Engine::Memory m = m_engine->memory(i);
            mem->addAction(QStringLiteral("%1  %2").arg(i + 1).arg(m.name), this, [this, row, id = m.id] {
                if (row >= 0) setStepMemory(row, id);
                else addStep(-1, id);
            });
        }
        mem->setEnabled(!mem->isEmpty());
        if (row >= 0) {
            menu.addAction(QStringLiteral("Play this step"), this, [this, row] { emit goToRequested(row); });
            menu.addAction(QStringLiteral("Delete"), this, &SequenceWindow::removeSteps);
        }
        menu.exec(m_steps->viewport()->mapToGlobal(pos));
    });
    connect(m_stepAdd, &QPushButton::clicked, this, [this] {
        const int r = m_steps->currentRow();
        addStep(r >= 0 ? r + 1 : -1, 0);
    });
    connect(m_stepDel, &QPushButton::clicked, this, &SequenceWindow::removeSteps);
    connect(m_up, &QPushButton::clicked, this, [this] { moveStep(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveStep(+1); });
    connect(m_engine, &Engine::sequencesChanged, this, &SequenceWindow::refresh);
    connect(m_engine, &Engine::sequencePositionChanged, this, &SequenceWindow::refresh);
    connect(m_engine, &Engine::memoriesChanged, this, &SequenceWindow::refresh);
    refresh();
}

void SequenceWindow::refresh()
{
    m_filling = true;
    const int n = m_engine->sequenceCount(), cur = m_engine->currentSequence();
    m_sequences->clear();
    for (int i = 0; i < n; ++i) {
        auto *it = new QListWidgetItem(m_engine->sequence(i).name, m_sequences);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
    }
    m_sequences->setCurrentRow(cur);
    const Engine::Sequence s = m_engine->sequence(cur);
    m_loop->setChecked(s.loop);
    for (QWidget *w : std::initializer_list<QWidget *>{m_dup, m_del, m_loop, m_stepDel, m_up, m_down}) // steps: a first one makes a sequence
        w->setEnabled(cur >= 0);
    const int keep = m_steps->currentRow();
    m_steps->setRowCount(int(s.steps.size()));
    const int pos = m_engine->sequencePosition();
    for (int k = 0; k < int(s.steps.size()); ++k) {
        const Engine::SequenceStep &st = s.steps[size_t(k)];
        auto *num = new QTableWidgetItem(QString::number(k + 1));
        num->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        num->setTextAlignment(Qt::AlignCenter);
        auto *mem = new QTableWidgetItem(memoryName(m_engine, st.memory));
        mem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (st.memory && m_engine->indexOfMemory(st.memory) < 0) mem->setForeground(QColor(255, 120, 100));
        auto *text = new QTableWidgetItem(st.text);
        m_steps->setItem(k, ColNum, num);
        m_steps->setItem(k, ColMemory, mem);
        m_steps->setItem(k, ColText, text);
        if (k == pos) // the step played
            for (QTableWidgetItem *it : {num, mem, text}) {
                QFont f = it->font();
                f.setBold(true);
                it->setFont(f);
                it->setForeground(theme::accent());
            }
    }
    if (keep >= 0 && keep < m_steps->rowCount()) m_steps->setCurrentCell(keep, ColMemory);
    m_filling = false;
}

void SequenceWindow::addStep(int at, quint64 memory)
{
    int cur = m_engine->currentSequence();
    if (cur < 0) { // a first sequence for it
        Engine::Sequence s;
        s.name = QStringLiteral("Sequence 1");
        cur = m_engine->addSequence(s);
        m_engine->setCurrentSequence(cur);
    }
    Engine::Sequence s = m_engine->sequence(cur);
    if (at < 0 || at > int(s.steps.size())) at = int(s.steps.size());
    s.steps.insert(s.steps.begin() + at, Engine::SequenceStep{memory, QString()});
    // The step played keeps being the same one
    const int pos = m_engine->sequencePosition();
    m_engine->setSequence(cur, s);
    if (pos >= at) m_engine->setSequencePosition(pos + 1);
    m_steps->setCurrentCell(at, ColMemory);
    emit edited();
}

void SequenceWindow::setStepMemory(int step, quint64 memory)
{
    const int cur = m_engine->currentSequence();
    Engine::Sequence s = m_engine->sequence(cur);
    if (step < 0 || step >= int(s.steps.size())) return;
    s.steps[size_t(step)].memory = memory;
    m_engine->setSequence(cur, s);
    emit edited();
}

void SequenceWindow::removeSteps()
{
    const int cur = m_engine->currentSequence();
    Engine::Sequence s = m_engine->sequence(cur);
    QList<int> rows;
    for (const QModelIndex &i : m_steps->selectionModel()->selectedRows()) rows << i.row();
    if (rows.isEmpty() && m_steps->currentRow() >= 0) rows << m_steps->currentRow();
    if (rows.isEmpty()) return;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    int pos = m_engine->sequencePosition();
    for (int r : rows) {
        if (r < 0 || r >= int(s.steps.size())) continue;
        s.steps.erase(s.steps.begin() + r);
        if (pos == r) pos = r - 1; // the step played is gone: as if the one before was
        else if (pos > r) --pos;
    }
    m_engine->setSequence(cur, s);
    m_engine->setSequencePosition(pos);
    emit edited();
}

void SequenceWindow::moveStep(int delta)
{
    const int cur = m_engine->currentSequence();
    Engine::Sequence s = m_engine->sequence(cur);
    const int r = m_steps->currentRow(), to = r + delta;
    if (r < 0 || to < 0 || to >= int(s.steps.size())) return;
    std::swap(s.steps[size_t(r)], s.steps[size_t(to)]);
    int pos = m_engine->sequencePosition();
    if (pos == r) pos = to;
    else if (pos == to) pos = r;
    m_engine->setSequence(cur, s);
    m_engine->setSequencePosition(pos);
    m_steps->setCurrentCell(to, ColMemory);
    emit edited();
}

// A memory dragged from the list of the memories: onto a step, it becomes its memory; below the last one, a new step
bool SequenceWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o != m_steps->viewport()) return QWidget::eventFilter(o, e);
    if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove) {
        auto *d = static_cast<QDragMoveEvent *>(e);
        if (!d->mimeData()->hasFormat(kMemoryMime)) return false;
        d->acceptProposedAction();
        return true;
    }
    if (e->type() == QEvent::Drop) {
        auto *d = static_cast<QDropEvent *>(e);
        if (!d->mimeData()->hasFormat(kMemoryMime)) return false;
        const quint64 id = d->mimeData()->data(kMemoryMime).toULongLong();
        const int row = m_steps->rowAt(d->position().toPoint().y());
        if (row >= 0) setStepMemory(row, id);
        else addStep(-1, id);
        d->acceptProposedAction();
        return true;
    }
    return QWidget::eventFilter(o, e);
}
