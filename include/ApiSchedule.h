#pragma once
#include <stdint.h>

// Unsigned elapsed-time comparisons remain valid across millis() rollover.
class ApiSchedule
{
public:
    bool blocked(uint32_t now) const
    {
        return waitMs_ != 0 && uint32_t(now - waitStarted_) < waitMs_;
    }

    bool pollDue(uint32_t now) const
    {
        return !blocked(now) && (!polled_ || uint32_t(now - lastPoll_) >= pollDelay_);
    }

    void afterPoll(uint32_t now)
    {
        polled_ = true;
        lastPoll_ = now;
        pollDelay_ = 7500;
    }

    void afterCommand(uint32_t now)
    {
        afterPoll(now);
        waitStarted_ = now;
        waitMs_ = 1000; // Space out queued commands; polling still waits 7.5 seconds.
    }

    void rateLimited(uint32_t now)
    {
        waitStarted_ = now;
        waitMs_ = nextBackoff_;
        nextBackoff_ = nextBackoff_ >= 450000 ? 900000 : nextBackoff_ * 2;
    }

    void failed(uint32_t now)
    {
        waitStarted_ = now;
        waitMs_ = 15000;
    }

    void succeeded()
    {
        waitMs_ = 0;
        nextBackoff_ = 60000;
    }

    uint32_t cooldownMs() const { return waitMs_; }

private:
    bool polled_ = false;
    uint32_t lastPoll_ = 0;
    uint32_t pollDelay_ = 7500;
    uint32_t waitStarted_ = 0;
    uint32_t waitMs_ = 0;
    uint32_t nextBackoff_ = 60000;
};
