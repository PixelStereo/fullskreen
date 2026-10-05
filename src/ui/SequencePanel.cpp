#include "SequencePanel.h"
#include "Engine.h"
#include "MemoryPanel.h"
#include "TimelinePanel.h"
#include "Widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

static QString memoryName(Engine *e, quint64 id)
{
    if (!id) return QStringLiteral("—");
    const int i = e->indexOfMemory(id);
    if (i < 0) return QStringLiteral("(memory gone)");
    const Engine::Memory m = e->memory(i);
    return QStringLiteral("%1  %2").arg(i + 1).arg(m.name);
}

// What a timeline step does
static QString actionName(const Engine::SequenceStep &st)
{
    using A = Engine::AnimAction;
    switch (st.action) {
    case A::Play: return QStringLiteral("▶ Play");
    case A::Pause: return QStringLiteral("❚❚ Pause");
    case A::Stop: return QStringLiteral("■ Stop");
    case A::Rewind: return QStringLiteral("⏮ Rewind");
    case A::Seek: return QStringLiteral("Seek %1 s").arg(st.seekTime, 0, 'f', 2);
    case A::Speed: return QStringLiteral("Speed × %1").arg(st.speed, 0, 'f', 2);
    case A::LoopMode: {
        const QString n = st.repeat > 0 ? QStringLiteral(" × %1").arg(st.repeat) : QStringLiteral(" ∞");
        return st.loop == Engine::AnimLoop::Once ? QStringLiteral("Loop: once")
             : st.loop == Engine::AnimLoop::PingPong ? QStringLiteral("Loop: ping-pong") + n
                                                    : QStringLiteral("Loop") + n;
    }
    }
    return {};
}

static QString timelineName(Engine *e, quint64 id)
{
    const int i = e->indexOfAnimation(id);
    return i < 0 ? QStringLiteral("(timeline gone)") : e->animation(i).name;
}

// What a step does: its memory (with its number), or its timeline and the action on it
static QString stepTarget(Engine *e, const Engine::SequenceStep &st, bool numbered = true)
{
    if (st.timeline) return QStringLiteral("≋ %1 · %2").arg(timelineName(e, st.timeline), actionName(st));
    if (numbered) return memoryName(e, st.memory);
    const int mi = e->indexOfMemory(st.memory);
    return !st.memory ? QStringLiteral("—") : mi < 0 ? QStringLiteral("(memory gone)") : e->memory(mi).name;
}

static bool targetGone(Engine *e, const Engine::SequenceStep &st)
{
    return st.timeline ? e->indexOfAnimation(st.timeline) < 0 : st.memory && e->indexOfMemory(st.memory) < 0;
}

static const char *kChanged = "#ffb347"; // something changed since the step was played
static const QColor kLive(76, 217, 100);  // a memory running
static const QColor kPreWait(224, 180, 58);  // waiting before the memory
static const QColor kPostWait(86, 156, 230); // waiting before the next step's GO

// Seconds as the cue list shows them: 2.5 · 1:05.0
static QString seconds(double t)
{
    t = std::max(0.0, t);
    if (t < 60) return QString::number(t, 'f', 1);
    const int m = int(t / 60);
    return QStringLiteral("%1:%2").arg(m).arg(t - m * 60, 4, 'f', 1, QLatin1Char('0'));
}

static QString continueName(Engine::StepContinue c)
{
    switch (c) {
    case Engine::StepContinue::Follow: return QStringLiteral("↓ Follow");
    case Engine::StepContinue::AutoFollow: return QStringLiteral("⤓ Auto-follow");
    default: return QStringLiteral("Wait (GO)");
    }
}

// Where a step on its way is in each of its times: -1 not there yet (or none), 0..1 on the way, 1 over
struct RunPhases {
    double pre = -1, action = -1, post = -1;
    double preElapsed = 0, actionElapsed = 0, postElapsed = 0;
};
static RunPhases phasesOf(const Engine::StepRun &r)
{
    RunPhases ph;
    auto frac = [](double e, double len) { return len > 0 ? std::clamp(e / len, 0.0, 1.0) : (e >= 0 ? 1.0 : 0.0); };
    ph.preElapsed = std::min(r.elapsed, r.preWait);
    ph.pre = r.preWait > 0 ? frac(r.elapsed, r.preWait) : -1;
    if (r.fired) {
        ph.actionElapsed = std::clamp(r.elapsed - r.preWait, 0.0, r.duration);
        ph.action = frac(r.elapsed - r.preWait, r.duration);
    }
    const double at = r.continueAt();
    if (at >= 0 && r.fired) {
        const double start = at - r.postWait;
        if (r.elapsed >= start) {
            ph.postElapsed = std::clamp(r.elapsed - start, 0.0, r.postWait);
            ph.post = frac(r.elapsed - start, r.postWait);
        }
    }
    return ph;
}

// ---------------------------------------------------------------------------
// RecallProgressBar
// ---------------------------------------------------------------------------
RecallProgressBar::RecallProgressBar(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    setFixedHeight(5);
    auto *t = new QTimer(this);
    t->setInterval(33);
    connect(t, &QTimer::timeout, this, &RecallProgressBar::poll);
    t->start();
}

void RecallProgressBar::poll()
{
    const Engine::RecallProgress r = m_engine->recallProgress();
    const double f = r.memory ? r.fraction() : 0.0;
    const bool running = r.running();
    if (std::abs(f - m_fraction) < 1e-4 && running == m_running) return;
    m_fraction = f;
    m_running = running;
    const int mi = m_engine->indexOfMemory(r.memory);
    setToolTip(mi < 0 ? QString()
                      : QStringLiteral("%1. %2 — %3 / %4 s").arg(mi + 1).arg(m_engine->memory(mi).name)
                            .arg(std::min(r.elapsed, r.total), 0, 'f', 1).arg(r.total, 0, 'f', 1));
    update();
}

void RecallProgressBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(40, 40, 44));
    if (m_fraction <= 0) return;
    QColor c = m_running ? kLive : QColor(kLive.red(), kLive.green(), kLive.blue(), 90); // done: dimmed
    p.fillRect(QRectF(0, 0, width() * m_fraction, height()), c);
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
    m_prevText = new QLabel;
    m_currentText = new QLabel;
    m_nextText = new QLabel;
    m_prev->setStyleSheet("color:#8a8a90;");
    m_next->setStyleSheet("color:#8a8a90;");
    for (QLabel *l : {m_prev, m_current, m_next, m_prevText, m_currentText, m_nextText}) {
        l->setMinimumWidth(40);
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long names give way, not the buttons
        l->setTextFormat(Qt::RichText);
    }
    for (QLabel *l : {m_prevText, m_nextText}) {
        l->setWordWrap(true);
        l->setStyleSheet("color:#7a7a80; font-style:italic; font-size:11px;");
    }
    m_currentText->setWordWrap(true);
    m_currentText->setStyleSheet("color:#e4e4e8; font-style:italic;");
    m_stop = new QPushButton(QStringLiteral("■"));
    m_stop->setToolTip(QStringLiteral("Stop the waits: the pre-waits and follows still to come are dropped "
                                      "(the memories already running go on)"));
    m_stop->setEnabled(false);
    m_stop->setFixedWidth(34);
    m_stop->setStyleSheet("QPushButton:enabled { background:#8a2a24; color:white; }");
    m_wait = new QLabel;
    m_wait->setTextFormat(Qt::RichText);
    m_wait->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_open = new QPushButton(QStringLiteral("Sequences…"));
    m_open->setToolTip(QStringLiteral("All the sequences and their steps, in a window that stays in front"));
    row->addWidget(m_sequence);
    row->addWidget(m_back);
    row->addWidget(m_go);
    row->addWidget(m_stop);
    row->addSpacing(6);
    // Previous, current and next steps, each with its text under it
    auto column = [](QLabel *name, QLabel *text) {
        auto *c = new QVBoxLayout;
        c->setSpacing(0);
        c->addWidget(name);
        c->addWidget(text);
        c->addStretch();
        return c;
    };
    row->addLayout(column(m_prev, m_prevText), 2);
    row->addWidget(new QLabel(QStringLiteral("›")), 0, Qt::AlignTop);
    row->addLayout(column(m_current, m_currentText), 3);
    row->addWidget(new QLabel(QStringLiteral("›")), 0, Qt::AlignTop);
    row->addLayout(column(m_next, m_nextText), 2);
    auto *timelines = new QPushButton(QStringLiteral("Timelines…"));
    timelines->setToolTip(QStringLiteral("The timelines: curves and oscillators driving the layers, played by the sequences"));
    auto *windows = new QVBoxLayout; // stacked: the steps keep their room
    windows->setSpacing(2);
    for (QPushButton *b : {m_open, timelines}) {
        b->setStyleSheet("QPushButton { padding:1px 8px; }");
        windows->addWidget(b);
    }
    row->addLayout(windows);
    connect(timelines, &QPushButton::clicked, this, &SequenceBar::timelinesRequested);
    v->addLayout(row);
    v->addWidget(m_wait);
    m_wait->hide();
    auto *progress = new RecallProgressBar(m_engine);
    progress->setToolTip(QStringLiteral("The memory running, and where it is in its time"));
    v->addWidget(progress);

    connect(m_go, &QPushButton::clicked, this, &SequenceBar::goRequested);
    connect(m_back, &QPushButton::clicked, this, &SequenceBar::backRequested);
    connect(m_stop, &QPushButton::clicked, this, &SequenceBar::stopRequested);
    connect(m_open, &QPushButton::clicked, this, &SequenceBar::windowRequested);
    auto *waits = new QTimer(this);
    waits->setInterval(50);
    connect(waits, &QTimer::timeout, this, &SequenceBar::pollWaits);
    waits->start();
    connect(m_sequence, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        if (!m_filling) m_engine->setCurrentSequence(i);
    });
    connect(m_engine, &Engine::sequencesChanged, this, &SequenceBar::refresh);
    connect(m_engine, &Engine::sequencePositionChanged, this, &SequenceBar::refresh);
    connect(m_engine, &Engine::memoriesChanged, this, &SequenceBar::refresh);
    connect(m_engine, &Engine::animationsChanged, this, &SequenceBar::refresh);
    refresh();
}

// What is waiting: a step in its pre-wait, or the next step's GO after a follow / auto-follow
void SequenceBar::pollWaits()
{
    const std::vector<Engine::StepRun> runs = m_engine->sequenceRuns();
    const Engine::Sequence s = m_engine->sequence(m_engine->currentSequence());
    QStringList parts;
    for (const Engine::StepRun &r : runs) {
        if (!r.fired) {
            parts << QStringLiteral("<span style='color:%1'>%2. pre-wait %3 s</span>")
                         .arg(kPreWait.name()).arg(r.step + 1).arg(seconds(r.preWait - r.elapsed));
            continue;
        }
        const double at = r.continueAt();
        if (at < 0 || r.continued) continue;
        const int n = int(s.steps.size());
        const int next = r.step + 1 < n ? r.step + 1 : s.loop ? 0 : -1;
        if (next < 0) continue;
        parts << QStringLiteral("<span style='color:%1'>%2 → %3 in %4 s</span>")
                     .arg(kPostWait.name(), r.next == Engine::StepContinue::Follow ? QStringLiteral("follow") : QStringLiteral("auto-follow"))
                     .arg(next + 1)
                     .arg(seconds(at - r.elapsed));
    }
    const QString text = parts.join(QStringLiteral(" · "));
    if (text != m_wait->text()) m_wait->setText(text);
    m_wait->setVisible(!text.isEmpty());
    m_stop->setEnabled(!text.isEmpty());
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
        const QString name = stepTarget(m_engine, s.steps[size_t(k)], false);
        return QStringLiteral("%1. %2").arg(k + 1).arg(name.toHtmlEscaped());
    };
    auto stepText = [&](int k) {
        return k >= 0 && k < int(s.steps.size()) ? s.steps[size_t(k)].text.toHtmlEscaped() : QString();
    };
    m_prev->setText(prev >= 0 ? stepName(prev) : QStringLiteral("—"));
    m_next->setText(next >= 0 ? stepName(next) : (s.steps.empty() ? QStringLiteral("—") : QStringLiteral("end")));
    m_prevText->setText(stepText(prev));
    m_nextText->setText(stepText(next));
    if (pos >= 0) {
        // The step played; in another color once something was changed since
        const QString color = m_modified ? QString::fromLatin1(kChanged) : kLive.name();
        m_current->setText(QStringLiteral("<span style='font-size:15px; font-weight:bold; color:%1'>%2</span>%3")
                               .arg(color, stepName(pos),
                                    m_modified ? QStringLiteral(" <span style='color:%1'>(changed)</span>").arg(QLatin1String(kChanged))
                                               : QString()));
        m_currentText->setText(stepText(pos));
    } else {
        m_current->setText(QStringLiteral("<span style='color:#8a8a90'>%1</span>")
                               .arg(s.steps.empty() ? QStringLiteral("no step — add them in Sequences…")
                                                    : QStringLiteral("ready: GO plays the first step")));
        m_currentText->clear();
    }
    for (QLabel *l : {m_prevText, m_currentText, m_nextText}) l->setVisible(!l->text().isEmpty());
    m_go->setEnabled(next >= 0);
    m_back->setEnabled(prev >= 0);
}

// ---------------------------------------------------------------------------
// SequenceWindow
// ---------------------------------------------------------------------------

enum StepCol { ColNum, ColMemory, ColPre, ColAction, ColPost, ColNext, ColText, ColCount };

// The time columns (pre-wait, action, post-wait) filling as their time goes by, the continuation as a word;
// the waits are edited with the app's number field, the continuation with a menu
class StepDelegate : public QStyledItemDelegate
{
public:
    StepDelegate(SequenceWindow *w) : QStyledItemDelegate(w), m_w(w) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        if (idx.column() == ColPre || idx.column() == ColPost) {
            auto *b = new NumberBox(parent);
            b->setRange(0, 3600);
            b->setDecimals(2);
            b->setSingleStep(0.1);
            b->setSuffix(QStringLiteral(" s"));
            return b;
        }
        if (idx.column() == ColNext) {
            auto *c = new QComboBox(parent);
            for (auto k : {Engine::StepContinue::Wait, Engine::StepContinue::Follow, Engine::StepContinue::AutoFollow})
                c->addItem(continueName(k), int(k));
            connect(c, qOverload<int>(&QComboBox::activated), this, [this, c] {
                auto *self = const_cast<StepDelegate *>(this);
                emit self->commitData(c);
                emit self->closeEditor(c);
            });
            return c;
        }
        return QStyledItemDelegate::createEditor(parent, opt, idx);
    }
    void setEditorData(QWidget *e, const QModelIndex &idx) const override
    {
        if (auto *b = qobject_cast<QDoubleSpinBox *>(e)) b->setValue(idx.data(Qt::EditRole).toDouble());
        else if (auto *c = qobject_cast<QComboBox *>(e)) {
            c->setCurrentIndex(std::max(0, c->findData(idx.data(Qt::EditRole).toInt())));
            c->showPopup();
        } else QStyledItemDelegate::setEditorData(e, idx);
    }
    void setModelData(QWidget *e, QAbstractItemModel *m, const QModelIndex &idx) const override
    {
        if (auto *b = qobject_cast<QDoubleSpinBox *>(e)) m->setData(idx, b->value(), Qt::EditRole);
        else if (auto *c = qobject_cast<QComboBox *>(e)) m->setData(idx, c->currentData().toInt(), Qt::EditRole);
        else QStyledItemDelegate::setModelData(e, m, idx);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        const int col = idx.column();
        if (col != ColPre && col != ColAction && col != ColPost && col != ColNext) {
            QStyledItemDelegate::paint(p, opt, idx);
            return;
        }
        QStyleOptionViewItem o(opt);
        initStyleOption(&o, idx);
        o.text.clear();
        const QWidget *w = opt.widget;
        (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &o, p, w);
        const auto next = Engine::StepContinue(idx.sibling(idx.row(), ColNext).data(Qt::EditRole).toInt());
        const bool selected = opt.state & QStyle::State_Selected;
        QColor ink = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        if (col == ColNext) {
            if (next == Engine::StepContinue::Wait) ink.setAlphaF(0.45);
            p->setPen(ink);
            p->drawText(opt.rect.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, continueName(next));
            return;
        }
        const double value = idx.data(Qt::EditRole).toDouble();
        // The step on its way (the latest, should it be there twice in a loop)
        const Engine::StepRun *run = nullptr;
        for (const Engine::StepRun &r : m_w->m_runs)
            if (r.step == idx.row()) run = &r;
        double frac = -1, elapsed = 0;
        QColor bar = kLive;
        if (run) {
            const RunPhases ph = phasesOf(*run);
            if (col == ColPre) frac = ph.pre, elapsed = ph.preElapsed, bar = kPreWait;
            else if (col == ColAction) frac = ph.action, elapsed = ph.actionElapsed;
            else frac = ph.post, elapsed = ph.postElapsed, bar = kPostWait;
        }
        const QRectF r = QRectF(opt.rect).adjusted(2, 3, -2, -3);
        if (frac >= 0) {
            QColor back = bar;
            back.setAlphaF(0.18);
            p->fillRect(r, back);
            QColor fill = bar;
            fill.setAlphaF(frac >= 1 ? 0.35 : 0.75);
            p->fillRect(QRectF(r.left(), r.top(), r.width() * frac, r.height()), fill);
        }
        // The post-wait only counts with a follow; a time of zero is quiet
        const bool unused = col == ColPost && next == Engine::StepContinue::Wait;
        if (unused || (value == 0 && frac < 0)) ink.setAlphaF(0.4);
        QFont f = opt.font;
        QString text = value < 0 ? QStringLiteral("∞") : seconds(value);
        if (frac >= 0 && frac < 1) { // on its way: the time gone by, counting up
            f.setBold(true);
            text = seconds(elapsed);
            ink = selected ? opt.palette.color(QPalette::HighlightedText) : QColor(Qt::white);
        }
        p->setFont(f);
        p->setPen(ink);
        p->drawText(opt.rect.adjusted(4, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight, text);
    }

private:
    SequenceWindow *m_w;
};

SequenceWindow::SequenceWindow(Engine *engine, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint), m_engine(engine)
{
    setWindowTitle(QStringLiteral("Sequences"));
    resize(1020, 460);
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
    m_loop = new FlagBox(QStringLiteral("Loop: GO on the last step plays the first"));
    left->addWidget(m_loop);
    h->addLayout(left, 1);

    // Steps
    auto *right = new QVBoxLayout;
    right->addWidget(new QLabel(QStringLiteral("<b>Steps</b> — drag a memory or a timeline onto a step (or below the last one to add one)")));
    m_status = new QLabel;
    m_status->setTextFormat(Qt::RichText);
    right->addWidget(m_status);
    right->addWidget(new RecallProgressBar(m_engine));
    m_steps = new QTableWidget(0, ColCount);
    m_steps->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Memory / Timeline"), QStringLiteral("Pre-wait"),
                                        QStringLiteral("Action"), QStringLiteral("Post-wait"), QStringLiteral("Continue"),
                                        QStringLiteral("Text")});
    m_steps->horizontalHeaderItem(ColPre)->setToolTip(QStringLiteral("Seconds between the step's GO and its memory"));
    m_steps->horizontalHeaderItem(ColAction)->setToolTip(QStringLiteral("The memory's time: its fade, or the longest time of its own values\n"
                                                                         "A timeline played: the time it plays, when it ends (endless: 0)"));
    m_steps->horizontalHeaderItem(ColPost)->setToolTip(
        QStringLiteral("Follow: seconds after the memory starts before the next step's GO\n"
                       "Auto-follow: seconds after the memory ends before the next step's GO\n"
                       "Wait: not used"));
    m_steps->horizontalHeaderItem(ColNext)->setToolTip(
        QStringLiteral("Wait: the next step waits for GO (button, Space, OSC)\n"
                       "Follow: the next step goes once this one is triggered (plus the post-wait), without waiting "
                       "for its memory to end\n"
                       "Auto-follow: the next step goes once this one's memory is over (plus the post-wait)"));
    m_steps->setItemDelegate(new StepDelegate(this));
    for (int c : {ColPre, ColAction, ColPost}) m_steps->setColumnWidth(c, 70);
    m_steps->setColumnWidth(ColNext, 110);
    m_steps->verticalHeader()->setVisible(false);
    m_steps->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_steps->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_steps->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    m_steps->setColumnWidth(ColNum, 40);
    m_steps->setColumnWidth(ColMemory, 220);
    m_steps->setAcceptDrops(true);
    m_steps->viewport()->setAcceptDrops(true);
    m_steps->viewport()->installEventFilter(this);
    m_steps->setContextMenuPolicy(Qt::CustomContextMenu);
    m_steps->setToolTip(QStringLiteral("Double-click a number to play that step · double-click a wait, a continuation "
                                       "or a text to edit it · double-click a timeline step: its action · right-click: "
                                       "choose the memory or the timeline"));
    right->addWidget(m_steps, 1);
    auto *stb = new QHBoxLayout;
    m_stepAdd = new QPushButton(QStringLiteral("+ Step"));
    m_stepDel = new QPushButton(QStringLiteral("− Delete"));
    m_up = new QPushButton(QStringLiteral("▲"));
    m_down = new QPushButton(QStringLiteral("▼"));
    for (QPushButton *b : {m_stepAdd, m_stepDel, m_up, m_down}) stb->addWidget(b);
    stb->addStretch();
    m_stop = new QPushButton(QStringLiteral("■ Stop the waits"));
    m_stop->setToolTip(QStringLiteral("The pre-waits and follows still to come are dropped (the memories already running go on)"));
    m_stop->setEnabled(false);
    stb->addWidget(m_stop);
    connect(m_stop, &QPushButton::clicked, this, &SequenceWindow::stopRequested);
    auto *timelines = new QPushButton(QStringLiteral("Timelines…"));
    timelines->setToolTip(QStringLiteral("The timelines: drag one from there onto a step"));
    stb->insertWidget(stb->count() - 1, timelines);
    connect(timelines, &QPushButton::clicked, this, &SequenceWindow::timelinesRequested);
    auto *runs = new QTimer(this);
    runs->setInterval(33);
    connect(runs, &QTimer::timeout, this, &SequenceWindow::pollRuns);
    runs->start();
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
        if (m_filling || current() < 0) return;
        Engine::Sequence s = m_engine->sequence(current());
        if (it->row() >= int(s.steps.size())) return;
        Engine::SequenceStep &st = s.steps[size_t(it->row())];
        switch (it->column()) {
        case ColText: st.text = it->text(); break;
        case ColPre: st.preWait = std::clamp(it->data(Qt::EditRole).toDouble(), 0.0, 3600.0); break;
        case ColPost: st.postWait = std::clamp(it->data(Qt::EditRole).toDouble(), 0.0, 3600.0); break;
        case ColNext: st.next = Engine::StepContinue(std::clamp(it->data(Qt::EditRole).toInt(), 0, 2)); break;
        default: return;
        }
        m_engine->setSequence(current(), s);
        emit edited();
    });
    connect(m_steps, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (col == ColNum) emit goToRequested(row);
        if (col == ColMemory) {
            const Engine::Sequence s = m_engine->sequence(m_engine->currentSequence());
            if (row < int(s.steps.size()) && s.steps[size_t(row)].timeline) {
                const QRect r = m_steps->visualItemRect(m_steps->item(row, col));
                QMenu menu;
                fillActionMenu(&menu, row);
                menu.exec(m_steps->viewport()->mapToGlobal(r.bottomLeft()));
            }
        }
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
        QMenu *tl = menu.addMenu(row >= 0 ? QStringLiteral("Timeline of this step") : QStringLiteral("New step with timeline"));
        for (int i = 0; i < m_engine->animationCount(); ++i) {
            const Engine::Animation a = m_engine->animation(i);
            tl->addAction(a.name, this, [this, row, id = a.id] {
                if (row >= 0) setStepTimeline(row, id);
                else addStep(-1, 0, id);
            });
        }
        tl->setEnabled(!tl->isEmpty());
        const Engine::Sequence cs = m_engine->sequence(m_engine->currentSequence());
        if (row >= 0 && row < int(cs.steps.size()) && cs.steps[size_t(row)].timeline)
            fillActionMenu(menu.addMenu(QStringLiteral("Timeline action")), row);
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
    connect(m_engine, &Engine::animationsChanged, this, &SequenceWindow::refresh);
    refresh();
}

// The actions a timeline step can have; the one it has is checked
void SequenceWindow::fillActionMenu(QMenu *menu, int row)
{
    using A = Engine::AnimAction;
    using L = Engine::AnimLoop;
    const Engine::Sequence s = m_engine->sequence(m_engine->currentSequence());
    if (row < 0 || row >= int(s.steps.size())) return;
    const Engine::SequenceStep cur = s.steps[size_t(row)];
    auto set = [this, row](const std::function<void(Engine::SequenceStep &)> &f) {
        const int c = m_engine->currentSequence();
        Engine::Sequence q = m_engine->sequence(c);
        if (row >= int(q.steps.size())) return;
        f(q.steps[size_t(row)]);
        m_engine->setSequence(c, q);
        emit edited();
    };
    for (A a : {A::Play, A::Pause, A::Stop, A::Rewind}) {
        Engine::SequenceStep x;
        x.action = a;
        QAction *act = menu->addAction(actionName(x), this, [set, a] { set([a](Engine::SequenceStep &st) { st.action = a; }); });
        act->setCheckable(true);
        act->setChecked(cur.action == a);
    }
    QAction *seek = menu->addAction(QStringLiteral("Seek…"), this, [this, set, cur] {
        bool ok = false;
        const double t = QInputDialog::getDouble(this, QStringLiteral("Seek"), QStringLiteral("Go to (seconds played)"),
                                                 cur.seekTime, 0, 360000, 2, &ok);
        if (ok) set([t](Engine::SequenceStep &st) { st.action = A::Seek; st.seekTime = t; });
    });
    seek->setCheckable(true);
    seek->setChecked(cur.action == A::Seek);
    QAction *speed = menu->addAction(cur.action == A::Speed ? QStringLiteral("Speed…  (× %1)").arg(cur.speed, 0, 'f', 2)
                                                            : QStringLiteral("Speed…"),
                                     this, [this, set, cur] {
        bool ok = false;
        const double v = QInputDialog::getDouble(this, QStringLiteral("Speed"),
                                                 QStringLiteral("Playback speed of the timeline (1 = normal, 0 = frozen)"),
                                                 cur.action == A::Speed ? cur.speed : 1.0, 0, 10, 2, &ok);
        if (ok) set([v](Engine::SequenceStep &st) { st.action = A::Speed; st.speed = v; });
    });
    speed->setCheckable(true);
    speed->setChecked(cur.action == A::Speed);
    QMenu *loop = menu->addMenu(cur.action == A::LoopMode ? QStringLiteral("Loop mode  (%1)").arg(actionName(cur))
                                                          : QStringLiteral("Loop mode"));
    auto mode = [set](L l, int n) {
        set([l, n](Engine::SequenceStep &st) {
            st.action = A::LoopMode;
            st.loop = l;
            st.repeat = n;
        });
    };
    loop->addAction(QStringLiteral("Once"), this, [mode] { mode(L::Once, 0); });
    loop->addAction(QStringLiteral("Loop ∞"), this, [mode] { mode(L::Loop, 0); });
    loop->addAction(QStringLiteral("Loop × N…"), this, [this, mode, cur] {
        bool ok = false;
        const int n = QInputDialog::getInt(this, QStringLiteral("Loop"), QStringLiteral("Passes"), std::max(2, cur.repeat), 1, 100000, 1, &ok);
        if (ok) mode(L::Loop, n);
    });
    loop->addAction(QStringLiteral("Ping-pong ∞"), this, [mode] { mode(L::PingPong, 0); });
    loop->addAction(QStringLiteral("Ping-pong × N…"), this, [this, mode, cur] {
        bool ok = false;
        const int n = QInputDialog::getInt(this, QStringLiteral("Ping-pong"), QStringLiteral("Passes"), std::max(2, cur.repeat), 1, 100000, 1, &ok);
        if (ok) mode(L::PingPong, n);
    });
}

// The steps on their way: their time columns are redrawn while they move (and once more when they are done)
void SequenceWindow::pollRuns()
{
    std::vector<Engine::StepRun> runs = m_engine->sequenceRuns();
    if (runs.empty() && m_runs.empty()) return;
    m_runs = std::move(runs);
    m_stop->setEnabled(m_engine->sequenceRunning());
    if (isVisible()) m_steps->viewport()->update();
}

void SequenceWindow::setModified(bool on)
{
    if (on == m_modified) return;
    m_modified = on;
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
        auto *mem = new QTableWidgetItem(stepTarget(m_engine, st));
        mem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (targetGone(m_engine, st)) mem->setForeground(QColor(255, 120, 100));
        else if (st.timeline) mem->setForeground(QColor(86, 196, 255));
        auto *text = new QTableWidgetItem(st.text);
        auto *pre = new QTableWidgetItem;
        pre->setData(Qt::EditRole, st.preWait);
        auto *action = new QTableWidgetItem;
        // A timeline played without end: no time to wait for, shown as ∞
        const int ai = st.timeline ? m_engine->indexOfAnimation(st.timeline) : -1;
        const bool endless = ai >= 0 && st.action == Engine::AnimAction::Play && !std::isfinite(m_engine->animation(ai).length());
        action->setData(Qt::EditRole, endless ? -1.0 : m_engine->stepDuration(st));
        action->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        auto *post = new QTableWidgetItem;
        post->setData(Qt::EditRole, st.postWait);
        auto *next = new QTableWidgetItem;
        next->setData(Qt::EditRole, int(st.next));
        m_steps->setItem(k, ColNum, num);
        m_steps->setItem(k, ColMemory, mem);
        m_steps->setItem(k, ColPre, pre);
        m_steps->setItem(k, ColAction, action);
        m_steps->setItem(k, ColPost, post);
        m_steps->setItem(k, ColNext, next);
        m_steps->setItem(k, ColText, text);
        if (k == pos) { // the step played: green, orange once something changed since
            const QColor c = m_modified ? QColor(QString::fromLatin1(kChanged)) : kLive;
            num->setText(QStringLiteral("▶ %1").arg(k + 1));
            for (QTableWidgetItem *it : {num, mem, text}) {
                QFont f = it->font();
                f.setBold(true);
                it->setFont(f);
                it->setForeground(c);
            }
        }
    }
    if (keep >= 0 && keep < m_steps->rowCount()) m_steps->setCurrentCell(keep, ColMemory);
    if (pos >= 0 && pos < int(s.steps.size()))
        m_status->setText(QStringLiteral("Played: <b style='color:%1'>%2. %3</b>%4")
                              .arg((m_modified ? QColor(QString::fromLatin1(kChanged)) : kLive).name())
                              .arg(pos + 1)
                              .arg(stepTarget(m_engine, s.steps[size_t(pos)], false).toHtmlEscaped())
                              .arg(m_modified ? QStringLiteral(" <span style='color:%1'>— changed since it was played</span>")
                                                    .arg(QLatin1String(kChanged))
                                              : QString()));
    else
        m_status->setText(QStringLiteral("<span style='color:#8a8a90'>No step played yet</span>"));
    m_filling = false;
}

void SequenceWindow::addStep(int at, quint64 memory, quint64 timeline)
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
    Engine::SequenceStep st;
    st.memory = timeline ? 0 : memory;
    st.timeline = timeline; // Play, by default
    s.steps.insert(s.steps.begin() + at, st);
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
    s.steps[size_t(step)].timeline = 0;
    m_engine->setSequence(cur, s);
    emit edited();
}

void SequenceWindow::setStepTimeline(int step, quint64 timeline)
{
    const int cur = m_engine->currentSequence();
    Engine::Sequence s = m_engine->sequence(cur);
    if (step < 0 || step >= int(s.steps.size())) return;
    Engine::SequenceStep &st = s.steps[size_t(step)];
    if (!st.timeline) st.action = Engine::AnimAction::Play; // a step that recalled a memory: Play
    st.timeline = timeline;
    st.memory = 0;
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

// A memory or a timeline dragged from its list: onto a step, it becomes what the step does (a timeline: Play); below
// the last one, a new step
bool SequenceWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o != m_steps->viewport()) return QWidget::eventFilter(o, e);
    if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove) {
        auto *d = static_cast<QDragMoveEvent *>(e);
        if (!d->mimeData()->hasFormat(kMemoryMime) && !d->mimeData()->hasFormat(kTimelineMime)) return false;
        d->acceptProposedAction();
        return true;
    }
    if (e->type() == QEvent::Drop) {
        auto *d = static_cast<QDropEvent *>(e);
        const bool timeline = d->mimeData()->hasFormat(kTimelineMime);
        if (!d->mimeData()->hasFormat(kMemoryMime) && !timeline) return false;
        const quint64 id = d->mimeData()->data(timeline ? kTimelineMime : kMemoryMime).toULongLong();
        const int row = m_steps->rowAt(d->position().toPoint().y());
        if (row >= 0) timeline ? setStepTimeline(row, id) : setStepMemory(row, id);
        else timeline ? addStep(-1, 0, id) : addStep(-1, id);
        d->acceptProposedAction();
        return true;
    }
    return QWidget::eventFilter(o, e);
}
