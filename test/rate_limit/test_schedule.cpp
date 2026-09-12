#include "ApiSchedule.h"
#include <assert.h>
#include <stdint.h>

int main()
{
    ApiSchedule schedule;
    assert(schedule.pollDue(0));
    schedule.afterPoll(100);
    assert(!schedule.pollDue(7599));
    assert(schedule.pollDue(7600));

    schedule.rateLimited(5100);
    assert(schedule.cooldownMs() == 60000);
    assert(!schedule.pollDue(65099));
    assert(schedule.pollDue(65100));
    schedule.rateLimited(65100);
    assert(schedule.cooldownMs() == 120000);
    for (int i = 0; i < 10; ++i)
        schedule.rateLimited(0);
    assert(schedule.cooldownMs() == 900000);
    schedule.succeeded();
    schedule.rateLimited(0);
    assert(schedule.cooldownMs() == 60000);

    schedule.succeeded();
    schedule.afterCommand(1000);
    assert(schedule.blocked(1999));
    assert(!schedule.pollDue(1999));
    assert(!schedule.blocked(2000));
    assert(!schedule.pollDue(2000));
    assert(!schedule.pollDue(8499));
    assert(schedule.pollDue(8500));
    schedule.failed(2000);
    assert(schedule.blocked(16999));
    assert(!schedule.blocked(17000));

    // Neither normal polling nor a 429 cooldown ends early on rollover.
    const uint32_t nearWrap = UINT32_MAX - 100;
    schedule.succeeded();
    schedule.afterPoll(nearWrap);
    assert(!schedule.pollDue(nearWrap + uint32_t(7499)));
    assert(schedule.pollDue(nearWrap + uint32_t(7500)));
    schedule.rateLimited(nearWrap);
    assert(schedule.blocked(nearWrap + uint32_t(59999)));
    assert(!schedule.blocked(nearWrap + uint32_t(60000)));
}
