#pragma once
#include <QWidget>

class Engine;
class IsfInstance;

// Panneau de paramètres généré automatiquement à partir des INPUTS d'un shader ISF.
class ParamPanel : public QWidget
{
    Q_OBJECT
public:
    ParamPanel(Engine *engine, IsfInstance *inst, QWidget *parent = nullptr);

signals:
    void rebuildRequested();

private:
    Engine *m_engine;
    IsfInstance *m_inst;
};
