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

#ifndef MORSE_CONNECTION_HPP
#define MORSE_CONNECTION_HPP

#include <TelepathyQt/BaseConnection>
#include <TelepathyQt/RequestableChannelClassSpec>
#include <TelepathyQt/RequestableChannelClassSpecList>

#include <TelegramQt/ConnectionApi>
#include <TelegramQt/TelegramNamespace>

#include "identifier.hpp"

class CAppInformation;
class CFileManager;

namespace Telegram {

namespace Client {

class AuthOperation;
class Client;
class ContactList;
class DialogList;
class MessagesOperation;

} // Client namespace

} // Telegram namespace

struct MorseDialogState {
    quint32 lastMessageId = 0;
};

class MorseConnection : public Tp::BaseConnection
{
    Q_OBJECT
public:
    MorseConnection(const QDBusConnection &dbusConnection,
            const QString &cmName, const QString &protocolName,
            const QVariantMap &parameters);
    ~MorseConnection();

    static Tp::AvatarSpec avatarDetails();
    static Tp::SimpleStatusSpecMap getSimpleStatusSpecMap();
    static Tp::RequestableChannelClassSpecList getRequestableChannelList();

    void doConnect(Tp::DBusError *error);
    void signInOrUp();

    QStringList inspectHandles(uint handleType, const Tp::UIntList &handles, Tp::DBusError *error);
    Tp::BaseChannelPtr createChannelCB(const QVariantMap &request, Tp::DBusError *error);

    Tp::UIntList requestHandles(uint handleType, const QStringList &identifiers, Tp::DBusError *error);

    Tp::ContactAttributesMap getContactListAttributes(const QStringList &interfaces, bool hold, Tp::DBusError *error);
    Tp::ContactAttributesMap getContactAttributes(const Tp::UIntList &handles, const QStringList &interfaces, Tp::DBusError *error);

    void requestSubscription(const Tp::UIntList &handles, const QString &message, Tp::DBusError *error);
    void removeContacts(const Tp::UIntList &handles, Tp::DBusError *error);

    Tp::ContactInfoFieldList requestContactInfo(uint handle, Tp::DBusError *error);
    Tp::ContactInfoFieldList getUserInfo(const quint32 userId) const;
    Tp::ContactInfoMap getContactInfo(const Tp::UIntList &contacts, Tp::DBusError *error);

    Tp::AliasMap getAliases(const Tp::UIntList &handles, Tp::DBusError *error = nullptr);
    void setAliases(const Tp::AliasMap &aliases, Tp::DBusError *error);

    QString getAlias(uint handle);
    QString getAlias(const MorseIdentifier identifier);

    Tp::SimplePresence getPresence(uint handle);
    uint setPresence(const QString &status, const QString &message, Tp::DBusError *error);

    uint ensureHandle(const MorseIdentifier &identifier);
    uint ensureContact(quint32 userId);
    uint ensureContact(const MorseIdentifier &identifier);
    uint ensureChat(const MorseIdentifier &identifier);

    Telegram::Client::Client *core() const { return m_client; }

    void updateDialogLastMessageId(const Telegram::Peer &peer, quint32 lastMessageId);
    MorseDialogState getDialogState(const Telegram::Peer &peer) const;

public slots:
    void onMessageReceived(const Telegram::Peer peer, quint32 messageId);
    void onMessagesReceived(const Telegram::Peer peer, const QVector<quint32> &messageIds);
    void onChatChanged(quint32 chatId);
    void setContactStatus(quint32 userId, TelegramNamespace::ContactStatus status);

signals:
    void chatDetailsChanged(quint32 chatId, const Tp::UIntList &handles);

private slots:
    void onConnectionStatusChanged(Telegram::Client::ConnectionApi::Status status,
                                   Telegram::Client::ConnectionApi::StatusReason reason);
    void onAuthenticated();
    void onSelfUserAvailable();
    void onAuthErrorReceived(TelegramNamespace::UnauthorizedError errorCode, const QString &errorMessage);
    void onAuthCodeRequired();
    void onAuthCodeCheckFailed(int status);
    void onPasswordRequired();
    void onPasswordCheckFailed();
    void onSignInFinished();
    void onCheckInFinished(Telegram::Client::AuthOperation *checkInOperation);
    void onConnectionReady();
    void onContactListChanged();
    void onDialogsReady();
    void onDisconnected();
    void onFileRequestCompleted(const QString &uniqueId);
    void onHistoryReceived(Telegram::Client::MessagesOperation *operation);

    /* Channel.Type.RoomList */
    void onGotRooms();

protected:
    Tp::BaseChannelPtr createRoomListChannel();

private:
    bool peerIsRoom(const Telegram::Peer peer) const;

    uint getHandle(const MorseIdentifier &identifier) const;
    uint getChatHandle(const MorseIdentifier &identifier) const;
    uint addContacts(const QVector<MorseIdentifier> &identifiers);

    void updateContactsStatus(const QVector<MorseIdentifier> &identifiers);
    void updateSelfContactState(Tp::ConnectionStatus status);
    void setSubscriptionState(const QVector<MorseIdentifier> &identifiers, const QVector<uint> &handles, uint state);

    void startMechanismWithData_authCode(const QString &mechanism, const QByteArray &data, Tp::DBusError *error);
    void startMechanismWithData_password(const QString &mechanism, const QByteArray &data, Tp::DBusError *error);

    /* Connection.Interface.Avatars */
    Tp::AvatarTokenMap getKnownAvatarTokens(const Tp::UIntList &contacts, Tp::DBusError *error);
    void requestAvatars(const Tp::UIntList &contacts, Tp::DBusError *error);

    /* Channel.Type.RoomList */
    void roomListStartListing(Tp::DBusError *error);
    void roomListStopListing(Tp::DBusError *error);

    bool coreIsReady();
    bool coreIsAuthenticated();

    void checkConnected();

    void loadState();
    void saveState();
    QString getAccountDataDirectory() const;

    Tp::BaseConnectionContactsInterfacePtr contactsIface;
    Tp::BaseConnectionSimplePresenceInterfacePtr simplePresenceIface;
    Tp::BaseConnectionContactListInterfacePtr contactListIface;
    Tp::BaseConnectionContactInfoInterfacePtr contactInfoIface;
    Tp::BaseConnectionAliasingInterfacePtr aliasingIface;
    Tp::BaseConnectionAvatarsInterfacePtr avatarsIface;
    Tp::BaseConnectionAddressingInterfacePtr addressingIface;
    Tp::BaseConnectionRequestsInterfacePtr requestsIface;
    Tp::BaseChannelSASLAuthenticationInterfacePtr saslIface_authCode;
    Tp::BaseChannelSASLAuthenticationInterfacePtr saslIface_password;
    Tp::BaseChannelRoomListTypePtr roomListChannel;

    QString m_wantedPresence;

    QVector<quint32> m_contactList;
    QMap<uint, MorseIdentifier> m_handles;
    QMap<uint, MorseIdentifier> m_chatHandles;
    /* Maps a contact handle to its subscription state */
    QHash<uint, uint> m_contactsSubscription;
    QHash<QString,Telegram::Peer> m_peerPictureRequests;
    QHash<Telegram::Peer, MorseDialogState> m_dialogsState;

    CAppInformation *m_appInfo = nullptr;
    Telegram::Client::Client *m_client = nullptr;
    Telegram::Client::AuthOperation *m_signOperation = nullptr;
    Telegram::Client::DialogList *m_dialogs = nullptr;
    Telegram::Client::ContactList *m_contacts = nullptr;
    CFileManager *m_fileManager = nullptr;

    int m_authReconnectionsCount = 0;

    QString m_selfPhone;
    QString m_serverAddress;
    QString m_serverKeyFile;
    uint m_serverPort = 0;
    uint m_keepAliveInterval;
};

#endif // MORSE_CONNECTION_HPP
