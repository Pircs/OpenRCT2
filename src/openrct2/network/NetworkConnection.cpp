/*****************************************************************************
 * Copyright (c) 2014-2020 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#ifndef DISABLE_NETWORK

#    include "NetworkConnection.h"

#    include "../core/String.hpp"
#    include "../localisation/Localisation.h"
#    include "../platform/platform.h"
#    include "Socket.h"
#    include "network.h"

constexpr size_t NETWORK_DISCONNECT_REASON_BUFFER_SIZE = 256;
constexpr size_t NetworkBufferSize = 1024 * 64; // 64 KiB, maximum packet size.

NetworkConnection::NetworkConnection()
{
    ResetLastPacketTime();
}

NetworkConnection::~NetworkConnection()
{
    delete[] _lastDisconnectReason;
}

NetworkReadPacket NetworkConnection::ReadPacket()
{
    size_t bytesRead = 0;

    // Read packet header.
    auto& header = InboundPacket.Header;
    if (InboundPacket.BytesTransferred < sizeof(InboundPacket.Header))
    {
        const size_t missingLength = sizeof(header) - InboundPacket.BytesTransferred;

        uint8_t* buffer = reinterpret_cast<uint8_t*>(&InboundPacket.Header);

        NetworkReadPacket status = Socket->ReceiveData(buffer, missingLength, &bytesRead);
        if (status != NetworkReadPacket::Success)
        {
            return status;
        }

        InboundPacket.BytesTransferred += bytesRead;
        if (InboundPacket.BytesTransferred < sizeof(InboundPacket.Header))
        {
            // If still not enough data for header, keep waiting.
            return NetworkReadPacket::MoreData;
        }

        // Normalise values.
        header.Size = Convert::NetworkToHost(header.Size);
        header.Id = ByteSwapBE(header.Id);

        // NOTE: For compatibility reasons for the master server we need to remove sizeof(Header.Id) from the size.
        // Previously the Id field was not part of the header rather part of the body.
        header.Size -= std::min<uint16_t>(header.Size, sizeof(header.Id));

        // Fall-through: Read rest of packet.
    }

    // Read packet body.
    {
        // NOTE: BytesTransfered includes the header length, this will not underflow.
        const size_t missingLength = header.Size - (InboundPacket.BytesTransferred - sizeof(header));

        uint8_t buffer[NetworkBufferSize];

        if (missingLength > 0)
        {
            NetworkReadPacket status = Socket->ReceiveData(buffer, std::min(missingLength, NetworkBufferSize), &bytesRead);
            if (status != NetworkReadPacket::Success)
            {
                return status;
            }

            InboundPacket.BytesTransferred += bytesRead;
            InboundPacket.Write(buffer, bytesRead);
        }

        if (InboundPacket.Data.size() == header.Size)
        {
            // Received complete packet.
            _lastPacketTime = platform_get_ticks();

            RecordPacketStats(InboundPacket, false);

            return NetworkReadPacket::Success;
        }
    }

    return NetworkReadPacket::MoreData;
}

bool NetworkConnection::SendPacket(NetworkPacket& packet)
{
    auto header = packet.Header;

    std::vector<uint8_t> buffer;
    buffer.reserve(sizeof(header) + header.Size);

    // NOTE: For compatibility reasons for the master server we need to add sizeof(Header.Id) to the size.
    // Previously the Id field was not part of the header rather part of the body.
    header.Size += sizeof(header.Id);
    header.Size = Convert::HostToNetwork(header.Size);
    header.Id = ByteSwapBE(header.Id);

    buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(&header), reinterpret_cast<uint8_t*>(&header) + sizeof(header));
    buffer.insert(buffer.end(), packet.Data.begin(), packet.Data.end());

    size_t bufferSize = buffer.size() - packet.BytesTransferred;
    size_t sent = Socket->SendData(buffer.data() + packet.BytesTransferred, bufferSize);
    if (sent > 0)
    {
        packet.BytesTransferred += sent;
    }

    bool sendComplete = packet.BytesTransferred == buffer.size();
    if (sendComplete)
    {
        RecordPacketStats(packet, true);
    }
    return sendComplete;
}

void NetworkConnection::QueuePacket(NetworkPacket&& packet, bool front)
{
    if (AuthStatus == NetworkAuth::Ok || !packet.CommandRequiresAuth())
    {
        packet.Header.Size = static_cast<uint16_t>(packet.Data.size());
        if (front)
        {
            // If the first packet was already partially sent add new packet to second position
            if (!_outboundPackets.empty() && _outboundPackets.front().BytesTransferred > 0)
            {
                auto it = _outboundPackets.begin();
                it++; // Second position
                _outboundPackets.insert(it, std::move(packet));
            }
            else
            {
                _outboundPackets.push_front(std::move(packet));
            }
        }
        else
        {
            _outboundPackets.push_back(std::move(packet));
        }
    }
}

void NetworkConnection::Disconnect()
{
    ShouldDisconnect = true;
}

bool NetworkConnection::IsValid() const
{
    return !ShouldDisconnect && Socket->GetStatus() == SocketStatus::Connected;
}

void NetworkConnection::SendQueuedPackets()
{
    while (!_outboundPackets.empty() && SendPacket(_outboundPackets.front()))
    {
        _outboundPackets.pop_front();
    }
}

void NetworkConnection::ResetLastPacketTime()
{
    _lastPacketTime = platform_get_ticks();
}

bool NetworkConnection::ReceivedPacketRecently()
{
#    ifndef DEBUG
    if (platform_get_ticks() > _lastPacketTime + 7000)
    {
        return false;
    }
#    endif
    return true;
}

const utf8* NetworkConnection::GetLastDisconnectReason() const
{
    return this->_lastDisconnectReason;
}

void NetworkConnection::SetLastDisconnectReason(const utf8* src)
{
    if (src == nullptr)
    {
        delete[] _lastDisconnectReason;
        _lastDisconnectReason = nullptr;
        return;
    }

    if (_lastDisconnectReason == nullptr)
    {
        _lastDisconnectReason = new utf8[NETWORK_DISCONNECT_REASON_BUFFER_SIZE];
    }
    String::Set(_lastDisconnectReason, NETWORK_DISCONNECT_REASON_BUFFER_SIZE, src);
}

void NetworkConnection::SetLastDisconnectReason(const rct_string_id string_id, void* args)
{
    char buffer[NETWORK_DISCONNECT_REASON_BUFFER_SIZE];
    format_string(buffer, NETWORK_DISCONNECT_REASON_BUFFER_SIZE, string_id, args);
    SetLastDisconnectReason(buffer);
}

void NetworkConnection::RecordPacketStats(const NetworkPacket& packet, bool sending)
{
    uint32_t packetSize = static_cast<uint32_t>(packet.BytesTransferred);
    NetworkStatisticsGroup trafficGroup;

    switch (packet.GetCommand())
    {
        case NetworkCommand::GameAction:
            trafficGroup = NetworkStatisticsGroup::Commands;
            break;
        case NetworkCommand::Map:
            trafficGroup = NetworkStatisticsGroup::MapData;
            break;
        default:
            trafficGroup = NetworkStatisticsGroup::Base;
            break;
    }

    if (sending)
    {
        Stats.bytesSent[EnumValue(trafficGroup)] += packetSize;
        Stats.bytesSent[EnumValue(NetworkStatisticsGroup::Total)] += packetSize;
    }
    else
    {
        Stats.bytesReceived[EnumValue(trafficGroup)] += packetSize;
        Stats.bytesReceived[EnumValue(NetworkStatisticsGroup::Total)] += packetSize;
    }
}

#endif
