#pragma once
#include "Layer.h"
#include <QDialog>

class QComboBox;

// Application preferences (saved on this machine, not in the project).
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

    // Mode given to a video or a sound when it is loaded into a layer (Loop unless changed)
    static PlayMode defaultPlayMode();
    static void setDefaultPlayMode(PlayMode m);

private:
    QComboBox *m_playMode = nullptr;
};
