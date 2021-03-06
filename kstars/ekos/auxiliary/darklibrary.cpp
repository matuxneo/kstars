/*  Ekos Dark Library Handler
    Copyright (C) 2016 Jasem Mutlaq <mutlaqja@ikarustech.com>

    This application is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
 */

#include "darklibrary.h"

#include "Options.h"

#include "kstars.h"
#include "kspaths.h"
#include "kstarsdata.h"
#include "fitsviewer/fitsdata.h"
#include "fitsviewer/fitsview.h"

namespace Ekos
{
DarkLibrary *DarkLibrary::_DarkLibrary = nullptr;

DarkLibrary *DarkLibrary::Instance()
{
    if (_DarkLibrary == nullptr)
        _DarkLibrary = new DarkLibrary(KStars::Instance());

    return _DarkLibrary;
}

DarkLibrary::DarkLibrary(QObject *parent) : QObject(parent)
{
    KStarsData::Instance()->userdb()->GetAllDarkFrames(darkFrames);

    subtractParams.duration    = 0;
    subtractParams.offsetX     = 0;
    subtractParams.offsetY     = 0;
    subtractParams.targetChip  = nullptr;
    subtractParams.targetImage = nullptr;

    QDir writableDir;
    writableDir.mkdir(KSPaths::writableLocation(QStandardPaths::GenericDataLocation) + "darks");
}

DarkLibrary::~DarkLibrary()
{
    qDeleteAll(darkFiles);
}

void DarkLibrary::refreshFromDB()
{
    KStarsData::Instance()->userdb()->GetAllDarkFrames(darkFrames);
}

FITSData *DarkLibrary::getDarkFrame(ISD::CCDChip *targetChip, double duration)
{
    for (auto &map : darkFrames)
    {
        // First check CCD name matches and check if we are on the correct chip
        if (map["ccd"].toString() == targetChip->getCCD()->getDeviceName() &&
                map["chip"].toInt() == static_cast<int>(targetChip->getType()))
        {
            int binX, binY;
            targetChip->getBinning(&binX, &binY);

            // Then check if binning is the same
            if (map["binX"].toInt() == binX && map["binY"].toInt() == binY)
            {
                // Then check for temperature
                if (targetChip->getCCD()->hasCooler())
                {
                    double temperature = 0;
                    targetChip->getCCD()->getTemperature(&temperature);
                    // TODO make this configurable value, the threshold
                    if (fabs(map["temperature"].toDouble() - temperature) > Options::maxDarkTemperatureDiff())
                        continue;
                }

                // Then check for duration
                // TODO make this value configurable
                if (fabs(map["duration"].toDouble() - duration) > 0.05)
                    continue;

                // Finally check if the duration is acceptable
                QDateTime frameTime = QDateTime::fromString(map["timestamp"].toString(), Qt::ISODate);
                if (frameTime.daysTo(QDateTime::currentDateTime()) > Options::darkLibraryDuration())
                    continue;

                QString filename = map["filename"].toString();

                if (darkFiles.contains(filename))
                    return darkFiles[filename];

                // Finally we made it, let's put it in the hash
                if (loadDarkFile(filename))
                    return darkFiles[filename];
                else
                {
                    // Remove bad dark frame
                    emit newLog(i18n("Removing bad dark frame file %1", filename));
                    darkFiles.remove(filename);
                    QFile::remove(filename);
                    KStarsData::Instance()->userdb()->DeleteDarkFrame(filename);
                    return nullptr;
                }
            }
        }
    }

    return nullptr;
}

bool DarkLibrary::loadDarkFile(const QString &filename)
{
    FITSData *darkData = new FITSData();

    bool rc = darkData->loadFITS(filename);

    if (rc)
        darkFiles[filename] = darkData;
    else
    {
        emit newLog(i18n("Failed to load dark frame file %1", filename));
        delete (darkData);
    }

    return rc;
}

bool DarkLibrary::saveDarkFile(FITSData *darkData)
{
    // IS8601 contains colons but they are illegal under Windows OS, so replacing them with '-'
    // The timestamp is no longer ISO8601 but it should solve interoperality issues between different OS hosts
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-ddThh-mm-ss");

    QString path = KSPaths::writableLocation(QStandardPaths::GenericDataLocation) + "darks/darkframe_" + ts + ".fits";

    if (darkData->saveFITS(path) != 0)
    {
        qCritical() << "DarkLibrary: Failed to save dark frame " << path;
        return false;
    }

    darkFiles[path] = darkData;

    QVariantMap map;
    int binX, binY;
    double temperature = 0;

    subtractParams.targetChip->getBinning(&binX, &binY);
    subtractParams.targetChip->getCCD()->getTemperature(&temperature);

    map["ccd"]         = subtractParams.targetChip->getCCD()->getDeviceName();
    map["chip"]        = static_cast<int>(subtractParams.targetChip->getType());
    map["binX"]        = binX;
    map["binY"]        = binY;
    map["temperature"] = temperature;
    map["duration"]    = subtractParams.duration;
    map["filename"]    = path;

    darkFrames.append(map);

    emit newLog(i18n("Dark frame saved to %1", path));

    KStarsData::Instance()->userdb()->AddDarkFrame(map);

    return true;
}

bool DarkLibrary::subtract(FITSData *darkData, FITSView *lightImage, FITSScale filter, uint16_t offsetX,
                           uint16_t offsetY)
{
    Q_ASSERT(darkData);
    Q_ASSERT(lightImage);

    switch (darkData->property("dataType").toInt())
    {
        case TBYTE:
            return subtract<uint8_t>(darkData, lightImage, filter, offsetX, offsetY);

        case TSHORT:
            return subtract<int16_t>(darkData, lightImage, filter, offsetX, offsetY);

        case TUSHORT:
            return subtract<uint16_t>(darkData, lightImage, filter, offsetX, offsetY);

        case TLONG:
            return subtract<int32_t>(darkData, lightImage, filter, offsetX, offsetY);

        case TULONG:
            return subtract<uint32_t>(darkData, lightImage, filter, offsetX, offsetY);

        case TFLOAT:
            return subtract<float>(darkData, lightImage, filter, offsetX, offsetY);

        case TLONGLONG:
            return subtract<int64_t>(darkData, lightImage, filter, offsetX, offsetY);

        case TDOUBLE:
            return subtract<double>(darkData, lightImage, filter, offsetX, offsetY);

        default:
            break;
    }

    return false;
}

template <typename T>
bool DarkLibrary::subtract(FITSData *darkData, FITSView *lightImage, FITSScale filter, uint16_t offsetX,
                           uint16_t offsetY)
{
    FITSData *lightData = lightImage->getImageData();


    T *lightBuffer = reinterpret_cast<T *>(lightData->getImageBuffer());
    int lightW      = lightData->width();
    int lightH      = lightData->height();

    int darkW      = darkData->width();
    int darkoffset = offsetX + offsetY * darkW;
    T *darkBuffer  = reinterpret_cast<T *>(darkData->getImageBuffer()) + darkoffset;

    for (int i = 0; i < lightH; i++)
    {
        for (int j = 0; j < lightW; j++)
            lightBuffer[j] = (lightBuffer[j] > darkBuffer[j]) ? (lightBuffer[j] - darkBuffer[j]) : 0;

        lightBuffer += lightW;
        darkBuffer += darkW;
    }

#if 0
    int lightOffset = 0;
    for (int i = 0; i < lightH; i++)
    {
        for (int j = 0; j < lightW; j++)
        {
            if (lightBuffer[j + lightOffset] > darkBuffer[j + darkoffset])
                lightBuffer[j + lightOffset] -= darkBuffer[j + darkoffset];
            else
                lightBuffer[j + lightOffset] = 0;
        }

        lightOffset += lightW;
        darkoffset += darkW;
    }
#endif

    lightData->applyFilter(filter);
    //if (Options::autoStretch())
    //    lightData->applyFilter(FITS_AUTO_STRETCH);
    //else if (filter == FITS_NONE)
    //    lightData->calculateStats(true);
    if (filter == FITS_NONE)
        lightData->calculateStats(true);
    lightImage->rescale(ZOOM_KEEP_LEVEL);
    lightImage->updateFrame();

    // If telescope is covered, let's uncover it
    if (m_TelescopeCovered)
    {
        QString deviceName = subtractParams.targetChip->getCCD()->getDeviceName();
        bool hasNoShutter = Options::shutterlessCCDs().contains(deviceName);
        // Only ask if no shutter and is temporary file
        // For regular files, the data is already loaded so no need to ask user to remove cover
        // since dark data is loaded from disk.
        if (hasNoShutter)
        {
            if (KMessageBox::warningContinueCancel(
                        nullptr, i18n("Remove cover from the telescope in order to continue."), i18n("Dark Exposure"),
                        KStandardGuiItem::cont(), KStandardGuiItem::cancel(),
                        "uncover_scope_dialog_notification", KMessageBox::WindowModal | KMessageBox::Notify) == KMessageBox::Cancel)
            {
                emit darkFrameCompleted(false);
                return false;
            }
            else
                m_TelescopeCovered = false;
        }
    }

    emit darkFrameCompleted(true);

    return true;
}

bool DarkLibrary::captureAndSubtract(ISD::CCDChip *targetChip, FITSView *targetImage, double duration, uint16_t offsetX,
                                     uint16_t offsetY)
{
    QStringList shutterfulCCDs  = Options::shutterfulCCDs();
    QStringList shutterlessCCDs = Options::shutterlessCCDs();
    QString deviceName          = targetChip->getCCD()->getDeviceName();

    bool hasShutter   = shutterfulCCDs.contains(deviceName);
    bool hasNoShutter = shutterlessCCDs.contains(deviceName);

    // If no information is available either way, then ask the user
    if (hasShutter == false && hasNoShutter == false)
    {
        // If DSLR then it is considered to have no shutter
        // since the camera needs to open the shutter to take dark frames
        if (targetChip->getISOList().empty() == false)
        {
            hasNoShutter = true;
            shutterlessCCDs.append(deviceName);
            Options::setShutterlessCCDs(shutterlessCCDs);
        }
        else if (KMessageBox::questionYesNo(nullptr, i18n("Does %1 have a shutter?", deviceName),
                                            i18n("Dark Exposure")) == KMessageBox::Yes)
        {
            hasNoShutter = false;
            shutterfulCCDs.append(deviceName);
            Options::setShutterfulCCDs(shutterfulCCDs);
        }
        else
        {
            hasNoShutter = true;
            shutterlessCCDs.append(deviceName);
            Options::setShutterlessCCDs(shutterlessCCDs);
        }
    }

    if (hasNoShutter)
    {
        if (KMessageBox::warningContinueCancel(
                    nullptr, i18n("Cover the telescope in order to take a dark exposure."), i18n("Dark Exposure"),
                    KStandardGuiItem::cont(), KStandardGuiItem::cancel(),
                    "cover_scope_dialog_notification", KMessageBox::WindowModal | KMessageBox::Notify) == KMessageBox::Cancel)
        {
            emit newLog(i18n("Dark frame capture cancelled."));
            disconnect(targetChip->getCCD(), SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));
            emit darkFrameCompleted(false);
            return false;
        }
        else
            m_TelescopeCovered = true;
    }

    targetChip->resetFrame();
    targetChip->setCaptureMode(FITS_CALIBRATE);
    targetChip->setFrameType(FRAME_DARK);

    subtractParams.targetChip  = targetChip;
    subtractParams.targetImage = targetImage;
    subtractParams.duration    = duration;
    subtractParams.offsetX     = offsetX;
    subtractParams.offsetY     = offsetY;

    connect(targetChip->getCCD(), SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));

    emit newLog(i18n("Capturing dark frame..."));

    targetChip->capture(duration);

    return true;
}

void DarkLibrary::newFITS(IBLOB *bp)
{
    INDI_UNUSED(bp);

    Q_ASSERT(subtractParams.targetChip);

    disconnect(subtractParams.targetChip->getCCD(), SIGNAL(BLOBUpdated(IBLOB*)), this, SLOT(newFITS(IBLOB*)));

    FITSView *calibrationView = subtractParams.targetChip->getImageView(FITS_CALIBRATE);

    if (calibrationView == nullptr)
    {
        emit darkFrameCompleted(false);
        return;
    }

    emit newLog(i18n("Dark frame received."));

    FITSData *calibrationData = new FITSData();

    // Deep copy of the data
    if (calibrationData->loadFITS(calibrationView->getImageData()->filename()))
    {
        saveDarkFile(calibrationData);
        subtract(calibrationData, subtractParams.targetImage, subtractParams.targetChip->getCaptureFilter(),
                 subtractParams.offsetX, subtractParams.offsetY);
    }
    else
    {
        delete calibrationData;
        emit darkFrameCompleted(false);
        emit newLog(i18n("Warning: Cannot load calibration file %1", calibrationView->getImageData()->filename()));
    }
}
}
