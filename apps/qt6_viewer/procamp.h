#ifndef PROCAMP_H
#define PROCAMP_H

#include <QDialog>

namespace Ui {
class ProcAmp;
}

class ProcAmp : public QDialog
{
    Q_OBJECT

public:
    explicit ProcAmp(QWidget *parent = nullptr);
    ~ProcAmp();

private:
    Ui::ProcAmp *ui;
};

#endif // PROCAMP_H
