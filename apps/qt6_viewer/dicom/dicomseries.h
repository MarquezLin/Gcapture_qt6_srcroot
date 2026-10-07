#ifndef DICOMSERIES_H
#define DICOMSERIES_H

#include <QString>
#include <QStringList>
#include <QVector>

struct DicomSeriesInstance
{
    QString path;
    int instanceNumber = 0;
    bool hasInstanceNumber = false;
    double slicePosition = 0.0;
    bool hasSlicePosition = false;
};

struct DicomSeries
{
    QString studyInstanceUid;
    QString seriesInstanceUid;
    QString studyDescription;
    QString seriesDescription;
    QString modality;
    QVector<DicomSeriesInstance> instances;

    QString displayName() const;
};

class DicomSeriesScanner
{
public:
    static QVector<DicomSeries> scanFolder(const QString &folderPath,
                                           QStringList *errors = nullptr);
};

#endif
