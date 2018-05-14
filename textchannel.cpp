/*
    This file is part of the telepathy-morse connection manager.
    Copyright (C) 2016 Alexandr Akulich <akulichalexander@gmail.com>

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

#include "textchannel.hpp"
#include "connection.hpp"

#include <TelegramQt/CTelegramCore>

#include <TelepathyQt/Constants>
#include <TelepathyQt/RequestableChannelClassSpec>
#include <TelepathyQt/RequestableChannelClassSpecList>
#include <TelepathyQt/Types>

#include <QVariantMap>
#include <QDateTime>
#include <QTimer>

QString userToVCard(const Telegram::UserInfo &userInfo)
{
    QStringList result;
    result.append(QStringLiteral("BEGIN:VCARD"));
    result.append(QStringLiteral("VERSION:4.0"));
    QString name = userInfo.firstName() + QLatin1Char(' ') + userInfo.lastName();
    name = name.simplified();
    if (name.isEmpty()) {
        return QString();
    }
    result.append(QStringLiteral("FN:") + name);
    if (!userInfo.phone().isEmpty()) {
        // TEL;VALUE=uri;TYPE=cell:tel:+33-01-23-45-67
       result.append(QStringLiteral("TEL;PREF:tel+") + userInfo.phone());
    }
    // N:Family Names (surnames);Given Names;Additional Names;Honorific Prefixes;Honorific Suffixes
    // N:Stevenson;John;Philip,Paul;Dr.;Jr.,M.D.,A.C.P.
    // N:Smith;John;;;
    result.append(QStringLiteral("N:") + userInfo.lastName() + QLatin1Char(';') + userInfo.firstName() + QStringLiteral(";;;"));
    result.append(QStringLiteral("END:VCARD"));

    return result.join(QStringLiteral("\r\n"));
}

MorseTextChannel::MorseTextChannel(MorseConnection *morseConnection, Tp::BaseChannel *baseChannel)
    : Tp::BaseChannelTextType(baseChannel),
      m_connection(morseConnection),
      m_client(morseConnection->client()),
      m_targetHandle(baseChannel->targetHandle()),
      m_targetHandleType(baseChannel->targetHandleType()),
      m_targetID(MorseIdentifier::fromString(baseChannel->targetID())),
      m_localTypingTimer(0)
{
    QStringList supportedContentTypes = QStringList()
            << QLatin1String("text/plain")
            << QLatin1String("text/vcard")
            << QLatin1String("application/geo+json")
               ;
    Tp::UIntList messageTypes = Tp::UIntList() << Tp::ChannelTextMessageTypeNormal << Tp::ChannelTextMessageTypeDeliveryReport;

    uint messagePartSupportFlags = 0;
    uint deliveryReportingSupport = Tp::DeliveryReportingSupportFlagReceiveSuccesses|Tp::DeliveryReportingSupportFlagReceiveRead;

    setMessageAcknowledgedCallback(Tp::memFun(this, &MorseTextChannel::messageAcknowledgedCallback));

    m_messagesIface = Tp::BaseChannelMessagesInterface::create(this,
                                                               supportedContentTypes,
                                                               messageTypes,
                                                               messagePartSupportFlags,
                                                               deliveryReportingSupport);

    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(m_messagesIface));
    m_messagesIface->setSendMessageCallback(Tp::memFun(this, &MorseTextChannel::sendMessageCallback));

    m_chatStateIface = Tp::BaseChannelChatStateInterface::create();
    m_chatStateIface->setSetChatStateCallback(Tp::memFun(this, &MorseTextChannel::setChatState));
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(m_chatStateIface));

    if (m_targetHandleType == Tp::HandleTypeContact) {
        connect(m_client, SIGNAL(contactMessageActionChanged(quint32,TelegramNamespace::MessageAction)),
                SLOT(whenContactChatStateComposingChanged(quint32,TelegramNamespace::MessageAction)));
    } else if (m_targetHandleType == Tp::HandleTypeRoom) {
#ifdef ENABLE_GROUP_CHAT
        Tp::ChannelGroupFlags groupFlags = Tp::ChannelGroupFlagProperties;

        // Permissions:
        groupFlags |= Tp::ChannelGroupFlagCanAdd;

        m_groupIface = Tp::BaseChannelGroupInterface::create();
        m_groupIface->setGroupFlags(groupFlags);
        m_groupIface->setSelfHandle(m_connection->selfHandle());
        baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(m_groupIface));

        Telegram::ChatInfo info;

        m_client->getChatInfo(&info, m_targetID.id);

        QDateTime creationTimestamp;
        if (info.date()) {
            creationTimestamp.setTime_t(info.date());
        }

        m_roomIface = Tp::BaseChannelRoomInterface::create(/* roomName */ m_targetID.toString(),
                                                           /* server */ QString(),
                                                           /* creator */ QString(),
                                                           /* creatorHandle */ 0,
                                                           creationTimestamp);

        baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(m_roomIface));

        m_roomConfigIface = Tp::BaseChannelRoomConfigInterface::create();
        baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(m_roomConfigIface));
        connect(m_client, SIGNAL(contactChatMessageActionChanged(quint32,quint32,TelegramNamespace::MessageAction)),
                SLOT(whenContactRoomStateComposingChanged(quint32,quint32,TelegramNamespace::MessageAction)));
#endif
    }

    connect(m_client, SIGNAL(messageReadInbox(Telegram::Peer,quint32)),
            SLOT(setMessageInboxRead(Telegram::Peer,quint32)));
    connect(m_client, SIGNAL(messageReadOutbox(Telegram::Peer,quint32)),
            SLOT(setMessageOutboxRead(Telegram::Peer,quint32)));
    connect(m_client, SIGNAL(sentMessageIdReceived(quint64,quint32)),
            SLOT(setResolvedMessageId(quint64,quint32)));
}

MorseTextChannelPtr MorseTextChannel::create(MorseConnection *morseConnection, Tp::BaseChannel *baseChannel)
{
    return MorseTextChannelPtr(new MorseTextChannel(morseConnection, baseChannel));
}

MorseTextChannel::~MorseTextChannel()
{
}

QString MorseTextChannel::sendMessageCallback(const Tp::MessagePartList &messageParts, uint flags, Tp::DBusError *error)
{
    QString content;
    foreach (const Tp::MessagePart &part, messageParts) {
        if (part.contains(QLatin1String("content-type"))
                && part.value(QLatin1String("content-type")).variant().toString() == QLatin1String("text/plain")
                && part.contains(QLatin1String("content"))) {
            content = part.value(QLatin1String("content")).variant().toString();
            break;
        }
    }

    quint64 tmpId = m_client->sendMessage(m_targetID, content);
    m_sentMessageIds.append(SentMessageId(tmpId));

    return QString::number(tmpId);
}

void MorseTextChannel::messageAcknowledgedCallback(const QString &messageId)
{
    m_client->setMessageRead(m_targetID, messageId.toUInt());
}

void MorseTextChannel::whenContactChatStateComposingChanged(quint32 userId, TelegramNamespace::MessageAction action)
{
    // We are connected to broadcast signal, so have to select only needed calls
    const MorseIdentifier identifier = MorseIdentifier::fromUserId(userId);
    if (identifier != m_targetID) {
        return;
    }
    setMessageAction(userId, action);
}

void MorseTextChannel::whenContactRoomStateComposingChanged(quint32 chatId, quint32 userId, TelegramNamespace::MessageAction action)
{
    // We are connected to broadcast signal, so have to select only needed calls
    const MorseIdentifier identifier = MorseIdentifier::fromChatId(chatId);
    if (identifier != m_targetID) {
        return;
    }
    setMessageAction(userId, action);
}

void MorseTextChannel::setMessageAction(quint32 userId, TelegramNamespace::MessageAction action)
{
    const uint handle = m_connection->ensureContact(userId);
    if (action) {
        m_chatStateIface->chatStateChanged(handle, Tp::ChannelChatStateComposing);
    } else {
        m_chatStateIface->chatStateChanged(handle, Tp::ChannelChatStateActive);
    }
}

void MorseTextChannel::onMessageReceived(const Telegram::Message &message)
{
    Tp::MessagePartList partList;
    Tp::MessagePart header;

    const QString token = QString::number(message.id);
    header[QLatin1String("message-token")] = QDBusVariant(token);
    header[QLatin1String("message-type")]  = QDBusVariant(Tp::ChannelTextMessageTypeNormal);
    header[QLatin1String("message-sent")]  = QDBusVariant(message.timestamp);

    bool broadcast = false;
    if (m_targetID.type == Telegram::Peer::Channel) {
        Telegram::ChatInfo info;
        if (!m_client->getChatInfo(&info, m_targetID.id)) {
            qWarning() << "Unable to get chat info" << m_targetID.toString();
        }
        broadcast = info.broadcast();
    }

    if (broadcast) {
        header[QLatin1String("message-sender")]    = QDBusVariant(m_targetHandle);
        header[QLatin1String("message-sender-id")] = QDBusVariant(m_targetID.toString());
    } else if (message.flags & TelegramNamespace::MessageFlagOut) {
        header[QLatin1String("message-sender")]    = QDBusVariant(m_connection->selfHandle());
        header[QLatin1String("message-sender-id")] = QDBusVariant(m_connection->selfID());
    } else {
        const MorseIdentifier senderId = MorseIdentifier::fromUserId(message.fromId);
        header[QLatin1String("message-sender")]    = QDBusVariant(m_connection->ensureHandle(senderId));
        header[QLatin1String("message-sender-id")] = QDBusVariant(senderId.toString());
    }

    // messageReceived signal is always emitted before maxMessageId update, so
    // the message is a new one, if its id is bigger, than the last known message id,
    // This works for both, In and Out messages.
    const bool scrollback = message.id <= m_client->maxMessageId();
    if (scrollback) {
        header[QLatin1String("scrollback")] = QDBusVariant(true);
        // Telegram has no timestamp for message read, only sent.
        // Fallback to the message sent timestamp to keep received messages in chronological order.
        // Alternatively, client can sort messages in order of message-sent.
        header[QLatin1String("message-received")]  = QDBusVariant(message.timestamp);
    } else {
        uint currentTimestamp = QDateTime::currentMSecsSinceEpoch() / 1000;
        header[QLatin1String("message-received")]  = QDBusVariant(currentTimestamp);
    }
    partList << header;

    Tp::MessagePartList body;
    if (!message.text.isEmpty()) {
        Tp::MessagePart text;
        text[QLatin1String("content-type")] = QDBusVariant(QLatin1String("text/plain"));
        text[QLatin1String("content")] = QDBusVariant(message.text);
        body << text;
    }

    if (message.type != TelegramNamespace::MessageTypeText) { // More, than a plain text message
        Telegram::MessageMediaInfo info;
        m_client->getMessageMediaInfo(&info, message.id, message.peer());

        bool handled = true;
        switch (message.type) {
        case TelegramNamespace::MessageTypeGeo: {
            static const QString jsonTemplate = QLatin1String("{\"type\":\"point\",\"coordinates\":[%1, %2]}");
            Tp::MessagePart geo;
            geo[QLatin1String("content-type")] = QDBusVariant(QLatin1String("application/geo+json"));
            geo[QLatin1String("alternative")] = QDBusVariant(QLatin1String("multimedia"));
            geo[QLatin1String("content")] = QDBusVariant(jsonTemplate.arg(info.latitude()).arg(info.longitude()));
            body << geo;
        }
            break;
        case TelegramNamespace::MessageTypeContact: {
            Telegram::UserInfo userInfo;
            if (!info.getContactInfo(&userInfo)) {
                qWarning() << Q_FUNC_INFO << "Unable to get user info from contact media message" << message.id;
                break;
            }

            QString data = userToVCard(userInfo);
            if (data.isEmpty()) {
                qWarning() << Q_FUNC_INFO << "Unable to get user vcard from user info from message" << message.id;
                break;
            }
            Tp::MessagePart userVCardPart;
            userVCardPart[QLatin1String("content-type")] = QDBusVariant(QLatin1String("text/vcard"));
            userVCardPart[QLatin1String("alternative")] = QDBusVariant(QLatin1String("multimedia"));
            userVCardPart[QLatin1String("content")] = QDBusVariant(data);
            body << userVCardPart;
        }
            break;
        case TelegramNamespace::MessageTypeWebPage: {
            Tp::MessagePart webPart;
            webPart[QLatin1String("interface")] = QDBusVariant(TP_QT_IFACE_CHANNEL + QLatin1String(".Interface.WebPage"));
            webPart[QLatin1String("alternative")] = QDBusVariant(QLatin1String("multimedia"));
            webPart[QLatin1String("title")] = QDBusVariant(info.title());
            webPart[QLatin1String("url")] = QDBusVariant(info.url());
            webPart[QLatin1String("displayUrl")] = QDBusVariant(info.displayUrl());
            webPart[QLatin1String("siteName")] = QDBusVariant(info.siteName());
            webPart[QLatin1String("description")] = QDBusVariant(info.description());
            body << webPart;
        }
            break;
        default:
            handled = false;
            break;
        }

        Tp::MessagePart textMessage;
        textMessage[QLatin1String("content-type")] = QDBusVariant(QLatin1String("text/plain"));
        textMessage[QLatin1String("alternative")] = QDBusVariant(QLatin1String("multimedia"));

        if (info.alt().isEmpty()) {
            const QString notHandledText = tr("Telepathy-Morse doesn't support this type of multimedia messages yet.");
            const QString badAlternativeText = tr("Telepathy client doesn't support this type of multimedia messages.");
            const QString notSupportedText = handled ? badAlternativeText : notHandledText;
            if (body.isEmpty()) {// There is no text part
                textMessage[QLatin1String("content")] = QDBusVariant(notSupportedText);
            } else { // There is a text part, so we need to add the notSupportedText on a new line
                textMessage[QLatin1String("content")] = QDBusVariant(QLatin1Char('\n') + notSupportedText);
            }
        } else {
            textMessage[QLatin1String("content")] = QDBusVariant(info.alt());
        }

        body << textMessage;
    }

    partList << body;
    addReceivedMessage(partList);
}

void MorseTextChannel::updateChatParticipants(const Tp::UIntList &handles)
{
#ifdef ENABLE_GROUP_CHAT
    m_groupIface->setMembers(handles, /* details */ QVariantMap());
#else
    Q_UNUSED(handles)
#endif
}

void MorseTextChannel::whenChatDetailsChanged(quint32 chatId, const Tp::UIntList &handles)
{
    qDebug() << Q_FUNC_INFO << chatId;

    if ((m_targetID.chatId() == chatId) || (m_targetID.channelId() == chatId)) {
        updateChatParticipants(handles);

        Telegram::ChatInfo info;
        if (m_client->getChatInfo(&info, chatId)) {
            m_roomConfigIface->setTitle(info.title());
            m_roomConfigIface->setConfigurationRetrieved(true);
        }
    }
}

void MorseTextChannel::setMessageInboxRead(Telegram::Peer peer, quint32 messageId)
{
    // We are connected to broadcast signal, so have to select only needed calls
    if (m_targetID != peer) {
        return;
    }

    // TODO: Mark *all* messages up to this as read
    QStringList tokens;

    foreach (const Tp::MessagePartList &message, pendingMessages()) {
        if (message.isEmpty()) {
            // Invalid message
            continue;
        }
        const Tp::MessagePart &header = message.front();
        bool ok;
        const QString token = header.value(QLatin1String("message-token")).variant().toString();
        quint32 mId = token.toUInt(&ok);
        if (!ok) {
            // Invalid message token
            continue;
        }

        if (mId <= messageId) {
            tokens.append(token);
        }
    }

#if TP_QT_VERSION >= TP_QT_VERSION_CHECK(0, 9, 8)
    Tp::DBusError error;
    acknowledgePendingMessages(tokens, &error);
#endif
}

void MorseTextChannel::setMessageOutboxRead(Telegram::Peer peer, quint32 messageId)
{
    // We are connected to broadcast signal, so have to select only needed calls
    if (m_targetID != peer) {
        return;
    }

    quint64 id = messageId;
    foreach (const SentMessageId &info, m_sentMessageIds) {
        if (info.id == messageId) {
            id = info.randomId;
            break;
        }
    }

    // TODO: Mark *all* messages up to this as read

    const QString token = QString::number(id);

    Tp::MessagePartList partList;

    Tp::MessagePart header;
    header[QLatin1String("message-sender")]    = QDBusVariant(m_connection->selfHandle());
    header[QLatin1String("message-sender-id")] = QDBusVariant(m_connection->selfID());
    header[QLatin1String("message-type")]      = QDBusVariant(Tp::ChannelTextMessageTypeDeliveryReport);
    header[QLatin1String("delivery-status")]   = QDBusVariant(Tp::DeliveryStatusRead);
    header[QLatin1String("delivery-token")]    = QDBusVariant(token);
    partList << header;

    addReceivedMessage(partList);
}

void MorseTextChannel::setResolvedMessageId(quint64 randomId, quint32 resolvedId)
{
    int index = m_sentMessageIds.indexOf(SentMessageId(randomId));
    if (index < 0) {
        return;
    }

    m_sentMessageIds[index].id = resolvedId;

    const QString token = QString::number(randomId);

    Tp::MessagePartList partList;

    Tp::MessagePart header;
    header[QLatin1String("message-sender")]    = QDBusVariant(m_targetHandle);
    header[QLatin1String("message-sender-id")] = QDBusVariant(m_targetID.toString());
    header[QLatin1String("message-type")]      = QDBusVariant(Tp::ChannelTextMessageTypeDeliveryReport);
    header[QLatin1String("delivery-status")]   = QDBusVariant(Tp::DeliveryStatusAccepted);
    header[QLatin1String("delivery-token")]    = QDBusVariant(token);
    partList << header;

    addReceivedMessage(partList);
}

void MorseTextChannel::reactivateLocalTyping()
{
    m_client->setTyping(m_targetID, TelegramNamespace::MessageActionTyping);
}

void MorseTextChannel::setChatState(uint state, Tp::DBusError *error)
{
    Q_UNUSED(error);

    if (!m_localTypingTimer) {
        m_localTypingTimer = new QTimer(this);
        m_localTypingTimer->setInterval(CTelegramCore::localTypingRecommendedRepeatInterval());
        connect(m_localTypingTimer, SIGNAL(timeout()), this, SLOT(reactivateLocalTyping()));
    }

    if (state == Tp::ChannelChatStateComposing) {
        m_client->setTyping(m_targetID, TelegramNamespace::MessageActionTyping);
        m_localTypingTimer->start();
    } else {
        m_client->setTyping(m_targetID, TelegramNamespace::MessageActionNone);
        m_localTypingTimer->stop();
    }
}
