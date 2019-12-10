/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
** Copyright (C) 2015 Nikolay Nizov <nizovn@gmail.com>
**
** This file is part of the QtPositioning module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qgeopositioninfosource_luneos_p.h"

#include <QtCore/QDateTime>
#include <QJsonValue>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>

#include <QCoreApplication>
#include <QDebug>
#include <glib.h>

#define MINIMUM_UPDATE_INTERVAL 1000

//#define Q_LOCATION_LUNEOS_DEBUG 1

QGeoPositionInfoSourceLuneOS::QGeoPositionInfoSourceLuneOS(QObject *parent)
:   QGeoPositionInfoSource(parent), m_running(false)
{

    try {
        QString serviceName("qtpositioning_");
        serviceName += QCoreApplication::applicationName();
        mHandle = LS::registerService(serviceName.toUtf8().constData());
        mHandle.attachToLoop(g_main_context_default());
    }
    catch (LS::Error &error) {
        qWarning("Failed to register service handle: %s", error.what());
        m_error = UnknownSourceError;
        Q_EMIT QGeoPositionInfoSource::error(m_error);
    }

    m_requestTimer.setSingleShot(true);
    QObject::connect(&m_requestTimer, SIGNAL(timeout()), this, SLOT(requestTimeout()));

    setPreferredPositioningMethods(AllPositioningMethods);
}

QGeoPositionInfoSourceLuneOS::~QGeoPositionInfoSourceLuneOS()
{
#ifdef Q_LOCATION_LUNEOS_DEBUG
    qDebug() << Q_FUNC_INFO;
#endif
    mTrackingCall.cancel();
    mRequestCall.cancel();
}

void QGeoPositionInfoSourceLuneOS::setUpdateInterval(int msec)
{
    QGeoPositionInfoSource::setUpdateInterval(qMax(minimumUpdateInterval(), msec));
}

void QGeoPositionInfoSourceLuneOS::setPreferredPositioningMethods(PositioningMethods methods)
{
    Q_UNUSED(methods);

    PositioningMethods previousPreferredPositioningMethods = preferredPositioningMethods();
    if (previousPreferredPositioningMethods == preferredPositioningMethods())
        return;

    QGeoPositionInfoSource::setPreferredPositioningMethods(supportedPositioningMethods());
}

QGeoPositionInfo QGeoPositionInfoSourceLuneOS::lastKnownPosition(bool fromSatellitePositioningMethodsOnly) const
{
    if (fromSatellitePositioningMethodsOnly) {
            return QGeoPositionInfo();
    }
    return m_lastPosition;
}

QGeoPositionInfoSourceLuneOS::PositioningMethods QGeoPositionInfoSourceLuneOS::supportedPositioningMethods() const
{
    return NonSatellitePositioningMethods;
}

void QGeoPositionInfoSourceLuneOS::startUpdates()
{
#ifdef Q_LOCATION_LUNEOS_DEBUG
    qDebug() << Q_FUNC_INFO;
#endif
    if (m_running) {
#ifdef Q_LOCATION_LUNEOS_DEBUG
        qDebug() << "QGeoPositionInfoSourceLuneOS already running";
#endif
        return;
    }

    m_running = true;

    QJsonObject request;
    request.insert("subscribe", true);
    QString payload = QJsonDocument(request).toJson();

    try {
        mTrackingCall = mHandle.callMultiReply("luna://org.webosports.service.location/startTracking",
                                           payload.toUtf8().constData());
        mTrackingCall.continueWith(cbProcessResults, this);
    }
    catch (LS::Error &error) {
        qWarning("Failed to startTracking: %s", error.what());
        m_error = UnknownSourceError;
        Q_EMIT QGeoPositionInfoSource::error(m_error);
    }

    // Emit last known position on start
    if (m_lastPosition.isValid()) {
        QMetaObject::invokeMethod(this, "positionUpdated", Qt::QueuedConnection,
                                  Q_ARG(QGeoPositionInfo, m_lastPosition));
    }
}

int QGeoPositionInfoSourceLuneOS::minimumUpdateInterval() const
{
    return MINIMUM_UPDATE_INTERVAL;
}

void QGeoPositionInfoSourceLuneOS::stopUpdates()
{
#ifdef Q_LOCATION_LUNEOS_DEBUG
    qDebug() << Q_FUNC_INFO;
#endif

    mTrackingCall.cancel();
    mRequestCall.cancel();
    m_running = false;

    if (m_requestTimer.isActive())
        m_requestTimer.stop();
}

void QGeoPositionInfoSourceLuneOS::requestUpdate(int timeout)
{
#ifdef Q_LOCATION_LUNEOS_DEBUG
    qDebug() << Q_FUNC_INFO;
#endif

    if (timeout < minimumUpdateInterval() && timeout != 0) {
        Q_EMIT updateTimeout();
        return;
    }

    if (m_requestTimer.isActive()) {
#ifdef Q_LOCATION_LUNEOS_DEBUG
        qDebug() << "QGeoPositionInfoSourceLuneOS request timer was active, ignoring requestUpdate.";
#endif
        return;
    }

    timeout = timeout? : MINIMUM_UPDATE_INTERVAL;
    m_requestTimer.start(timeout);

    try {
        mRequestCall = mHandle.callOneReply("luna://org.webosports.service.location/getCurrentPosition", "{}");

        mRequestCall.continueWith(cbProcessResults, this);
        mRequestCall.setTimeout(timeout);
    }
    catch (LS::Error &error) {
        qWarning("Failed to getCurrentPosition: %s", error.what());
        m_error = UnknownSourceError;
        Q_EMIT QGeoPositionInfoSource::error(m_error);
    }
}

void QGeoPositionInfoSourceLuneOS::requestTimeout()
{
#ifdef Q_LOCATION_LUNEOS_DEBUG
    qDebug() << "QGeoPositionInfoSourceLuneOS requestUpdate timeout occurred.";
#endif

    Q_EMIT updateTimeout();
}

QGeoPositionInfoSource::Error QGeoPositionInfoSourceLuneOS::error() const
{
    return m_error;
}

bool QGeoPositionInfoSourceLuneOS::cbProcessResults(LSHandle *handle, LSMessage *message, void *context)
{
    Q_UNUSED(handle);

#ifdef Q_LOCATION_LUNEOS_DEBUG
    qDebug() << Q_FUNC_INFO << LSMessageGetPayload(message);
#endif
    QGeoPositionInfoSourceLuneOS *instance = static_cast<QGeoPositionInfoSourceLuneOS*>(context);

    QJsonObject response = QJsonDocument::fromJson(LSMessageGetPayload(message)).object();

    bool success = response.value("returnValue").toBool();
    if (!success) {
        instance->m_error = UnknownSourceError;
        Q_EMIT instance->QGeoPositionInfoSource::error(instance->m_error);
        return true;
    }

    double latitude = response.value("latitude").toDouble(qQNaN());
    double longitude = response.value("longitude").toDouble(qQNaN());
    double altitude = response.value("altitude").toDouble(qQNaN());
    int timestamp = response.value("timestamp").toInt(QDateTime::currentMSecsSinceEpoch());
    QDateTime qtimestamp;
    qtimestamp.setTime_t(timestamp);

    QGeoPositionInfo position = QGeoPositionInfo(QGeoCoordinate(latitude, longitude, altitude), qtimestamp);

    double horizontalAccuracy = response.value("horizAccuracy").toDouble(qQNaN());
    double verticalAccuracy = response.value("vertAccuracy").toDouble(qQNaN());
    double velocity = response.value("velocity").toDouble(qQNaN());
    double direction = response.value("heading").toDouble(qQNaN());

    if (!qIsNaN(horizontalAccuracy) && horizontalAccuracy != -1)
        position.setAttribute(QGeoPositionInfo::HorizontalAccuracy, horizontalAccuracy);
    if (!qIsNaN(verticalAccuracy) && verticalAccuracy != -1)
        position.setAttribute(QGeoPositionInfo::VerticalAccuracy, verticalAccuracy);
    if (!qIsNaN(velocity) && velocity != -1)
        position.setAttribute(QGeoPositionInfo::GroundSpeed, velocity);
    if (!qIsNaN(direction) && direction != -1)
        position.setAttribute(QGeoPositionInfo::Direction, direction);

    if (position.isValid()) {
        instance->m_lastPosition = position;
        Q_EMIT instance->positionUpdated(position);
    }

    return true;
}

//QT_END_NAMESPACE
