#ifndef TESTS_SIMTURNS_LOBBY_STARTUP_SPDLOG_H
#define TESTS_SIMTURNS_LOBBY_STARTUP_SPDLOG_H

namespace spdlog {

namespace level {
enum level_enum
{
    trace,
    debug,
    info,
    warn,
    err,
    critical,
    off,
};
} // namespace level

class logger
{
public:
    void flush() noexcept {}
};

inline logger* default_logger()
{
    static logger instance;
    return &instance;
}

template<class... Args>
void log(level::level_enum, const char*, const Args&...) noexcept
{}

template<class... Args>
void critical(const char*, const Args&...) noexcept
{}

} // namespace spdlog

#endif
