#pragma once
#include "Engine.h"
#include <QWidget>

class Engine;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class QCheckBox;
class QTimer;

// Where the memory recalled last is in its fade: a thin bar filling over its time (green while it runs), with the
// memory's name and the seconds as tooltip. Follows the engine by itself.
class RecallProgressBar : public QWidget
{
    Q_OBJECT
public:
    explicit RecallProgressBar(Engine *engine, QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(100, 5); }
    QSize minimumSizeHint() const override { return QSize(10, 5); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void poll();
    Engine *m_engine;
    double m_fraction = 1;
    bool m_running = false;
};

// The sequences of the show: ordered steps, each recalling a memory and carrying a text for the operator, with a
// pre-wait, a post-wait and what comes next (wait for GO, follow, auto-follow), as in QLab.
// SequenceBar: the reduced view, under the preview — GO BACK, GO, STOP (the waits still to come), the previous /
// current / next steps (the current one in another color once something was changed since it was played), the
// current step's text, and the time left before what is waiting.
// SequenceWindow: a floating window, always in front, to edit everything: the sequences (new, duplicate, delete,
// rename, loop), their steps (+, delete, move, a memory dragged onto a step, its text), and to play any step.
class SequenceBar : public QWidget
{
    Q_OBJECT
public:
    explicit SequenceBar(Engine *engine, QWidget *parent = nullptr);
    void refresh();
    void setModified(bool on); // something changed since the current step was played

signals:
    void goRequested();     // GO: the next step
    void backRequested();   // GO BACK: the previous step
    void stopRequested();   // the pre-waits and follows still to come
    void windowRequested(); // the floating window
    void edited();

private:
    Engine *m_engine;
    QComboBox *m_sequence;
    QPushButton *m_go, *m_back, *m_stop, *m_open;
    QLabel *m_wait; // what is waiting, and in how long
    void pollWaits();
    QLabel *m_prev, *m_current, *m_next;
    QLabel *m_prevText, *m_currentText, *m_nextText; // each step's text, under it
    bool m_modified = false, m_filling = false;
};

class SequenceWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SequenceWindow(Engine *engine, QWidget *parent = nullptr);
    void refresh();
    void setModified(bool on); // something changed since the current step was played

signals:
    void goToRequested(int step); // play that step of the current sequence
    void stopRequested();         // the pre-waits and follows still to come
    void edited();                // the sequences changed: the project is modified

protected:
    bool eventFilter(QObject *o, QEvent *e) override; // memories dropped onto the steps

private:
    void addStep(int at, quint64 memory);
    void setStepMemory(int step, quint64 memory);
    void removeSteps();
    void moveStep(int delta);
    Engine *m_engine;
    QListWidget *m_sequences;
    QCheckBox *m_loop;
    QTableWidget *m_steps;
    QLabel *m_status; // the step played, and whether something changed since
    bool m_modified = false;
    QPushButton *m_add, *m_dup, *m_del, *m_stepAdd, *m_stepDel, *m_up, *m_down, *m_stop;
    bool m_filling = false;
    std::vector<Engine::StepRun> m_runs; // the steps on their way, drawn in their time columns
    void pollRuns();
    friend class StepDelegate;
};
