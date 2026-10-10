#include "TimelinePanel.h"
#include "Commands.h"
#include "Engine.h"
#include "Widgets.h"

#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

static const QColor kPlaying(76, 217, 100);

// ---------------------------------------------------------------------------
// TimelineList

QStringList TimelineList::mimeTypes() const { return {QString::fromLatin1(kTimelineMime)}; }

QMimeData *TimelineList::mimeData(const QList<QListWidgetItem *> &items) const
{
    if (items.isEmpty()) return nullptr;
    auto *m = new QMimeData;
    m->setData(kTimelineMime, QByteArray::number(items.first()->data(Qt::UserRole).toULongLong()));
    return m;
}

// ---------------------------------------------------------------------------
// A timeline's track: on or off, the layer (or the composition) and the number it drives, then the common settings

class TimelineTrackRow : public TrackRow
{
public:
    TimelineTrackRow(TimelineEditor *editor, int index) : TrackRow(editor, index), m_tl(editor) {}
    void fill() override;

protected:
    void buildHeader(QGridLayout *g) override;

private:
    void fillParams(); // the numbers of its layer, in the menu
    void selectParam(const QString &path);
    TimelineEditor *m_tl;
    FlagBox *m_on = nullptr;
    QComboBox *m_layer = nullptr;
    QPushButton *m_param = nullptr; // its menu: the categories, each one a sub-menu of its numbers
};

void TimelineTrackRow::buildHeader(QGridLayout *g)
{
    m_on = new FlagBox;
    m_on->setToolTip(QStringLiteral("On: the track drives its number while the timeline plays"));
    m_layer = new QComboBox;
    m_layer->setToolTip(QStringLiteral("The layer driven (or the composition)"));
    auto *del = new QPushButton(QStringLiteral("✕"));
    del->setFixedWidth(26);
    del->setToolTip(QStringLiteral("Delete the track"));
    m_param = new QPushButton;
    m_param->setToolTip(QStringLiteral("The number driven: a category, then the number in it"));
    m_param->setMenu(new QMenu(m_param));
    m_param->setStyleSheet(QStringLiteral("QPushButton { text-align:left; padding-left:6px; }"));
    g->addWidget(m_on, 0, 0);
    g->addWidget(m_layer, 0, 1, 1, 2);
    g->addWidget(del, 0, 3);
    g->addWidget(m_param, 1, 0, 1, 4);

    connect(m_on, &QCheckBox::toggled, this, [this](bool on) {
        if (m_filling) return;
        track().enabled = on;
        changed();
    });
    connect(m_layer, qOverload<int>(&QComboBox::activated), this, [this](int) {
        track().layer = m_layer->currentData().toULongLong();
        const auto &params = m_ed->paramsOf(track().layer);
        // Its first number, from where it is
        track().param = params.empty() ? QString() : params.front().path;
        track().keys = {AnimKey{0, currentValue(), 3}};
        fillParams();
        fill();
        changed();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        QTimer::singleShot(0, m_tl, [tl = m_tl, i = m_i] { tl->deleteTrack(i); }); // not from inside this row
    });
    // The layers (top first), then the composition
    m_layer->addItem(QStringLiteral("Composition"), QVariant::fromValue<quint64>(0));
    {
        Engine::Lock lk(&m_e->mutex());
        for (int i = 0; i < m_e->layerCount(); ++i) {
            const Layer *l = m_e->layer(i);
            m_layer->addItem((l->isViewport ? QStringLiteral("▣ ") : l->isGroup ? QStringLiteral("▤ ") : QString()) + l->name,
                             QVariant::fromValue<quint64>(l->id));
        }
    }
    fillParams();
}

void TimelineTrackRow::fillParams()
{
    QMenu *menu = m_param->menu();
    menu->clear();
    addParamMenu(menu, m_ed->paramsOf(track().layer), [this](QMenu *into, const Engine::AnimParam &p, const QString &text) {
        QAction *a = into->addAction(text, this, [this, path = p.path] { selectParam(path); });
        a->setCheckable(true);
        a->setChecked(p.path == track().param);
    });
}

void TimelineTrackRow::selectParam(const QString &path)
{
    track().param = path;
    const auto [lo, hi] = range();
    track().keys = {AnimKey{0, currentValue(), 3}};
    track().center = path == "spatial/rotation" ? 0 : (lo + hi) / 2;
    track().amplitude = path == "spatial/rotation" ? 180 : (hi - lo) / 2;
    fillParams(); // the check mark
    fill();
    changed();
}

void TimelineTrackRow::fill()
{
    m_filling = true;
    const AnimTrack &t = track();
    m_on->setChecked(t.enabled);
    int li = m_layer->findData(QVariant::fromValue<quint64>(t.layer));
    if (li < 0) { // the layer is gone: said so, the track is kept
        m_layer->addItem(QStringLiteral("(layer gone)"), QVariant::fromValue<quint64>(t.layer));
        li = m_layer->count() - 1;
    }
    m_layer->setCurrentIndex(li);
    QString label = t.param.isEmpty() ? QStringLiteral("(choose a number)") : QStringLiteral("(%1 — not on this layer)").arg(t.param);
    for (const Engine::AnimParam &p : m_ed->paramsOf(t.layer))
        if (p.path == t.param) label = p.label;
    m_param->setText(label);
    m_filling = false;
    TrackRow::fill();
}

// ---------------------------------------------------------------------------
// TimelineEditor

TimelineEditor::TimelineEditor(Engine *engine, QUndoStack *undo, QWidget *parent)
    : AnimEditor(engine, undo, Layout::Side, true, parent)
{
    auto *bottom = new QHBoxLayout;
    m_addTrack = new QPushButton(QStringLiteral("+ Track"));
    m_addTrack->setToolTip(QStringLiteral("A track for the layer selected in the layer list"));
    bottom->addWidget(m_addTrack);
    auto *how = new QLabel(QStringLiteral("Lane: click: a key · drag a key: move it (Shift: one axis) · drag elsewhere: draw · "
                                          "double-click a key: delete · right-click: curve (Bézier…), value, start from the current value"));
    how->setStyleSheet("color:#8a8a90; font-size:11px;");
    bottom->addWidget(how, 1);
    mainLayout()->addLayout(bottom);

    connect(m_addTrack, &QPushButton::clicked, this, [this] {
        if (!m_has) return;
        AnimTrack t;
        quint64 layer = m_currentLayer;
        if (m_engine->indexOfId(layer) < 0) { // none selected: the first layer that is not a viewport
            layer = 0;
            Engine::Lock lk(&m_engine->mutex());
            for (int i = 0; i < m_engine->layerCount() && !layer; ++i)
                if (!m_engine->layer(i)->isViewport) layer = m_engine->layer(i)->id;
        }
        t.layer = layer;
        t.param = QStringLiteral("opacity");
        double v = 1;
        m_engine->animParamValue(t.layer, t.param, &v);
        t.keys = {AnimKey{0, v, 3}};
        m_edit.tracks.push_back(t);
        commit(QStringLiteral("Add Track"));
        rebuildRows();
        applyView();
    });
    connect(m_engine, &Engine::layersChanged, this, [this] {
        if (QApplication::mouseButtons() & Qt::LeftButton) return;
        forgetParams(); // a layer's numbers (another source, another effect)
        rebuildRows();  // the names of the layers
        applyView();
    });
}

void TimelineEditor::setTimeline(quint64 id)
{
    if (id != m_id) forgetView(); // another timeline: seen whole
    m_id = id;
    reload();
}

bool TimelineEditor::fetch(Animation *a) const
{
    const int i = m_engine->indexOfAnimation(m_id);
    if (i < 0) return false;
    *a = m_engine->animation(i);
    return true;
}

void TimelineEditor::store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey)
{
    Animation a = after;
    a.name = before.name; // its name is the list's (renamed meanwhile)
    if (m_undo) m_undo->push(new cmd::SetAnimation(m_engine, m_id, before, a, text, mergeKey));
    else m_engine->setAnimation(m_engine->indexOfAnimation(m_id), a);
}

void TimelineEditor::control(AnimAction action, double time) { m_engine->controlAnimation(m_id, action, time); }

TrackRow *TimelineEditor::makeRow(int index) { return new TimelineTrackRow(this, index); }

void TimelineEditor::deleteTrack(int index)
{
    if (index < 0 || index >= int(m_edit.tracks.size())) return;
    m_edit.tracks.erase(m_edit.tracks.begin() + index);
    if (index < int(m_laneViews.size())) m_laneViews.erase(m_laneViews.begin() + index);
    commit(QStringLiteral("Delete Track"));
    rebuildRows();
    applyView();
}

// ---------------------------------------------------------------------------
// TimelineWindow

TimelineWindow::TimelineWindow(Engine *engine, QUndoStack *undo, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint), m_engine(engine), m_undo(undo)
{
    setWindowTitle(QStringLiteral("Timelines"));
    resize(1120, 560);
    auto *h = new QHBoxLayout(this);

    // The list
    auto *left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral("<b>Timelines</b>")));
    m_list = new TimelineList;
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setToolTip(QStringLiteral("Drag a timeline onto a step of a sequence (Play by default) · double-click to rename"));
    left->addWidget(m_list, 1);
    auto *lb = new QHBoxLayout;
    m_add = new QPushButton(QStringLiteral("+"));
    m_add->setToolTip(QStringLiteral("New timeline"));
    m_dup = new QPushButton(QStringLiteral("Duplicate"));
    m_del = new QPushButton(QStringLiteral("−"));
    m_del->setToolTip(QStringLiteral("Delete the timeline (the steps that drive it then do nothing)"));
    for (QPushButton *b : {m_add, m_dup, m_del}) lb->addWidget(b);
    left->addLayout(lb);
    auto *hint = new QLabel(QStringLiteral("Drag a timeline onto a step of a sequence: the step plays it (or pauses, "
                                           "stops, rewinds, seeks, changes its loop)."));
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#8a8a90; font-size:11px;");
    left->addWidget(hint);
    h->addLayout(left, 1);

    // The one selected
    m_editor = new TimelineEditor(engine, undo);
    h->addWidget(m_editor, 4);
    connect(m_editor, &AnimEditor::edited, this, &TimelineWindow::edited);

    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (m_filling) return;
        m_current = it ? it->data(Qt::UserRole).toULongLong() : 0;
        showCurrent();
    });
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_filling) return;
        const int i = m_engine->indexOfAnimation(it->data(Qt::UserRole).toULongLong());
        if (i < 0) return;
        const Animation before = m_engine->animation(i);
        if (before.name == it->text()) return;
        Animation a = before;
        a.name = it->text();
        m_renaming = true;
        if (m_undo) m_undo->push(new cmd::SetAnimation(m_engine, a.id, before, a, QStringLiteral("Rename Timeline")));
        else m_engine->setAnimation(i, a);
        m_renaming = false;
        emit edited();
    });
    auto add = [this](const Animation &a, int at, const QString &text) {
        auto *c = new cmd::AddAnimation(m_engine, a, at, text);
        if (m_undo) m_undo->push(c);
        else c->redo();
        m_current = c->animationId();
        if (!m_undo) delete c;
        refreshList();
        showCurrent();
        emit edited();
    };
    connect(m_add, &QPushButton::clicked, this, [this, add] {
        Animation a;
        a.name = QStringLiteral("Timeline %1").arg(m_engine->animationCount() + 1);
        add(a, m_engine->animationCount(), QStringLiteral("New Timeline"));
    });
    connect(m_dup, &QPushButton::clicked, this, [this, add] {
        const int i = m_engine->indexOfAnimation(m_current);
        if (i < 0) return;
        Animation a = m_engine->animation(i);
        a.id = 0;
        a.name += QStringLiteral(" copy");
        add(a, i + 1, QStringLiteral("Duplicate Timeline"));
    });
    connect(m_del, &QPushButton::clicked, this, [this] {
        const int i = m_engine->indexOfAnimation(m_current);
        if (i < 0) return;
        if (m_undo) m_undo->push(new cmd::RemoveAnimation(m_engine, i));
        else m_engine->removeAnimation(i);
        emit edited();
    });
    connect(m_engine, &Engine::animationsChanged, this, [this] {
        if (m_renaming || m_editor->isCommitting()) return; // its own edit: the rows (a key dragged) stay
        refreshList();
        showCurrent();
    });
    auto *timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, &TimelineWindow::pollList);
    timer->start();
    refreshList();
    showCurrent();
}

void TimelineWindow::refreshList()
{
    m_filling = true;
    m_list->clear();
    const int n = m_engine->animationCount();
    if (m_engine->indexOfAnimation(m_current) < 0) m_current = n > 0 ? m_engine->animation(0).id : 0;
    for (int i = 0; i < n; ++i) {
        const Animation a = m_engine->animation(i);
        auto *it = new QListWidgetItem(a.name, m_list);
        it->setData(Qt::UserRole, QVariant::fromValue<quint64>(a.id));
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
        if (a.id == m_current) m_list->setCurrentItem(it);
    }
    m_filling = false;
}

void TimelineWindow::showCurrent()
{
    const bool has = m_engine->indexOfAnimation(m_current) >= 0;
    m_dup->setEnabled(has);
    m_del->setEnabled(has);
    m_editor->setTimeline(has ? m_current : 0);
}

void TimelineWindow::pollList()
{
    if (!isVisible()) return;
    for (int k = 0; k < m_list->count(); ++k) {
        QListWidgetItem *it = m_list->item(k);
        const Animation a = m_engine->animation(m_engine->indexOfAnimation(it->data(Qt::UserRole).toULongLong()));
        const QBrush b = a.state == AnimState::Playing ? QBrush(kPlaying) : QBrush();
        if (it->foreground() != b) it->setForeground(b);
    }
}
