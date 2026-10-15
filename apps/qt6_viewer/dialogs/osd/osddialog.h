#pragma once

#include <QDialog>

class GvfgSource;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class OsdDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit OsdDialog(GvfgSource *source, QWidget *parent = nullptr);

private:
    void chooseColor();
    void apply();
    void clear();
    void updateColorButton();

    GvfgSource *source_ = nullptr;
    QLineEdit *text_ = nullptr;
    QSpinBox *x_ = nullptr;
    QSpinBox *y_ = nullptr;
    QSpinBox *fontSize_ = nullptr;
    QPushButton *colorButton_ = nullptr;
    QLabel *status_ = nullptr;
    unsigned int colorRgb_ = 0x00ffffffu;
};
