/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2012 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
**
** GNU Lesser General Public License Usage
**
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this file.
** Please review the following information to ensure the GNU Lesser General
** Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** Other Usage
**
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**************************************************************************/

#include "qmladapter.h"

#include "qmlengine.h"
#include "qmlv8debuggerclient.h"
#include "qscriptdebuggerclient.h"

#include <qmldebug/qdebugmessageclient.h>
#include <utils/qtcassert.h>

#include <QDebug>

using namespace QmlDebug;

namespace Debugger {
namespace Internal {

QmlAdapter::QmlAdapter(DebuggerEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_qmlClient(0)
    , m_conn(0)
    , m_msgClient(0)
{
    m_connectionTimer.setInterval(4000);
    m_connectionTimer.setSingleShot(true);
    connect(&m_connectionTimer, SIGNAL(timeout()), SLOT(checkConnectionState()));

    m_conn = new QmlDebugConnection(this);
    connect(m_conn, SIGNAL(stateChanged(QAbstractSocket::SocketState)),
            SLOT(connectionStateChanged()));
    connect(m_conn, SIGNAL(error(QAbstractSocket::SocketError)),
            SLOT(connectionErrorOccurred(QAbstractSocket::SocketError)));

    createDebuggerClients();
    m_msgClient = new QDebugMessageClient(m_conn);
    connect(m_msgClient, SIGNAL(newStatus(QmlDebug::ClientStatus)),
            this, SLOT(clientStatusChanged(QmlDebug::ClientStatus)));

}

QmlAdapter::~QmlAdapter()
{
}

void QmlAdapter::beginConnectionTcp(const QString &address, quint16 port)
{
    if (m_engine.isNull()
            || (m_conn && m_conn->state() != QAbstractSocket::UnconnectedState))
        return;

    showConnectionStatusMessage(tr("Connecting to debug server %1:%2").arg(address).arg(
                                    QString::number(port)));
    m_conn->connectToHost(address, port);

    //A timeout to check the connection state
    m_connectionTimer.start();
}

void QmlAdapter::beginConnectionOst(const QString &channel)
{
    if (m_engine.isNull()
            || (m_conn && m_conn->state() != QAbstractSocket::UnconnectedState))
        return;

    showConnectionStatusMessage(tr("Connecting to debug server on %1").arg(channel));
    m_conn->connectToOst(channel);

    //A timeout to check the connection state
    m_connectionTimer.start();
}

void QmlAdapter::closeConnection()
{
    if (m_connectionTimer.isActive()) {
        m_connectionTimer.stop();
    } else {
        if (m_conn) {
            m_conn->close();
        }
    }
}

void QmlAdapter::connectionErrorOccurred(QAbstractSocket::SocketError socketError)
{
    showConnectionStatusMessage(tr("Error: (%1) %2", "%1=error code, %2=error message")
                                .arg(socketError).arg(m_conn->errorString()));

    // this is only an error if we are already connected and something goes wrong.
    if (isConnected()) {
        emit connectionError(socketError);
    } else {
        m_connectionTimer.stop();
        emit connectionStartupFailed();
    }
}

void QmlAdapter::clientStatusChanged(QmlDebug::ClientStatus status)
{
    QString serviceName;
    float version = 0;
    if (QmlDebugClient *client = qobject_cast<QmlDebugClient*>(sender())) {
        serviceName = client->name();
        version = client->serviceVersion();
    }

    logServiceStatusChange(serviceName, version, status);
}

void QmlAdapter::debugClientStatusChanged(QmlDebug::ClientStatus status)
{
    if (status != QmlDebug::Enabled)
        return;
    QmlDebugClient *client = qobject_cast<QmlDebugClient*>(sender());
    QTC_ASSERT(client, return);

    m_qmlClient = qobject_cast<BaseQmlDebuggerClient *>(client);
    m_qmlClient->startSession();
}

void QmlAdapter::connectionStateChanged()
{
    switch (m_conn->state()) {
    case QAbstractSocket::UnconnectedState:
    {
        showConnectionStatusMessage(tr("disconnected.\n\n"));
        emit disconnected();

        break;
    }
    case QAbstractSocket::HostLookupState:
        showConnectionStatusMessage(tr("resolving host..."));
        break;
    case QAbstractSocket::ConnectingState:
        showConnectionStatusMessage(tr("connecting to debug server..."));
        break;
    case QAbstractSocket::ConnectedState:
    {
        showConnectionStatusMessage(tr("connected.\n"));

        m_connectionTimer.stop();

        //reloadEngines();
        emit connected();
        break;
    }
    case QAbstractSocket::ClosingState:
        showConnectionStatusMessage(tr("closing..."));
        break;
    case QAbstractSocket::BoundState:
    case QAbstractSocket::ListeningState:
        break;
    }
}

void QmlAdapter::checkConnectionState()
{
    if (!isConnected()) {
        closeConnection();
        emit connectionStartupFailed();
    }
}

bool QmlAdapter::isConnected() const
{
    return m_conn && m_qmlClient && m_conn->state() == QAbstractSocket::ConnectedState;
}

void QmlAdapter::createDebuggerClients()
{
    QScriptDebuggerClient *debugClient1 = new QScriptDebuggerClient(m_conn);
    connect(debugClient1, SIGNAL(newStatus(QmlDebug::ClientStatus)),
            this, SLOT(clientStatusChanged(QmlDebug::ClientStatus)));
    connect(debugClient1, SIGNAL(newStatus(QmlDebug::ClientStatus)),
            this, SLOT(debugClientStatusChanged(QmlDebug::ClientStatus)));

    QmlV8DebuggerClient *debugClient2 = new QmlV8DebuggerClient(m_conn);
    connect(debugClient2, SIGNAL(newStatus(QmlDebug::ClientStatus)),
            this, SLOT(clientStatusChanged(QmlDebug::ClientStatus)));
    connect(debugClient2, SIGNAL(newStatus(QmlDebug::ClientStatus)),
            this, SLOT(debugClientStatusChanged(QmlDebug::ClientStatus)));

    m_debugClients.insert(debugClient1->name(),debugClient1);
    m_debugClients.insert(debugClient2->name(),debugClient2);

    debugClient1->setEngine((QmlEngine*)(m_engine.data()));
    debugClient2->setEngine((QmlEngine*)(m_engine.data()));
}

QmlDebugConnection *QmlAdapter::connection() const
{
    return m_conn;
}

DebuggerEngine *QmlAdapter::debuggerEngine() const
{
    return m_engine.data();
}

void QmlAdapter::showConnectionStatusMessage(const QString &message)
{
    if (!m_engine.isNull())
        m_engine.data()->showMessage(QLatin1String("QML Debugger: ") + message, LogStatus);
}

void QmlAdapter::showConnectionErrorMessage(const QString &message)
{
    if (!m_engine.isNull())
        m_engine.data()->showMessage(QLatin1String("QML Debugger: ") + message, LogError);
}

BaseQmlDebuggerClient *QmlAdapter::activeDebuggerClient() const
{
    return m_qmlClient;
}

QHash<QString, BaseQmlDebuggerClient*> QmlAdapter::debuggerClients() const
{
    return m_debugClients;
}

QDebugMessageClient *QmlAdapter::messageClient() const
{
    return m_msgClient;
}

void QmlAdapter::logServiceStatusChange(const QString &service, float version,
                                        QmlDebug::ClientStatus newStatus)
{
    switch (newStatus) {
    case QmlDebug::Unavailable: {
        showConnectionStatusMessage(tr("Status of '%1' Version: %2 changed to 'unavailable'.").
                                    arg(service).arg(QString::number(version)));
        break;
    }
    case QmlDebug::Enabled: {
        showConnectionStatusMessage(tr("Status of '%1' Version: %2 changed to 'enabled'.").
                                    arg(service).arg(QString::number(version)));
        break;
    }

    case QmlDebug::NotConnected: {
        showConnectionStatusMessage(tr("Status of '%1' Version: %2 changed to 'not connected'.").
                                    arg(service).arg(QString::number(version)));
        break;
    }
    }
}

void QmlAdapter::logServiceActivity(const QString &service, const QString &logMessage)
{
    if (!m_engine.isNull())
        m_engine.data()->showMessage(service + QLatin1Char(' ') + logMessage, LogDebug);
}

} // namespace Internal
} // namespace Debugger
