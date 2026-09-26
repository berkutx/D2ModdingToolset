#ifndef TESTS_SIMTURNS_LOBBY_STARTUP_BIT_STREAM_H
#define TESTS_SIMTURNS_LOBBY_STARTUP_BIT_STREAM_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SLNet {

using MessageID = std::uint8_t;

struct RakNetGUID
{
    std::uint64_t value{};
};

class BitStream
{
public:
    void Write(MessageID value) { m_bytes.push_back(value); }

    void WriteAlignedBytes(const unsigned char* bytes, unsigned size)
    {
        if (bytes && size) m_bytes.insert(m_bytes.end(), bytes, bytes + size);
    }

    const std::vector<std::uint8_t>& bytes() const { return m_bytes; }

private:
    std::vector<std::uint8_t> m_bytes;
};

} // namespace SLNet

enum PacketPriority
{
    HIGH_PRIORITY,
};

#endif
