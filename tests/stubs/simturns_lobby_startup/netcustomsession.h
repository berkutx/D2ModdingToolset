#ifndef TESTS_SIMTURNS_LOBBY_STARTUP_NETCUSTOMSESSION_H
#define TESTS_SIMTURNS_LOBBY_STARTUP_NETCUSTOMSESSION_H

namespace hooks {

class CNetCustomSession
{
public:
    explicit CNetCustomSession(bool host) : m_host(host) {}
    bool isHost() const { return m_host; }
    void setHost(bool host) { m_host = host; }

private:
    bool m_host{};
};

} // namespace hooks

#endif
