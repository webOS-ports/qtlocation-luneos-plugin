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

/*
 * com.webos.service.location/getLocationUpdates schema limits, see
 * JSCEHMA_GET_LOCATION_UPDATES in the service's LunaLocationServiceUtil.h.
 * The schema is STRICT_SCHEMA, so sending any key outside it - or a value
 * outside these bounds - makes the whole call fail validation rather than
 * being ignored.
 */
#define MAXIMUM_MINIMUM_INTERVAL 3600000
#define MAXIMUM_RESPONSE_TIMEOUT 720

//#define Q_LOCATION_LUNEOS_DEBUG 1

QGeoPositionInfoSourceLuneOS::QGeoPositionInfoSourceLuneOS(QObject *parent)
:   QGeoPositionInfoSource(parent), m_running(false), m_error(NoError)
{
    /*
     * Register anonymously. This plugin is a library loaded into arbitrary
     * application processes, so it must not claim a bus name of its own: the
     * name it used to take ("qtpositioning_" + applicationName) appears in no
     * role file's allowedNames, and com.webos.service.location gates
     * getLocationUpdates behind the location.query ACG. An anonymous handle
     * inherits the hosting application's role instead, so the app's own
     * requiredPermissions decide whether the call is allowed - which is where
     * that decision belongs.
     */
    m_requestTimer.setSingleShot(true);
    QObject::connect(&m_requestTimer, SIGNAL(timeout()), this, SLOT(requestTimeout()));

    bool registered = false;

    try {
        mHandle = LS::registerService(nullptr);
        registered = true;
    }
    catch (LS::Error &error) {
        qWarning("Failed to register anonymous service handle: %s", error.what());
    }

    if (!registered) {
        // Fall back to the historical named registration, for hub
        // configurations that refuse anonymous clients.
        try {
            QString serviceName("qtpositioning_");
            serviceName += QCoreApplication::applicationName();
            mHandle = LS::registerService(serviceName.toUtf8().constData());
            registered = true;
        }
        catch (LS::Error &error) {
            qWarning("Failed to register service handle: %s", error.what());
        }
    }

    if (!registered) {
        m_error = UnknownSourceError;
        Q_EMIT QGeoPositionInfoSource::errorOccurred(m_error);
        return;
    }

    mHandle.attachToLoop(g_main_context_default());

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
    QGeoPositionInfoSource::setPreferredPositioningMethods(methods & supportedPositioningMethods());
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
    /*
     * com.webos.service.location arbitrates between a GPS handler (the nyx GPS
     * device) and a network handler, so both families are reachable through it.
     */
    return AllPositioningMethods;
}

/*
 * Map the requested positioning methods onto the service's "Handler" argument.
 * Returns an empty string when both families are wanted, in which case the key
 * is omitted and the service defaults to its HYBRID handler.
 */
QString QGeoPositionInfoSourceLuneOS::handlerForPreferredMethods() const
{
    const PositioningMethods methods = preferredPositioningMethods();

    /*
     * These enumerators are masks rather than single bits
     * (SatellitePositioningMethods is 0x000000ff, NonSatellitePositioningMethods
     * is 0xffffff00), so they have to be tested with a bitwise AND - testFlag()
     * would demand every bit of the mask.
     */
    const bool satellite = (methods & SatellitePositioningMethods) != 0;
    const bool nonSatellite = (methods & NonSatellitePositioningMethods) != 0;

    if (satellite && !nonSatellite)
        return QStringLiteral("gps");
    if (nonSatellite && !satellite)
        return QStringLiteral("network");

    return QString();
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
    request.insert("minimumInterval",
                   qBound(0, updateInterval(), MAXIMUM_MINIMUM_INTERVAL));

    const QString handler = handlerForPreferredMethods();
    if (!handler.isEmpty())
        request.insert("Handler", handler);

    QString payload = QJsonDocument(request).toJson();

    try {
        mTrackingCall = mHandle.callMultiReply("luna://com.webos.service.location/getLocationUpdates",
                                           payload.toUtf8().constData());
        mTrackingCall.continueWith(cbProcessResults, this);
    }
    catch (LS::Error &error) {
        qWarning("Failed to start getLocationUpdates: %s", error.what());
        m_error = UnknownSourceError;
        Q_EMIT QGeoPositionInfoSource::errorOccurred(m_error);
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
        Q_EMIT QGeoPositionInfoSource::errorOccurred(QGeoPositionInfoSource::UpdateTimeoutError);
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

    /*
     * responseTimeout is in seconds, while Qt hands us milliseconds. Round up
     * so a sub-second request still asks the service for at least one second,
     * and stay inside the schema's 720s ceiling.
     */
    const int responseTimeout = qBound(0, (timeout + 999) / 1000, MAXIMUM_RESPONSE_TIMEOUT);

    QJsonObject request;
    request.insert("subscribe", false);
    request.insert("responseTimeout", responseTimeout);

    const QString handler = handlerForPreferredMethods();
    if (!handler.isEmpty())
        request.insert("Handler", handler);

    QString payload = QJsonDocument(request).toJson();

    try {
        mRequestCall = mHandle.callOneReply("luna://com.webos.service.location/getLocationUpdates",
                                            payload.toUtf8().constData());

        mRequestCall.continueWith(cbProcessResults, this);
        mRequestCall.setTimeout(timeout);
    }
    catch (LS::Error &error) {
        qWarning("Failed to request getLocationUpdates: %s", error.what());
        m_error = UnknownSourceError;
        Q_EMIT QGeoPositionInfoSource::errorOccurred(m_error);
    }
}

void QGeoPositionInfoSourceLuneOS::requestTimeout()
{
#ifdef Q_LOCATION_LUNEOS_DEBUG
    qDebug() << "QGeoPositionInfoSourceLuneOS requestUpdate timeout occurred.";
#endif

    mRequestCall.cancel();

    Q_EMIT QGeoPositionInfoSource::errorOccurred(QGeoPositionInfoSource::UpdateTimeoutError);
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
        qWarning("com.webos.service.location returned an error: %d %s",
                 response.value("errorCode").toInt(),
                 qPrintable(response.value("errorText").toString()));
        instance->m_error = UnknownSourceError;
        Q_EMIT instance->QGeoPositionInfoSource::errorOccurred(instance->m_error);
        return true;
    }

    /*
     * The subscription acknowledgement carries returnValue but no fix, so only
     * treat a reply as a position once it actually has coordinates.
     */
    if (!response.contains("latitude") || !response.contains("longitude"))
        return true;

    double latitude = response.value("latitude").toDouble(qQNaN());
    double longitude = response.value("longitude").toDouble(qQNaN());
    double altitude = response.value("altitude").toDouble(qQNaN());

    /*
     * timestamp is int64 milliseconds since the epoch (see
     * location_util_add_pos_json). It must not go through toInt(), which is
     * 32-bit and silently truncates, nor through setSecsSinceEpoch().
     */
    qint64 timestamp = static_cast<qint64>(
        response.value("timestamp").toDouble(QDateTime::currentMSecsSinceEpoch()));
    QDateTime qtimestamp;
    qtimestamp.setMSecsSinceEpoch(timestamp);

    QGeoPositionInfo position = QGeoPositionInfo(QGeoCoordinate(latitude, longitude, altitude), qtimestamp);

    double horizontalAccuracy = response.value("horizAccuracy").toDouble(qQNaN());
    double verticalAccuracy = response.value("vertAccuracy").toDouble(qQNaN());
    double speed = response.value("speed").toDouble(qQNaN());
    double direction = response.value("direction").toDouble(qQNaN());

    if (!qIsNaN(horizontalAccuracy) && horizontalAccuracy != -1)
        position.setAttribute(QGeoPositionInfo::HorizontalAccuracy, horizontalAccuracy);
    if (!qIsNaN(verticalAccuracy) && verticalAccuracy != -1)
        position.setAttribute(QGeoPositionInfo::VerticalAccuracy, verticalAccuracy);
    if (!qIsNaN(speed) && speed != -1)
        position.setAttribute(QGeoPositionInfo::GroundSpeed, speed);
    if (!qIsNaN(direction) && direction != -1)
        position.setAttribute(QGeoPositionInfo::Direction, direction);

    if (position.isValid()) {
        instance->m_error = NoError;
        instance->m_lastPosition = position;

        /*
         * A fix satisfies any requestUpdate() in flight, so cancel its timer
         * before emitting - otherwise it fires afterwards and reports a
         * spurious UpdateTimeoutError for a request that succeeded.
         */
        if (instance->m_requestTimer.isActive())
            instance->m_requestTimer.stop();

        Q_EMIT instance->positionUpdated(position);
    }

    return true;
}

//QT_END_NAMESPACE
