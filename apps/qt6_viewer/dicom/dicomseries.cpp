#include "dicomseries.h"

#include <dcmtk/dcmdata/dctk.h>

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <tuple>

namespace
{
QString tagText(DcmDataset *dataset, const DcmTagKey &tag)
{
    OFString value;
    if (!dataset || dataset->findAndGetOFStringArray(tag, value).bad())
        return {};
    return QString::fromUtf8(value.c_str()).trimmed();
}

bool parseSlicePosition(const QString &text, double *position)
{
    const QStringList parts = text.split(QLatin1Char('\\'));
    if (parts.size() != 3)
        return false;
    bool ok = false;
    const double value = parts.at(2).toDouble(&ok);
    if (ok && position)
        *position = value;
    return ok;
}
}

QString DicomSeries::displayName() const
{
    QStringList parts;
    if (!modality.isEmpty())
        parts << modality;
    if (!seriesDescription.isEmpty())
        parts << seriesDescription;
    else if (!studyDescription.isEmpty())
        parts << studyDescription;
    if (parts.isEmpty())
        parts << QStringLiteral("Unnamed Series");
    return QStringLiteral("%1 (%2 images)")
        .arg(parts.join(QStringLiteral(" - ")))
        .arg(instances.size());
}

QVector<DicomSeries> DicomSeriesScanner::scanFolder(const QString &folderPath,
                                                     QStringList *errors)
{
    QHash<QString, int> seriesByKey;
    QVector<DicomSeries> result;
    QDirIterator iterator(folderPath, QDir::Files | QDir::Readable,
                          QDirIterator::Subdirectories);

    while (iterator.hasNext())
    {
        const QString path = iterator.next();
        const QByteArray nativePath = QFile::encodeName(path);
        DcmFileFormat file;
        const OFCondition status = file.loadFile(nativePath.constData());
        if (status.bad())
            continue;

        DcmDataset *dataset = file.getDataset();
        if (!dataset || !dataset->tagExists(DCM_PixelData))
            continue;
        const QString seriesUid = tagText(dataset, DCM_SeriesInstanceUID);
        const QString studyUid = tagText(dataset, DCM_StudyInstanceUID);
        if (seriesUid.isEmpty())
        {
            if (errors)
                errors->append(QStringLiteral("Missing SeriesInstanceUID: %1").arg(path));
            continue;
        }

        const QString key = studyUid + QLatin1Char('|') + seriesUid;
        int seriesIndex = seriesByKey.value(key, -1);
        if (seriesIndex < 0)
        {
            DicomSeries series;
            series.studyInstanceUid = studyUid;
            series.seriesInstanceUid = seriesUid;
            series.studyDescription = tagText(dataset, DCM_StudyDescription);
            series.seriesDescription = tagText(dataset, DCM_SeriesDescription);
            series.modality = tagText(dataset, DCM_Modality);
            seriesIndex = result.size();
            seriesByKey.insert(key, seriesIndex);
            result.push_back(series);
        }

        DicomSeriesInstance instance;
        instance.path = QFileInfo(path).absoluteFilePath();
        bool instanceOk = false;
        instance.instanceNumber = tagText(dataset, DCM_InstanceNumber).toInt(&instanceOk);
        instance.hasInstanceNumber = instanceOk;
        instance.hasSlicePosition = parseSlicePosition(
            tagText(dataset, DCM_ImagePositionPatient), &instance.slicePosition);
        result[seriesIndex].instances.push_back(instance);
    }

    for (DicomSeries &series : result)
    {
        const bool allHaveSlicePosition = std::all_of(
            series.instances.cbegin(), series.instances.cend(),
            [](const DicomSeriesInstance &instance) {
                return instance.hasSlicePosition;
            });
        const bool allHaveInstanceNumber = std::all_of(
            series.instances.cbegin(), series.instances.cend(),
            [](const DicomSeriesInstance &instance) {
                return instance.hasInstanceNumber;
            });

        std::stable_sort(series.instances.begin(), series.instances.end(),
                         [allHaveSlicePosition, allHaveInstanceNumber](
                             const DicomSeriesInstance &left,
                             const DicomSeriesInstance &right) {
            if (allHaveSlicePosition)
            {
                return std::tie(left.slicePosition, left.path) <
                       std::tie(right.slicePosition, right.path);
            }
            if (allHaveInstanceNumber)
            {
                return std::tie(left.instanceNumber, left.path) <
                       std::tie(right.instanceNumber, right.path);
            }

            // Mixed or incomplete metadata must still use one fixed key for
            // every comparison so std::stable_sort receives a strict ordering.
            const auto key = [](const DicomSeriesInstance &instance) {
                if (instance.hasSlicePosition)
                    return std::tuple<int, double, int, QString>(
                        0, instance.slicePosition, 0, instance.path.toCaseFolded());
                if (instance.hasInstanceNumber)
                    return std::tuple<int, double, int, QString>(
                        1, 0.0, instance.instanceNumber, instance.path.toCaseFolded());
                return std::tuple<int, double, int, QString>(
                    2, 0.0, 0, instance.path.toCaseFolded());
            };
            return key(left) < key(right);
        });
    }

    std::sort(result.begin(), result.end(),
              [](const DicomSeries &left, const DicomSeries &right) {
        return QString::compare(left.displayName(), right.displayName(),
                                Qt::CaseInsensitive) < 0;
    });
    return result;
}
