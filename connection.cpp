/*
    This file is part of the telepathy-morse connection manager.
    Copyright (C) 2014-2016 Alexandr Akulich <akulichalexander@gmail.com>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "connection.hpp"

#include "textchannel.hpp"

#if TP_QT_VERSION < TP_QT_VERSION_CHECK(0, 9, 8)
#include "contactgroups.hpp"
#endif

#include <TelegramQt/CAppInformation>
#include <TelegramQt/CTelegramCore>
#include <TelegramQt/Debug>

#include <TelepathyQt/Constants>
#include <TelepathyQt/BaseChannel>

#include <QDebug>

#define INSECURE_SAVE

#ifdef INSECURE_SAVE

#if QT_VERSION >= 0x050000
#include <QStandardPaths>
#else
#include <QDesktopServices>
#endif // QT_VERSION >= 0x050000

#define DIALOGS_AS_CONTACTLIST

#include <QDir>
#include <QFile>

#include "extras/CFileManager.hpp"

static const QString secretsDirPath = QLatin1String("/secrets/");
#endif // INSECURE_SAVE

static const QString c_onlineSimpleStatusKey = QLatin1String("available");
static const QString c_saslMechanismTelepathyPassword = QLatin1String("X-TELEPATHY-PASSWORD");

Tp::SimpleStatusSpecMap MorseConnection::getSimpleStatusSpecMap()
{
    //Presence
    Tp::SimpleStatusSpec spOffline;
    spOffline.type = Tp::ConnectionPresenceTypeOffline;
    spOffline.maySetOnSelf = true;
    spOffline.canHaveMessage = false;

    Tp::SimpleStatusSpec spAvailable;
    spAvailable.type = Tp::ConnectionPresenceTypeAvailable;
    spAvailable.maySetOnSelf = true;
    spAvailable.canHaveMessage = false;

    Tp::SimpleStatusSpec spHidden;
    spHidden.type = Tp::ConnectionPresenceTypeHidden;
    spHidden.maySetOnSelf = true;
    spHidden.canHaveMessage = false;

    Tp::SimpleStatusSpec spUnknown;
    spUnknown.type = Tp::ConnectionPresenceTypeUnknown;
    spUnknown.maySetOnSelf = false;
    spUnknown.canHaveMessage = false;

    Tp::SimpleStatusSpecMap specs;
    specs.insert(QLatin1String("offline"), spOffline);
    specs.insert(QLatin1String("available"), spAvailable);
    specs.insert(QLatin1String("hidden"), spHidden);
    specs.insert(QLatin1String("unknown"), spUnknown);
    return specs;
}

Tp::RequestableChannelClassSpecList MorseConnection::getRequestableChannelList()
{
    Tp::RequestableChannelClassSpecList result;

    /* Fill requestableChannelClasses */
    Tp::RequestableChannelClass personalChat;
    personalChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType")] = TP_QT_IFACE_CHANNEL_TYPE_TEXT;
    personalChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType")]  = Tp::HandleTypeContact;
    personalChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"));
    personalChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID"));
    result << Tp::RequestableChannelClassSpec(personalChat);

#ifdef ENABLE_GROUP_CHAT
    Tp::RequestableChannelClass groupChat;
    groupChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType")] = TP_QT_IFACE_CHANNEL_TYPE_TEXT;
    groupChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType")]  = Tp::HandleTypeRoom;
    groupChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"));
    groupChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID"));
    result << Tp::RequestableChannelClassSpec(groupChat);

    Tp::RequestableChannelClass chatList;
    chatList.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType")] = TP_QT_IFACE_CHANNEL_TYPE_ROOM_LIST;
    result << Tp::RequestableChannelClassSpec(chatList);
#endif // ENABLE_GROUP_CHAT

    return result;
}

MorseConnection::MorseConnection(const QDBusConnection &dbusConnection, const QString &cmName, const QString &protocolName, const QVariantMap &parameters) :
    Tp::BaseConnection(dbusConnection, cmName, protocolName, parameters),
    m_appInfo(nullptr),
    m_core(nullptr),
    m_fileManager(nullptr),
    m_passwordInfo(nullptr),
    m_authReconnectionsCount(0)
{
    qDebug() << Q_FUNC_INFO;
    /* Connection.Interface.Contacts */
    contactsIface = Tp::BaseConnectionContactsInterface::create();
    contactsIface->setGetContactAttributesCallback(Tp::memFun(this, &MorseConnection::getContactAttributes));
    contactsIface->setContactAttributeInterfaces(QStringList()
                                                 << TP_QT_IFACE_CONNECTION
                                                 << TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST
                                                 << TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_INFO
                                                 << TP_QT_IFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE
                                                 << TP_QT_IFACE_CONNECTION_INTERFACE_ALIASING
                                                 << TP_QT_IFACE_CONNECTION_INTERFACE_AVATARS);
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(contactsIface));

    /* Connection.Interface.SimplePresence */
    simplePresenceIface = Tp::BaseConnectionSimplePresenceInterface::create();
    simplePresenceIface->setStatuses(getSimpleStatusSpecMap());
    simplePresenceIface->setSetPresenceCallback(Tp::memFun(this,&MorseConnection::setPresence));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(simplePresenceIface));

    /* Connection.Interface.ContactList */
    contactListIface = Tp::BaseConnectionContactListInterface::create();
    contactListIface->setContactListPersists(true);
    contactListIface->setCanChangeContactList(true);
    contactListIface->setDownloadAtConnection(true);
    contactListIface->setGetContactListAttributesCallback(Tp::memFun(this, &MorseConnection::getContactListAttributes));
    contactListIface->setRequestSubscriptionCallback(Tp::memFun(this, &MorseConnection::requestSubscription));
    contactListIface->setRemoveContactsCallback(Tp::memFun(this, &MorseConnection::removeContacts));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(contactListIface));

    /* Connection.Interface.ContactInfo */
    contactInfoIface = Tp::BaseConnectionContactInfoInterface::create();

    Tp::FieldSpec vcardSpecPhone;
    Tp::FieldSpec vcardSpecNickname;
    Tp::FieldSpec vcardSpecName;
    vcardSpecPhone.name = QLatin1String("tel");
    vcardSpecNickname.name = QLatin1String("n");
    vcardSpecName.name = QLatin1String("nickname");
    contactInfoIface->setSupportedFields(Tp::FieldSpecs()
                                         << vcardSpecPhone
                                         << vcardSpecNickname
                                         << vcardSpecName
                                         );
    contactInfoIface->setContactInfoFlags(Tp::ContactInfoFlagPush);
    contactInfoIface->setGetContactInfoCallback(Tp::memFun(this, &MorseConnection::getContactInfo));
    contactInfoIface->setRequestContactInfoCallback(Tp::memFun(this, &MorseConnection::requestContactInfo));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(contactInfoIface));

    /* Connection.Interface.Aliasing */
    aliasingIface = Tp::BaseConnectionAliasingInterface::create();
    aliasingIface->setGetAliasesCallback(Tp::memFun(this, &MorseConnection::getAliases));
    aliasingIface->setSetAliasesCallback(Tp::memFun(this, &MorseConnection::setAliases));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(aliasingIface));

    /* Connection.Interface.Avatars */
    avatarsIface = Tp::BaseConnectionAvatarsInterface::create();
    avatarsIface->setAvatarDetails(Tp::AvatarSpec(/* supportedMimeTypes */ QStringList() << QLatin1String("image/jpeg"),
                                                  /* minHeight */ 0, /* maxHeight */ 160, /* recommendedHeight */ 160,
                                                  /* minWidth */ 0, /* maxWidth */ 160, /* recommendedWidth */ 160,
                                                  /* maxBytes */ 10240));
    avatarsIface->setGetKnownAvatarTokensCallback(Tp::memFun(this, &MorseConnection::getKnownAvatarTokens));
    avatarsIface->setRequestAvatarsCallback(Tp::memFun(this, &MorseConnection::requestAvatars));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(avatarsIface));

#ifdef ENABLE_GROUP_CHAT
# ifdef USE_BUNDLED_GROUPS_IFACE
    ConnectionContactGroupsInterfacePtr groupsIface = ConnectionContactGroupsInterface::create();
# else
    Tp::BaseConnectionContactGroupsInterfacePtr groupsIface = Tp::BaseConnectionContactGroupsInterface::create();
# endif
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(groupsIface));
#endif

    /* Connection.Interface.Requests */
    requestsIface = Tp::BaseConnectionRequestsInterface::create(this);
    requestsIface->requestableChannelClasses = getRequestableChannelList().bareClasses();

    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(requestsIface));

    m_selfPhone = parameters.value(QLatin1String("account")).toString();
    m_keepaliveInterval = parameters.value(QLatin1String("keepalive-interval"), CTelegramCore::defaultPingInterval() / 1000).toUInt();

    setConnectCallback(Tp::memFun(this, &MorseConnection::doConnect));
    setInspectHandlesCallback(Tp::memFun(this, &MorseConnection::inspectHandles));
    setCreateChannelCallback(Tp::memFun(this, &MorseConnection::createChannelCB));
    setRequestHandlesCallback(Tp::memFun(this, &MorseConnection::requestHandles));

    connect(this, SIGNAL(disconnected()), SLOT(whenDisconnected()));

    m_handles.insert(1, MorseIdentifier());
    setSelfHandle(1);

    m_appInfo = new CAppInformation(this);
    m_appInfo->setAppId(14617);
    m_appInfo->setAppHash(QLatin1String("e17ac360fd072f83d5d08db45ce9a121"));
    m_appInfo->setAppVersion(QLatin1String("0.1"));
    m_appInfo->setDeviceInfo(QLatin1String("pc"));
    m_appInfo->setOsInfo(QLatin1String("GNU/Linux"));
    m_appInfo->setLanguageCode(QLocale::system().bcp47Name());

    m_core = new CTelegramCore(this);
    m_core->setPingInterval(m_keepaliveInterval * 1000);
    m_core->setAppInformation(m_appInfo);
    m_core->setMessageReceivingFilter(TelegramNamespace::MessageFlagOut|TelegramNamespace::MessageFlagRead);
#ifndef TELEGRAMQT_VERSION
    m_core->setAcceptableMessageTypes(
                    TelegramNamespace::MessageTypeText |
                    TelegramNamespace::MessageTypePhoto |
                    TelegramNamespace::MessageTypeAudio |
                    TelegramNamespace::MessageTypeVideo |
                    TelegramNamespace::MessageTypeContact |
                    TelegramNamespace::MessageTypeDocument |
                    TelegramNamespace::MessageTypeGeo );
#endif

    connect(m_core, &CTelegramCore::connectionStateChanged,
            this, &MorseConnection::whenConnectionStateChanged);
    connect(m_core, &CTelegramCore::selfUserAvailable,
            this, &MorseConnection::onSelfUserAvailable);
    connect(m_core, &CTelegramCore::authorizationErrorReceived,
            this, &MorseConnection::onAuthErrorReceived);
    connect(m_core, &CTelegramCore::phoneCodeRequired,
            this, &MorseConnection::whenPhoneCodeRequired);
    connect(m_core, &CTelegramCore::authSignErrorReceived,
            this, &MorseConnection::whenAuthSignErrorReceived);
    connect(m_core, &CTelegramCore::passwordInfoReceived,
            this, &MorseConnection::onPasswordInfoReceived);
    connect(m_core, &CTelegramCore::contactListChanged,
            this, &MorseConnection::onContactListChanged);
    connect(m_core, &CTelegramCore::messageReceived,
             this, &MorseConnection::whenMessageReceived);
    connect(m_core, &CTelegramCore::chatChanged,
            this, &MorseConnection::whenChatChanged);
    connect(m_core, &CTelegramCore::contactStatusChanged,
            this, &MorseConnection::setContactStatus);

    const QString proxyType = parameters.value(QLatin1String("proxy-type")).toString();
    if (!proxyType.isEmpty()) {
        if (proxyType == QLatin1String("socks5")) {
            const QString proxyServer = parameters.value(QLatin1String("proxy-server")).toString();
            const quint16 proxyPort = parameters.value(QLatin1String("proxy-port")).toUInt();
            const QString proxyUsername = parameters.value(QLatin1String("proxy-username")).toString();
            const QString proxyPassword = parameters.value(QLatin1String("proxy-password")).toString();
            if (proxyServer.isEmpty() || proxyPort == 0) {
                qWarning() << "Invalid proxy configuration, ignored";
            } else {
                qDebug() << Q_FUNC_INFO << "Set proxy";
                QNetworkProxy proxy;
                proxy.setType(QNetworkProxy::Socks5Proxy);
                proxy.setHostName(proxyServer);
                proxy.setPort(proxyPort);
                proxy.setUser(proxyUsername);
                proxy.setPassword(proxyPassword);
                m_core->setProxy(proxy);
            }
        } else {
            qWarning() << "Unknown proxy type" << proxyType << ", ignored.";
        }
    }
    m_fileManager = new CFileManager(m_core, this);
    connect(m_fileManager, &CFileManager::requestComplete, this, &MorseConnection::onFileRequestCompleted);
}

MorseConnection::~MorseConnection()
{
}

void MorseConnection::doConnect(Tp::DBusError *error)
{
    Q_UNUSED(error);

    m_authReconnectionsCount = 0;
    setStatus(Tp::ConnectionStatusConnecting, Tp::ConnectionStatusReasonNoneSpecified);

    const QByteArray sessionData = getSessionData(m_selfPhone);

    if (sessionData.isEmpty()) {
        qDebug() << "init connection...";
        m_core->initConnection();
    } else {
        qDebug() << "restore connection...";
        m_core->restoreConnection(sessionData);
    }
}

void MorseConnection::whenConnectionStateChanged(TelegramNamespace::ConnectionState state)
{
    qDebug() << Q_FUNC_INFO << state;
    switch (state) {
    case TelegramNamespace::ConnectionStateAuthRequired:
        m_core->requestPhoneCode(m_selfPhone);
        break;
    case TelegramNamespace::ConnectionStateAuthenticated:
        whenAuthenticated();
        break;
    case TelegramNamespace::ConnectionStateReady:
        tryToSaveData();
        whenConnectionReady();
        updateSelfContactState(Tp::ConnectionStatusConnected);
        break;
    case TelegramNamespace::ConnectionStateDisconnected:
        if (status() == Tp::ConnectionStatusConnected) {
            setStatus(Tp::ConnectionStatusDisconnected, Tp::ConnectionStatusReasonNetworkError);
            updateSelfContactState(Tp::ConnectionStatusDisconnected);
            emit disconnected();
        }
        break;
    default:
        break;
    }
}

void MorseConnection::whenAuthenticated()
{
    qDebug() << Q_FUNC_INFO;

    if (!saslIface_authCode.isNull()) {
        saslIface_authCode->setSaslStatus(Tp::SASLStatusSucceeded, QLatin1String("Succeeded"), QVariantMap());
    }

    if (!saslIface_password.isNull()) {
        saslIface_password->setSaslStatus(Tp::SASLStatusSucceeded, QLatin1String("Succeeded"), QVariantMap());
    }

    if (m_passwordInfo) {
        delete m_passwordInfo;
        m_passwordInfo = 0;
    }

    checkConnected();
    contactListIface->setContactListState(Tp::ContactListStateWaiting);
}

void MorseConnection::onSelfUserAvailable()
{
    qDebug() << Q_FUNC_INFO;

    MorseIdentifier selfIdentifier = MorseIdentifier::fromUserId(m_core->selfId());

    m_handles.insert(1, selfIdentifier);

    int selfHandle = 1;
    setSelfContact(selfHandle, selfIdentifier.toString());

    Tp::SimpleContactPresences presences;
    Tp::SimplePresence presence;

    if (m_wantedPresence.isNull()) {
        m_wantedPresence = c_onlineSimpleStatusKey;
    }

    presence.status = m_wantedPresence;
    presence.statusMessage = QString();
    presence.type = simplePresenceIface->statuses().value(m_wantedPresence).type;
    presences[selfHandle] = presence;
    simplePresenceIface->setPresences(presences);

    checkConnected();
}

void MorseConnection::onAuthErrorReceived(TelegramNamespace::UnauthorizedError errorCode, const QString &errorMessage)
{
    qDebug() << Q_FUNC_INFO << errorCode << errorMessage;

    if (errorCode == TelegramNamespace::UnauthorizedSessionPasswordNeeded) {
        if (!saslIface_authCode.isNull()) {
            saslIface_authCode->setSaslStatus(Tp::SASLStatusSucceeded, QLatin1String("Succeeded"), QVariantMap());
        }

        m_core->getPassword();
        return;
    }

    static const int reconnectionsLimit = 1;

    if (m_authReconnectionsCount < reconnectionsLimit) {
        qDebug() << "MorseConnection::whenAuthErrorReceived(): Auth error received. Trying to re-init connection without session data..." << m_authReconnectionsCount + 1 << " attempt.";
        setStatus(Tp::ConnectionStatusConnecting, Tp::ConnectionStatusReasonAuthenticationFailed);
        ++m_authReconnectionsCount;
        m_core->closeConnection();
        m_core->initConnection();
    } else {
        qDebug() << "MorseConnection::whenAuthErrorReceived(): Auth error received. Can not connect (tried" << m_authReconnectionsCount << " times).";
        setStatus(Tp::ConnectionStatusDisconnected, Tp::ConnectionStatusReasonAuthenticationFailed);
    }
}

void MorseConnection::whenPhoneCodeRequired()
{
    qDebug() << Q_FUNC_INFO;

    Tp::DBusError error;

    //Registration
    Tp::BaseChannelPtr baseChannel = Tp::BaseChannel::create(this, TP_QT_IFACE_CHANNEL_TYPE_SERVER_AUTHENTICATION);

    Tp::BaseChannelServerAuthenticationTypePtr authType
            = Tp::BaseChannelServerAuthenticationType::create(TP_QT_IFACE_CHANNEL_INTERFACE_SASL_AUTHENTICATION);
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(authType));

    saslIface_authCode = Tp::BaseChannelSASLAuthenticationInterface::create(QStringList()
                                                                            << c_saslMechanismTelepathyPassword,
                                                                            /* hasInitialData */ true,
                                                                            /* canTryAgain */ true,
                                                                            /* authorizationIdentity */ m_selfPhone,
                                                                            /* defaultUsername */ QString(),
                                                                            /* defaultRealm */ QString(),
                                                                            /* maySaveResponse */ false);

    saslIface_authCode->setStartMechanismWithDataCallback(Tp::memFun(this, &MorseConnection::startMechanismWithData_authCode));

    baseChannel->setRequested(false);
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(saslIface_authCode));
    baseChannel->registerObject(&error);

    if (error.isValid()) {
        qDebug() << Q_FUNC_INFO << error.name() << error.message();
    } else {
        addChannel(baseChannel);
    }
}

void MorseConnection::onPasswordInfoReceived(quint64 requestId)
{
    Q_UNUSED(requestId)

    qDebug() << Q_FUNC_INFO;

    m_passwordInfo = new Telegram::PasswordInfo();
    m_core->getPasswordInfo(m_passwordInfo, requestId);

    Tp::DBusError error;

    Tp::BaseChannelPtr baseChannel = Tp::BaseChannel::create(this, TP_QT_IFACE_CHANNEL_TYPE_SERVER_AUTHENTICATION);

    Tp::BaseChannelServerAuthenticationTypePtr authType
            = Tp::BaseChannelServerAuthenticationType::create(TP_QT_IFACE_CHANNEL_INTERFACE_SASL_AUTHENTICATION);
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(authType));

    saslIface_password = Tp::BaseChannelSASLAuthenticationInterface::create(QStringList()
                                                                            << c_saslMechanismTelepathyPassword,
                                                                            /* hasInitialData */ true,
                                                                            /* canTryAgain */ true,
                                                                            /* authorizationIdentity */ m_selfPhone,
                                                                            /* defaultUsername */ QString(),
                                                                            /* defaultRealm */ QString(),
                                                                            /* maySaveResponse */ true);

    saslIface_password->setStartMechanismWithDataCallback(Tp::memFun(this, &MorseConnection::startMechanismWithData_password));

    baseChannel->setRequested(false);
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(saslIface_password));
    baseChannel->registerObject(&error);

    if (error.isValid()) {
        qDebug() << Q_FUNC_INFO << error.name() << error.message();
    } else {
        addChannel(baseChannel);
    }
}

void MorseConnection::whenAuthSignErrorReceived(TelegramNamespace::AuthSignError errorCode, const QString &errorMessage)
{
    qDebug() << Q_FUNC_INFO << errorCode << errorMessage;

    QVariantMap details;
    details[QLatin1String("server-message")] = errorMessage;

    switch (errorCode) {
    case TelegramNamespace::AuthSignErrorPhoneCodeIsExpired:
    case TelegramNamespace::AuthSignErrorPhoneCodeIsInvalid:
        if (!saslIface_authCode.isNull()) {
            saslIface_authCode->setSaslStatus(Tp::SASLStatusServerFailed, TP_QT_ERROR_AUTHENTICATION_FAILED, details);
        }
        break;
    case TelegramNamespace::AuthSignErrorPasswordHashInvalid:
        if (!saslIface_password.isNull()) {
            saslIface_password->setSaslStatus(Tp::SASLStatusServerFailed, TP_QT_ERROR_AUTHENTICATION_FAILED, details);
        }
        break;
    default:
        qWarning() << Q_FUNC_INFO << "Unhandled!";
        break;
    }
}

void MorseConnection::startMechanismWithData_authCode(const QString &mechanism, const QByteArray &data, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << mechanism << data;

    if (!saslIface_authCode->availableMechanisms().contains(mechanism)) {
        error->set(TP_QT_ERROR_NOT_IMPLEMENTED, QString(QLatin1String("Given SASL mechanism \"%1\" is not implemented")).arg(mechanism));
        return;
    }

    saslIface_authCode->setSaslStatus(Tp::SASLStatusInProgress, QLatin1String("InProgress"), QVariantMap());

    m_core->signIn(m_selfPhone, QString::fromLatin1(data.constData()));
}

void MorseConnection::startMechanismWithData_password(const QString &mechanism, const QByteArray &data, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << mechanism << data;

    if (!saslIface_password->availableMechanisms().contains(mechanism)) {
        error->set(TP_QT_ERROR_NOT_IMPLEMENTED, QString(QLatin1String("Given SASL mechanism \"%1\" is not implemented")).arg(mechanism));
        return;
    }

    saslIface_password->setSaslStatus(Tp::SASLStatusInProgress, QLatin1String("InProgress"), QVariantMap());

    m_core->tryPassword(m_passwordInfo->currentSalt(), data);
}

void MorseConnection::whenConnectionReady()
{
    qDebug() << Q_FUNC_INFO;
    m_core->setOnlineStatus(m_wantedPresence == c_onlineSimpleStatusKey);
    m_core->setMessageReceivingFilter(TelegramNamespace::MessageFlagNone);
    onContactListChanged();
}

QStringList MorseConnection::inspectHandles(uint handleType, const Tp::UIntList &handles, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << handleType << handles;

    switch (handleType) {
    case Tp::HandleTypeContact:
    case Tp::HandleTypeRoom:
        break;
    default:
        if (error) {
            error->set(TP_QT_ERROR_INVALID_ARGUMENT, QLatin1String("Unsupported handle type"));
        }
        return QStringList();
        break;
    }

    QStringList result;

    const QMap<uint, MorseIdentifier> handlesContainer = handleType == Tp::HandleTypeContact ? m_handles : m_chatHandles;

    foreach (uint handle, handles) {
        if (!handlesContainer.contains(handle)) {
            if (error) {
                error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Unknown handle"));
            }
            return QStringList();
        }

        result.append(handlesContainer.value(handle).toString());
    }

    return result;
}

Tp::BaseChannelPtr MorseConnection::createChannelCB(const QVariantMap &request, Tp::DBusError *error)
{
    const QString channelType = request.value(TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType")).toString();

    if (channelType == TP_QT_IFACE_CHANNEL_TYPE_ROOM_LIST) {
        return createRoomListChannel();
    }

    uint targetHandleType = request.value(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType")).toUInt();
    uint targetHandle = 0;
    MorseIdentifier targetID;

    switch (targetHandleType) {
    case Tp::HandleTypeContact:
        if (request.contains(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"))) {
            targetHandle = request.value(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle")).toUInt();
            targetID = m_handles.value(targetHandle);
        } else if (request.contains(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID"))) {
            targetID = MorseIdentifier::fromString(request.value(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID")).toString());
            targetHandle = ensureHandle(targetID);
        }
        break;
    case Tp::HandleTypeRoom:
        if (request.contains(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"))) {
            targetHandle = request.value(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle")).toUInt();
            targetID = m_chatHandles.value(targetHandle);
        } else if (request.contains(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID"))) {
            targetID = MorseIdentifier::fromString(request.value(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID")).toString());
            targetHandle = ensureHandle(targetID);
        }
    default:
        break;
    }

    // Looks like there is no any case for InitiatorID other than selfID
    uint initiatorHandle = 0;

    if (targetHandleType == Tp::HandleTypeContact) {
        initiatorHandle = request.value(TP_QT_IFACE_CHANNEL + QLatin1String(".InitiatorHandle"), selfHandle()).toUInt();
    }

    qDebug() << "MorseConnection::createChannel " << channelType
             << targetHandleType
             << targetHandle
             << request;

    switch (targetHandleType) {
    case Tp::HandleTypeContact:
#ifdef ENABLE_GROUP_CHAT
    case Tp::HandleTypeRoom:
#endif
        break;
    default:
        if (error) {
            error->set(TP_QT_ERROR_INVALID_ARGUMENT, QLatin1String("Unknown target handle type"));
        }
        return Tp::BaseChannelPtr();
        break;
    }

    if (!targetHandle
            || ((targetHandleType == Tp::HandleTypeContact) && !m_handles.contains(targetHandle))
            || ((targetHandleType == Tp::HandleTypeRoom) && !m_chatHandles.contains(targetHandle))
            ) {
        if (error) {
            error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Target handle is unknown."));
        }
        return Tp::BaseChannelPtr();
    }

    Tp::BaseChannelPtr baseChannel = Tp::BaseChannel::create(this, channelType, Tp::HandleType(targetHandleType), targetHandle);
    baseChannel->setTargetID(targetID.toString());
    baseChannel->setInitiatorHandle(initiatorHandle);

    if (channelType == TP_QT_IFACE_CHANNEL_TYPE_TEXT) {
        MorseTextChannelPtr textChannel = MorseTextChannel::create(this, baseChannel.data());
        baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(textChannel));

        if (targetHandleType == Tp::HandleTypeRoom) {
            connect(this, SIGNAL(chatDetailsChanged(quint32,Tp::UIntList)),
                    textChannel.data(), SLOT(whenChatDetailsChanged(quint32,Tp::UIntList)));

            whenChatChanged(targetID.id);
        }
    }

    return baseChannel;
}

Tp::UIntList MorseConnection::requestHandles(uint handleType, const QStringList &identifiers, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << identifiers;

    if (handleType != Tp::HandleTypeContact) {
        error->set(TP_QT_ERROR_INVALID_ARGUMENT, QLatin1String("MorseConnection::requestHandles - Handle Type unknown"));
        return Tp::UIntList();
    }

    Tp::UIntList result;
    foreach(const QString &identify, identifiers) {
        const MorseIdentifier id = MorseIdentifier::fromString(identify);
        if (!id.isValid()) {
            error->set(TP_QT_ERROR_INVALID_ARGUMENT, QLatin1String("MorseConnection::requestHandles - invalid identifier"));
            return Tp::UIntList();
        }
        result.append(ensureContact(id));
    }

    return result;
}

Tp::ContactAttributesMap MorseConnection::getContactListAttributes(const QStringList &interfaces, bool hold, Tp::DBusError *error)
{
    Q_UNUSED(hold);
    return getContactAttributes(m_contactList.toList(), interfaces, error);
}

Tp::ContactAttributesMap MorseConnection::getContactAttributes(const Tp::UIntList &handles, const QStringList &interfaces, Tp::DBusError *error)
{
//    http://telepathy.freedesktop.org/spec/Connection_Interface_Contacts.html#Method:GetContactAttributes
//    qDebug() << Q_FUNC_INFO << handles << interfaces;

    Tp::ContactAttributesMap contactAttributes;

    foreach (const uint handle, handles) {
        if (m_handles.contains(handle)) {
            QVariantMap attributes;
            const MorseIdentifier identifier = m_handles.value(handle);
            if (!identifier.isValid()) {
                qWarning() << Q_FUNC_INFO << "Handle is in map, but identifier is not valid";
                continue;
            }
            attributes[TP_QT_IFACE_CONNECTION + QLatin1String("/contact-id")] = identifier.toString();

            if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST)) {
                attributes[TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST + QLatin1String("/subscribe")] = m_contactsSubscription.value(handle, Tp::SubscriptionStateUnknown);
                attributes[TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST + QLatin1String("/publish")] = m_contactsSubscription.value(handle, Tp::SubscriptionStateUnknown);
            }

            if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE)) {
                attributes[TP_QT_IFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE + QLatin1String("/presence")] = QVariant::fromValue(getPresence(handle));
            }

            if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_ALIASING)) {
                attributes[TP_QT_IFACE_CONNECTION_INTERFACE_ALIASING + QLatin1String("/alias")] = QVariant::fromValue(getAlias(handle));
            }

            if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_AVATARS)) {
                attributes[TP_QT_IFACE_CONNECTION_INTERFACE_AVATARS + QLatin1String("/token")] = QVariant::fromValue(m_core->peerPictureToken(identifier));
            }

            if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_INFO)) {
                attributes[TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_INFO + QLatin1String("/info")] = QVariant::fromValue(getUserInfo(identifier.userId()));
            }

            contactAttributes[handle] = attributes;
        }
    }
    return contactAttributes;
}

void MorseConnection::requestSubscription(const Tp::UIntList &handles, const QString &message, Tp::DBusError *error)
{
//    http://telepathy.freedesktop.org/spec/Connection_Interface_Contact_List.html#Method:RequestSubscription

    Q_UNUSED(message);
    const QStringList phoneNumbers = inspectHandles(Tp::HandleTypeContact, handles, error);

    if (error->isValid()) {
        return;
    }

    if (phoneNumbers.isEmpty()) {
        error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Invalid handle(s)"));
    }

    if (!coreIsReady()) {
        error->set(TP_QT_ERROR_DISCONNECTED, QLatin1String("Disconnected"));
    }

    m_core->addContacts(phoneNumbers);
}

void MorseConnection::removeContacts(const Tp::UIntList &handles, Tp::DBusError *error)
{
    if (handles.isEmpty()) {
        error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Invalid argument (no handles provided)"));
    }

    if (!coreIsReady()) {
        error->set(TP_QT_ERROR_DISCONNECTED, QLatin1String("Disconnected"));
    }

    QVector<quint32> ids;

    foreach (uint handle, handles) {
        if (!m_handles.contains(handle)) {
            error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Unknown handle"));
            return;
        }

        quint32 id = m_handles.value(handle).userId();

        if (!id) {
            error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Internal error (invalid handle)"));
        }

        ids.append(id);
    }

    m_core->deleteContacts(ids);
}

Tp::ContactInfoFieldList MorseConnection::requestContactInfo(uint handle, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << handle;

    if (!m_handles.contains(handle)) {
        error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Invalid handle"));
        return Tp::ContactInfoFieldList();
    }
    MorseIdentifier identifier = m_handles.value(handle);
    if (!identifier.isValid()) {
        error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Invalid morse identifier"));
        return Tp::ContactInfoFieldList();
    }

    return getUserInfo(identifier.userId());
}

Tp::ContactInfoFieldList MorseConnection::getUserInfo(const quint32 userId) const
{
    Telegram::UserInfo userInfo;
    if (!m_core->getUserInfo(&userInfo, userId)) {
        return Tp::ContactInfoFieldList();
    }

    Tp::ContactInfoFieldList contactInfo;
    if (!userInfo.userName().isEmpty()) {
        Tp::ContactInfoField contactInfoField;
        contactInfoField.fieldName = QLatin1String("nickname");
        contactInfoField.fieldValue.append(userInfo.userName());
        contactInfo << contactInfoField;
    }
    if (!userInfo.phone().isEmpty()) {
        Tp::ContactInfoField contactInfoField;
        contactInfoField.fieldName = QLatin1String("tel");
        QString phone = userInfo.phone();
        if (!phone.startsWith(QLatin1Char('+'))) {
            phone.prepend(QLatin1Char('+'));
        }
        contactInfoField.parameters.append(QLatin1String("type=text"));
        contactInfoField.parameters.append(QLatin1String("type=cell"));
        contactInfoField.fieldValue.append(phone);
        contactInfo << contactInfoField;
    }

    QString name = userInfo.firstName() + QLatin1Char(' ') + userInfo.lastName();
    name = name.simplified();
    if (!name.isEmpty()) {
        Tp::ContactInfoField contactInfoField;
        contactInfoField.fieldName = QLatin1String("fn"); // Formatted name
        contactInfoField.fieldValue.append(name);
        contactInfo << contactInfoField;
    }
    {
        Tp::ContactInfoField contactInfoField;
        contactInfoField.fieldName = QLatin1String("n");
        contactInfoField.fieldValue.append(userInfo.lastName()); // "Surname"
        contactInfoField.fieldValue.append(userInfo.firstName()); // "Given"
        contactInfoField.fieldValue.append(QString()); // Additional
        contactInfoField.fieldValue.append(QString()); // Prefix
        contactInfoField.fieldValue.append(QString()); // Suffix
        contactInfo << contactInfoField;
    }

    return contactInfo;
}

Tp::ContactInfoMap MorseConnection::getContactInfo(const Tp::UIntList &contacts, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << contacts;

    if (contacts.isEmpty()) {
        return Tp::ContactInfoMap();
    }

    Tp::ContactInfoMap result;

    foreach (uint handle, contacts) {
        const Tp::ContactInfoFieldList contactInfo = requestContactInfo(handle, error);
        if (!contactInfo.isEmpty()) {
            result.insert(handle, contactInfo);
        }
    }

    return result;
}

Tp::AliasMap MorseConnection::getAliases(const Tp::UIntList &handles, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << handles;

    Tp::AliasMap aliases;

    foreach (uint handle, handles) {
        aliases[handle] = getAlias(handle);
    }

    return aliases;
}

void MorseConnection::setAliases(const Tp::AliasMap &aliases, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << aliases;
    error->set(TP_QT_ERROR_NOT_IMPLEMENTED, QLatin1String("Not implemented"));
}

QString MorseConnection::getAlias(uint handle)
{
    return getAlias(m_handles.value(handle));
}

QString MorseConnection::getAlias(const MorseIdentifier identifier)
{
    if (!identifier.isValid()) {
        return QLatin1String("Invalid alias");
    }

    if (identifier.type == Telegram::Peer::User) {
        Telegram::UserInfo info;
        if (!m_core->getUserInfo(&info, identifier.userId())) {
            return tr("Unknown name");
        }

        QString name;
        if (!info.firstName().isEmpty()) {
            name = info.firstName();
        }
        if (!info.lastName().isEmpty()) {
            if (!name.isEmpty()) {
                name += QLatin1Char(' ') + info.lastName();
            } else {
                name = info.lastName();
            }
        }

        if (!name.simplified().isEmpty()) {
            return name;
        }

        if (!info.userName().isEmpty()) {
            return info.userName();
        }
    } else {
        Telegram::ChatInfo info;
        if (!m_core->getChatInfo(&info, identifier)) {
            return tr("Unknown title");
        }
        QString name = info.title();
        if (!name.isEmpty()) {
            return name;
        }
    }

    return tr("Unknown name");
}

Tp::SimplePresence MorseConnection::getPresence(uint handle)
{
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
    return *simplePresenceIface->getPresences(Tp::UIntList() << handle).constBegin();
#else
    return simplePresenceIface->getPresences(Tp::UIntList() << handle).first();
#endif
}

uint MorseConnection::setPresence(const QString &status, const QString &message, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << status;
    Q_UNUSED(message)
    Q_UNUSED(error)

    m_wantedPresence = status;

    if (coreIsAuthenticated()) {
        m_core->setOnlineStatus(status == c_onlineSimpleStatusKey);
    }

    return 0;
}

uint MorseConnection::ensureHandle(const MorseIdentifier &identifier)
{
    if (peerIsRoom(identifier)) {
        return ensureChat(identifier);
    } else {
        return ensureContact(identifier);
    }
}

uint MorseConnection::ensureContact(quint32 userId)
{
    return ensureContact(MorseIdentifier::fromUserId(userId));
}

uint MorseConnection::ensureContact(const MorseIdentifier &identifier)
{
    uint handle = getHandle(identifier);
    if (!handle) {
        handle = addContacts( {identifier});
    }
    return handle;
}

uint MorseConnection::ensureChat(const MorseIdentifier &identifier)
{
    uint handle = getChatHandle(identifier);
    if (!handle) {
        if (m_chatHandles.isEmpty()) {
            handle = 1;
        } else {
            handle = m_chatHandles.keys().last() + 1;
        }

        m_chatHandles.insert(handle, identifier);
    }
    return handle;
}

/**
 * Add contacts with identifiers \a identifiers to known contacts list (not roster)
 *
 * \return the maximum handle value
 */
uint MorseConnection::addContacts(const QVector<MorseIdentifier> &identifiers)
{
    qDebug() << Q_FUNC_INFO;
    uint handle = 0;

    if (!m_handles.isEmpty()) {
        handle = m_handles.keys().last();
    }

    QList<uint> newHandles;
    QVector<MorseIdentifier> newIdentifiers;
    foreach (const MorseIdentifier &identifier, identifiers) {
        if (getHandle(identifier)) {
            continue;
        }

        ++handle;
        m_handles.insert(handle, identifier);
        newHandles << handle;
        newIdentifiers << identifier;
    }

    return handle;
}

void MorseConnection::updateContactsStatus(const QVector<MorseIdentifier> &identifiers)
{
    qDebug() << Q_FUNC_INFO;
    Tp::SimpleContactPresences newPresences;
    foreach (const MorseIdentifier &identifier, identifiers) {
        uint handle = ensureContact(identifier);

        if (handle == selfHandle()) {
            continue;
        }

        TelegramNamespace::ContactStatus st = TelegramNamespace::ContactStatusUnknown;

        if (m_core) {
            Telegram::UserInfo info;
            m_core->getUserInfo(&info, identifier.userId());

            st = info.status();
        }

        Tp::SimplePresence presence;

        switch (st) {
        case TelegramNamespace::ContactStatusOnline:
            presence.status = QLatin1String("available");
            presence.type = Tp::ConnectionPresenceTypeAvailable;
            break;
        case TelegramNamespace::ContactStatusOffline:
            presence.status = QLatin1String("offline");
            presence.type = Tp::ConnectionPresenceTypeOffline;
            break;
        default:
        case TelegramNamespace::ContactStatusUnknown:
            presence.status = QLatin1String("unknown");
            presence.type = Tp::ConnectionPresenceTypeUnknown;
            break;
        }

        newPresences[handle] = presence;
    }
    simplePresenceIface->setPresences(newPresences);
}

void MorseConnection::updateSelfContactState(Tp::ConnectionStatus status)
{
    Tp::SimpleContactPresences newPresences;
    Tp::SimplePresence presence;
    if (status == Tp::ConnectionStatusConnected) {
        presence.status = QLatin1String("available");
        presence.type = Tp::ConnectionPresenceTypeAvailable;
    } else {
        presence.status = QLatin1String("offline");
        presence.type = Tp::ConnectionPresenceTypeOffline;
    }

    newPresences[selfHandle()] = presence;
    simplePresenceIface->setPresences(newPresences);
}

void MorseConnection::setSubscriptionState(const QVector<MorseIdentifier> &identifiers, const QVector<uint> &handles, uint state)
{
    qDebug() << Q_FUNC_INFO;
    Tp::ContactSubscriptionMap changes;
    Tp::HandleIdentifierMap identifiersMap;

    for(int i = 0; i < identifiers.size(); ++i) {
        Tp::ContactSubscriptions change;
        change.publish = Tp::SubscriptionStateYes;
        change.publishRequest = QString();
        change.subscribe = state;
        changes[handles[i]] = change;
        identifiersMap[handles[i]] = identifiers[i].toString();
        m_contactsSubscription[handles[i]] = state;
    }
    Tp::HandleIdentifierMap removals;
    contactListIface->contactsChangedWithID(changes, identifiersMap, removals);
}

/* Receive message from outside (telegram server) */
void MorseConnection::whenMessageReceived(const Telegram::Message &message)
{
    bool groupChatMessage = peerIsRoom(message.peer());
    uint targetHandle = ensureHandle(message.peer());

    //TODO: initiator should be group creator
    Tp::DBusError error;
    bool yours;

    QVariantMap request;
    request[TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType")] = TP_QT_IFACE_CHANNEL_TYPE_TEXT;
    request[TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle")] = targetHandle;
    request[TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType")] = groupChatMessage ? Tp::HandleTypeRoom : Tp::HandleTypeContact;
    request[TP_QT_IFACE_CHANNEL + QLatin1String(".InitiatorHandle")] = targetHandle;

#if TP_QT_VERSION >= TP_QT_VERSION_CHECK(0, 9, 8)
    Tp::BaseChannelPtr channel;
    if (message.fromId == m_core->selfId()) {
        channel = getExistingChannel(request, &error);
        if (channel.isNull()) {
            return;
        }
    } else {
        channel = ensureChannel(request, yours, /* suppressHandler */ false, &error);
    }
#else
    Tp::BaseChannelPtr channel = ensureChannel(request, yours, /* suppressHandler */ false, &error);
#endif

    if (error.isValid()) {
        qWarning() << Q_FUNC_INFO << "ensureChannel failed:" << error.name() << " " << error.message();
        return;
    }

    MorseTextChannelPtr textChannel = MorseTextChannelPtr::dynamicCast(channel->interface(TP_QT_IFACE_CHANNEL_TYPE_TEXT));

    if (!textChannel) {
        qDebug() << Q_FUNC_INFO << "Error, channel is not a morseTextChannel?";
        return;
    }

    textChannel->onMessageReceived(message);
}

void MorseConnection::whenChatChanged(quint32 chatId)
{
    QVector<quint32> participants;
    if (m_core->getChatParticipants(&participants, chatId) && !participants.isEmpty()) {

        Tp::UIntList handles;
        foreach (quint32 participant, participants) {
            handles.append(ensureHandle(MorseIdentifier::fromUserId(participant)));
        }

        emit chatDetailsChanged(chatId, handles);
    }
}

void MorseConnection::onContactListChanged()
{
    if (!coreIsReady()) {
        return;
    }
#ifdef DIALOGS_AS_CONTACTLIST
    const QVector<Telegram::Peer> ids = m_core->dialogs();
#else
    const QVector<quint32> ids = m_core->contactList();
#endif

    qDebug() << Q_FUNC_INFO << ids;

    QVector<uint> newContactListHandles;
    QVector<MorseIdentifier> newContactListIdentifiers;
    newContactListHandles.reserve(ids.count());
    newContactListIdentifiers.reserve(ids.count());

#ifdef DIALOGS_AS_CONTACTLIST
    for (const Telegram::Peer peer : ids) {
        if (peerIsRoom(peer)) {
            continue;
        }
        Telegram::UserInfo info;
        if (peer.type == Telegram::Peer::User) {
            m_core->getUserInfo(&info, peer.id);
            if (info.isDeleted()) {
                qDebug() << Q_FUNC_INFO << "skip deleted user id" << peer.id;
                continue;
            }
        }
        newContactListIdentifiers.append(peer);
#else
    for (quint32 id : ids) {
        newContactListIdentifiers.append(MorseIdentifier::fromUserId(id));
#endif
        newContactListHandles.append(ensureContact(newContactListIdentifiers.last()));
    }

    Tp::HandleIdentifierMap removals;
    foreach (uint handle, m_contactList) {
        if (newContactListHandles.contains(handle)) {
            continue;
        }
        const MorseIdentifier identifier = m_handles.value(handle);
        if (!identifier.isValid()) {
            qWarning() << Q_FUNC_INFO << "Internal corruption. Handle" << handle << "has invalid corresponding identifier";
            removals.insert(handle, identifier.toString());
        }
    }

    m_contactList = newContactListHandles;

    qDebug() << Q_FUNC_INFO << newContactListIdentifiers;
    Tp::ContactSubscriptionMap changes;
    Tp::HandleIdentifierMap identifiersMap;

    for(int i = 0; i < newContactListIdentifiers.size(); ++i) {
        Tp::ContactSubscriptions change;
        change.publish = Tp::SubscriptionStateYes;
        change.subscribe = Tp::SubscriptionStateYes;
        changes[newContactListHandles[i]] = change;
        identifiersMap[newContactListHandles[i]] = newContactListIdentifiers.at(i).toString();
        m_contactsSubscription[newContactListHandles[i]] = Tp::SubscriptionStateYes;
    }

    contactListIface->contactsChangedWithID(changes, identifiersMap, removals);

    updateContactsStatus(newContactListIdentifiers);

    contactListIface->setContactListState(Tp::ContactListStateSuccess);
}

void MorseConnection::whenDisconnected()
{
    qDebug() << Q_FUNC_INFO;

    m_core->setOnlineStatus(false); // TODO: Real disconnect
    tryToSaveData();
    setStatus(Tp::ConnectionStatusDisconnected, Tp::ConnectionStatusReasonRequested);
}

void MorseConnection::onFileRequestCompleted(const QString &uniqueId)
{
    qDebug() << Q_FUNC_INFO << uniqueId;
    if (m_peerPictureRequests.contains(uniqueId)) {
        const Telegram::Peer peer = m_peerPictureRequests.value(uniqueId);
        if (!peerIsRoom(peer)) {
            const FileInfo *fileInfo = m_fileManager->getFileInfo(uniqueId);
            avatarsIface->avatarRetrieved(peer.id, uniqueId, fileInfo->data(), fileInfo->mimeType());
        } else {
            qWarning() << "MorseConnection::onFileRequestCompleted(): Ignore room picture";
        }
    } else {
        qWarning() << "MorseConnection::onFileRequestCompleted(): Unexpected file id";
    }
}

void MorseConnection::whenGotRooms()
{
    qDebug() << Q_FUNC_INFO;
    Tp::RoomInfoList rooms;

    const QVector<Telegram::Peer> dialogs = m_core->dialogs();
    for(const Telegram::Peer peer : dialogs) {
        if (!peerIsRoom(peer)) {
            continue;
        }
        Telegram::ChatInfo chatInfo;
        if (!m_core->getChatInfo(&chatInfo, peer.id)) {
            continue;
        }
        if (chatInfo.migratedTo().isValid()) {
            continue;
        }
        const MorseIdentifier chatID = peer;
        Tp::RoomInfo roomInfo;
        roomInfo.channelType = TP_QT_IFACE_CHANNEL_TYPE_TEXT;
        roomInfo.handle = ensureChat(chatID);
        roomInfo.info[QLatin1String("handle-name")] = chatID.toString();
        roomInfo.info[QLatin1String("members-only")] = true;
        roomInfo.info[QLatin1String("invite-only")] = true;
        roomInfo.info[QLatin1String("password")] = false;
        roomInfo.info[QLatin1String("name")] = chatInfo.title();
        roomInfo.info[QLatin1String("members")] = chatInfo.participantsCount();
        rooms << roomInfo;
    }

    roomListChannel->gotRooms(rooms);
    roomListChannel->setListingRooms(false);
}

Tp::BaseChannelPtr MorseConnection::createRoomListChannel()
{
    qDebug() << Q_FUNC_INFO;
    Tp::BaseChannelPtr baseChannel = Tp::BaseChannel::create(this, TP_QT_IFACE_CHANNEL_TYPE_ROOM_LIST);

    roomListChannel = Tp::BaseChannelRoomListType::create();
    roomListChannel->setListRoomsCallback(Tp::memFun(this, &MorseConnection::roomListStartListing));
    roomListChannel->setStopListingCallback(Tp::memFun(this, &MorseConnection::roomListStopListing));
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(roomListChannel));

    return baseChannel;
}

Tp::AvatarTokenMap MorseConnection::getKnownAvatarTokens(const Tp::UIntList &contacts, Tp::DBusError *error)
{
    if (contacts.isEmpty()) {
        error->set(TP_QT_ERROR_INVALID_ARGUMENT, QLatin1String("No handles provided"));
    }

    if (!coreIsAuthenticated()) {
        error->set(TP_QT_ERROR_DISCONNECTED, QLatin1String("Disconnected"));
    }

    Tp::AvatarTokenMap result;
    foreach (quint32 handle, contacts) {
        if (!m_handles.contains(handle)) {
            error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Invalid handle(s)"));
        }
        result.insert(handle, m_core->peerPictureToken(m_handles.value(handle)));
    }

    return result;
}

void MorseConnection::requestAvatars(const Tp::UIntList &contacts, Tp::DBusError *error)
{
    if (contacts.isEmpty()) {
        error->set(TP_QT_ERROR_INVALID_ARGUMENT, QLatin1String("No handles provided"));
    }

    if (!coreIsAuthenticated()) {
        error->set(TP_QT_ERROR_DISCONNECTED, QLatin1String("Disconnected"));
    }

    foreach (quint32 handle, contacts) {
        if (!m_handles.contains(handle)) {
            error->set(TP_QT_ERROR_INVALID_HANDLE, QLatin1String("Invalid handle(s)"));
        }
        const Telegram::Peer peer = m_handles.value(handle);
        Telegram::RemoteFile pictureFile;
        m_fileManager->getPeerPictureFileInfo(peer, &pictureFile);
        const QString requestId = pictureFile.getUniqueId();
        const FileInfo *fileInfo = m_fileManager->getFileInfo(requestId);
        if (fileInfo && fileInfo->isComplete()) {
            const QByteArray data = m_fileManager->getData(requestId);
            if (!data.isEmpty()) {
                // I don't see an easy way to delay the invocation; emit the signal synchronously for now. Should not be a problem for a good client.
                avatarsIface->avatarRetrieved(handle, requestId, data, fileInfo->mimeType());
            }
            continue;
        }
        const QString newRequestId = m_fileManager->requestFile(pictureFile);
        if (newRequestId != requestId) {
            qWarning() << "Unexpected request id!" << newRequestId << "(expected:" << requestId;
        }
        m_peerPictureRequests.insert(newRequestId, peer);
    }
}

void MorseConnection::roomListStartListing(Tp::DBusError *error)
{
    Q_UNUSED(error)

    QTimer::singleShot(0, this, SLOT(whenGotRooms()));
    roomListChannel->setListingRooms(true);
}

void MorseConnection::roomListStopListing(Tp::DBusError *error)
{
    Q_UNUSED(error)
    roomListChannel->setListingRooms(false);
}

bool MorseConnection::coreIsReady()
{
    return m_core && (m_core->connectionState() == TelegramNamespace::ConnectionStateReady);
}

bool MorseConnection::coreIsAuthenticated()
{
    return m_core && (m_core->connectionState() >= TelegramNamespace::ConnectionStateAuthenticated);
}

void MorseConnection::checkConnected()
{
    if (coreIsAuthenticated() && m_handles.value(selfHandle()).isValid()) {
        setStatus(Tp::ConnectionStatusConnected, Tp::ConnectionStatusReasonRequested);
    }
}

QByteArray MorseConnection::getSessionData(const QString &phone)
{
#ifdef INSECURE_SAVE

#if QT_VERSION >= 0x050000
    QFile secretFile(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + secretsDirPath + phone);
#else // QT_VERSION >= 0x050000
    QFile secretFile(QDesktopServices::storageLocation(QDesktopServices::CacheLocation) + secretsDirPath + phone);
#endif // QT_VERSION >= 0x050000

    if (secretFile.open(QIODevice::ReadOnly)) {
        const QByteArray data = secretFile.readAll();
        qDebug() << Q_FUNC_INFO << phone << "(" << data.size() << "bytes)";
        return data;
    }
    qDebug() << Q_FUNC_INFO << "Unable to open file" << "for account" << phone;
#endif // INSECURE_SAVE

    return QByteArray();
}

bool MorseConnection::saveSessionData(const QString &phone, const QByteArray &data)
{
#ifdef INSECURE_SAVE
    QDir dir;
#if QT_VERSION >= 0x050000
    dir.mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + secretsDirPath);
    QFile secretFile(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + secretsDirPath + phone);
#else // QT_VERSION >= 0x050000
    dir.mkpath(QDesktopServices::storageLocation(QDesktopServices::CacheLocation) + secretsDirPath);
    QFile secretFile(QDesktopServices::storageLocation(QDesktopServices::CacheLocation) + secretsDirPath + phone);
#endif // QT_VERSION >= 0x050000

    if (secretFile.open(QIODevice::WriteOnly)) {
        qDebug() << Q_FUNC_INFO << phone << "(" << data.size() << "bytes)";
        return secretFile.write(data) == data.size();
    }
    qWarning() << Q_FUNC_INFO << "Unable to save the session data to file" << "for account" << phone;
#endif // INSECURE_SAVE

    return false;
}

void MorseConnection::tryToSaveData()
{
    qDebug() << Q_FUNC_INFO;
    if (m_core->connectionState() == TelegramNamespace::ConnectionStateReady) {
        qDebug() << "Session is ready";
        saveSessionData(m_selfPhone, m_core->connectionSecretInfo());
    }
}

bool MorseConnection::peerIsRoom(const Telegram::Peer peer) const
{
    if (peer.type == Telegram::Peer::User) {
        return false;
    }
    if (peer.type == Telegram::Peer::Channel) {
        Telegram::ChatInfo info;
        if (m_core->getChatInfo(&info, peer)) {
            if (info.broadcast()) {
                return false;
            }
        }
    }
    return true;
}

void MorseConnection::setContactStatus(quint32 userId, TelegramNamespace::ContactStatus status)
{
    qDebug() << "Update presence for " << userId << "to" << status;

    Tp::SimpleContactPresences newPresences;
    uint handle = ensureContact(MorseIdentifier::fromUserId(userId));

    if (handle == selfHandle()) {
        return;
    }

    Tp::SimplePresence presence;

    switch (status) {
    case TelegramNamespace::ContactStatusOnline:
        presence.status = QLatin1String("available");
        presence.type = Tp::ConnectionPresenceTypeAvailable;
        break;
    case TelegramNamespace::ContactStatusOffline:
        presence.status = QLatin1String("offline");
        presence.type = Tp::ConnectionPresenceTypeOffline;
        break;
    default:
    case TelegramNamespace::ContactStatusUnknown:
        presence.status = QLatin1String("unknown");
        presence.type = Tp::ConnectionPresenceTypeUnknown;
        break;
    }

    newPresences[handle] = presence;

    simplePresenceIface->setPresences(newPresences);
}

uint MorseConnection::getHandle(const MorseIdentifier &identifier) const
{
    return m_handles.key(identifier, 0);
}

uint MorseConnection::getChatHandle(const MorseIdentifier &identifier) const
{
    return m_chatHandles.key(identifier, 0);
}
