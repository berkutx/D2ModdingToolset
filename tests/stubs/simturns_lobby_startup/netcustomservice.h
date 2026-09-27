#ifndef TESTS_SIMTURNS_LOBBY_STARTUP_NETCUSTOMSERVICE_H
#define TESTS_SIMTURNS_LOBBY_STARTUP_NETCUSTOMSERVICE_H

#include "BitStream.h"
#include "netcustomsession.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hooks {

constexpr SLNet::MessageID ID_LOBBY_SIMULTANEOUS_TURNS = 152;

class CNetCustomService
{
public:
    explicit CNetCustomService(CNetCustomSession* session) : m_session(session)
    {
        s_instance = this;
    }

    ~CNetCustomService()
    {
        if (s_instance == this) s_instance = nullptr;
    }

    static CNetCustomService* get() { return s_instance; }
    CNetCustomSession* getSession() const { return m_session; }
    const SLNet::RakNetGUID getLobbyGuid() const { return {}; }
    bool roomRequiresSimultaneousTurns() const { return m_simultaneousTurns; }

    bool send(const SLNet::BitStream& stream, const SLNet::RakNetGUID&, PacketPriority) const
    {
        sent.push_back(stream.bytes());
        return sendAccepted;
    }

    void enqueueSystemNotice(const char* text)
    {
        notices.emplace_back(text ? text : "");
    }

    mutable std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::string> notices;
    bool sendAccepted{true};

private:
    inline static CNetCustomService* s_instance{};
    CNetCustomSession* m_session{};
    bool m_simultaneousTurns{true};
};

} // namespace hooks

#endif
