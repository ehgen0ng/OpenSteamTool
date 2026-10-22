#pragma once

#include "Types.h"
#include "Enums.h"

#include <cstddef>
#include <cstring>
#include <limits>

enum class NetPacketLayout { Legacy = 1, Extended = 2 };

struct CNetPacket {
    // Legacy native layout; only Data()/DataSize() may be used by hooks.
    // The meaning of the extra field in newer clients is intentionally opaque.
    HCONNECTION m_hConnection;
    uint8* m_pubData;
    uint32 m_cubData;
    int32 m_cRef;
    uint8* m_pubNetworkBuffer;
    CNetPacket* m_pNext;

    static constexpr std::size_t DataOffset(NetPacketLayout layout) {
        return layout == NetPacketLayout::Extended ? 16 : offsetof(CNetPacket, m_pubData);
    }

    static constexpr std::size_t SizeOffset(NetPacketLayout layout) {
        return DataOffset(layout) + sizeof(uint8*);
    }

    uint8*& Data() {
        return *reinterpret_cast<uint8**>(reinterpret_cast<byte*>(this) + DataOffset(s_layout));
    }

    uint32& DataSize() {
        return *reinterpret_cast<uint32*>(reinterpret_cast<byte*>(this) + SizeOffset(s_layout));
    }

    // Both loaders select once from hash-bound metadata, before installing hooks.
    static void SetPayloadLayout(NetPacketLayout layout) { s_layout = layout; }
    static NetPacketLayout GetPayloadLayout() { return s_layout; }

private:
    inline static NetPacketLayout s_layout = NetPacketLayout::Legacy;
};

static_assert(sizeof(void*) != 8 || offsetof(CNetPacket, m_pubData) == 8);
static_assert(sizeof(void*) != 8 || offsetof(CNetPacket, m_cubData) == 16);

struct MsgHdr {
    uint32 eMsg; // Raw wire value includes kMsgHdrProtoFlag, not just an EMsg.
    uint32 headerLength;
};
static_assert(sizeof(MsgHdr) == 8);
static_assert(sizeof(EMsg) == 4);
constexpr uint32 kMsgHdrProtoFlag = 0x80000000;

namespace NetPacket {

// Sanity filter only: this does not establish that arbitrary memory is mapped.
// In particular, never probe two native layouts and guess from pointer values.
inline bool IsPlausibleRange(const void* data, std::size_t size) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    constexpr auto max = std::numeric_limits<std::uintptr_t>::max();
    return address >= 0x10000 && address != max &&
           address != std::numeric_limits<uint32>::max() &&
           size <= max - address;
}

inline bool HasPlausiblePayload(CNetPacket* packet) {
    const auto fieldsSize = CNetPacket::SizeOffset(CNetPacket::GetPayloadLayout()) + sizeof(uint32);
    if (!IsPlausibleRange(packet, fieldsSize) ||
        reinterpret_cast<std::uintptr_t>(packet) % alignof(CNetPacket) != 0) return false;
    return packet->DataSize() >= sizeof(MsgHdr) &&
           IsPlausibleRange(packet->Data(), packet->DataSize());
}

inline bool UnpackRaw(const uint8* data, uint32 size,
                      EMsg& eMsg, const uint8*& pHdr, uint32& cbHdr,
                      const uint8*& pBody, uint32& cbBody) {
    eMsg = static_cast<EMsg>(0);
    pHdr = pBody = nullptr;
    cbHdr = cbBody = 0;
    if (size < sizeof(MsgHdr) || !IsPlausibleRange(data, size)) return false;

    // Network bytes need not be naturally aligned.
    MsgHdr header;
    std::memcpy(&header, data, sizeof(header));
    if (!(header.eMsg & kMsgHdrProtoFlag) ||
        header.headerLength > size - sizeof(MsgHdr)) return false;

    eMsg = static_cast<EMsg>(header.eMsg & ~kMsgHdrProtoFlag);
    cbHdr = header.headerLength;
    pHdr = data + sizeof(MsgHdr);
    pBody = pHdr + cbHdr;
    cbBody = size - sizeof(MsgHdr) - cbHdr;
    return true;
}

} // namespace NetPacket
