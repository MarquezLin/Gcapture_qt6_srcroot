#ifndef PROCAMP_H
#define PROCAMP_H

#include <QDialog>
#include "gcapture.h"

namespace Ui
{
    class ProcAmp;
}

class ProcAmp : public QDialog
{
    Q_OBJECT

public:
    explicit ProcAmp(QWidget *parent = nullptr);
    ~ProcAmp();

    void setValues(const gcap_procamp_t &p);
    gcap_procamp_t values() const;
    void setControlsEnabled(bool en);
    void setParams(const gcap_procamp_t &p);

signals:
    void valuesChanged(const gcap_procamp_t &p);

private:
    void updateValueLabels();
    gcap_procamp_t currentParams() const;
    Ui::ProcAmp *ui;
};

#endif // PROCAMP_H
