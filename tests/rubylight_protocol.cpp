// Rubylight control messages: Nightjar's phase lock slack window (its own test
// cases from protocol/core/src/phase_lock/tests.rs), clock sync and the reports
// the host sends.
#include "streaming/rubylightprotocol.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

using namespace Rubylight;

static constexpr int64_t PERIOD = 8333333; // 120.00 Hz

static void earlyEdge()
{
    SlackWindow window;
    for (int64_t i = 0; i < 100; ++i) window.push(3000000 + i * 10000, PERIOD);
    const auto report = window.report(10);
    assert(report && report->frames == 100 && report->periodNs == PERIOD);
    assert(std::llabs(report->leadNs - 3100000) <= 20000);
    assert(std::llabs(int64_t(report->spreadNs) - 800000) <= 20000);
    assert(window.size() == 0 && !window.report(1));
}

static void straddlingCluster()
{
    SlackWindow window;
    for (int64_t i = 0; i < 50; ++i) {
        window.push(200000 + i, PERIOD);
        window.push(PERIOD - 200000 - i, PERIOD);
    }
    const auto report = window.report(10);
    assert(report && report->leadNs < 0 && report->leadNs > -300000 && report->spreadNs < 500000);
}

static void refreshChange()
{
    SlackWindow window;
    window.push(1000000, PERIOD);
    window.push(1000000, 16666667);
    assert(window.size() == 1 && window.report(1)->periodNs == 16666667);
}

static void extraRoom()
{
    const auto fill = [](SlackWindow& window, int64_t period) {
        for (int64_t i = 0; i < 30; ++i) window.push(2000000 + (i % 10) * 400000, period);
    };
    SlackWindow window;
    fill(window, 16666667);
    const auto plain = *window.report(10);
    // 30 frames in half a second at 60 Hz: one per refresh.
    fill(window, 16666667);
    const auto paced = *window.reportOver(10, 500000000);
    assert(paced.leadNs == plain.leadNs - int32_t(plain.spreadNs) / 4);
    assert(paced.frames == plain.frames && paced.spreadNs == plain.spreadNs && paced.periodNs == plain.periodNs);
    // At 120 Hz the same frames come every second refresh: a miss costs no frame.
    fill(window, 8333333);
    const auto half = *window.reportOver(10, 500000000);
    fill(window, 8333333);
    const auto halfPlain = *window.report(10);
    assert(half.leadNs == halfPlain.leadNs && half.spreadNs == halfPlain.spreadNs);
    fill(window, 16666667);
    assert(window.reportOver(10, 0)->leadNs == plain.leadNs);

    // A 7 ms spread on 60 Hz leaves no room.
    SlackWindow wide;
    for (int64_t i = 0; i < 30; ++i) wide.push(1000000 + (i % 10) * 900000, 16666667);
    SlackWindow copy = wide;
    const auto widePlain = *copy.report(10);
    const auto widePaced = *wide.reportOver(10, 500000000);
    assert(widePlain.spreadNs >= 7000000 && widePaced.leadNs == widePlain.leadNs);
}

static void reportBytes()
{
    PhaseReport report {60, uint32_t(PERIOD), -1200000, 900000};
    const auto bytes = report.encode();
    assert(bytes[0] == 1 && bytes[1] == 0 && u16At(&bytes[2]) == 60 && u32At(&bytes[4]) == PERIOD);
    assert(int32_t(u32At(&bytes[8])) == -1200000 && u32At(&bytes[12]) == 900000);
}

static void clockSync()
{
    const auto probe = clockSyncRequest(7, 1000000);
    assert(probe[0] == 1 && u32At(&probe[4]) == 7 && u64At(&probe[8]) == 1000000);
    // Host clock 5 s ahead of the client's, 400 µs each way, 100 µs inside the host.
    uint8_t reply[32] {};
    reply[0] = 1;
    putU32(reply + 4, 7);
    putU64(reply + 8, 1000000);
    const uint64_t hostReceive = 1000000 + 400 + 10000000 - 5000000;
    putU64(reply + 16, hostReceive);
    putU64(reply + 24, hostReceive + 100);
    const auto decoded = ClockSyncReply::decode(reply, 32);
    assert(decoded && decoded->sequence == 7);
    assert(!ClockSyncReply::decode(reply, 31));
    ClockOffset offset;
    const auto rtt = offset.add(*decoded, 1000000 + 900);
    assert(rtt && *rtt == 800);
    assert(*offset.offsetUs() == -5000000 && *offset.rttUs() == 800);
    // A slower exchange does not replace the best one.
    uint8_t slow[32];
    std::memcpy(slow, reply, 32);
    putU64(slow + 8, 2000000);
    putU64(slow + 16, 2000000 + 3000 + 10000000 - 5000000);
    putU64(slow + 24, 2000000 + 3000 + 10000000 - 5000000 + 100);
    assert(*offset.add(*ClockSyncReply::decode(slow, 32), 2000000 + 3500) == 3400);
    assert(*offset.rttUs() == 800);
    // A reply that came back before it was sent is impossible.
    assert(!offset.add(*decoded, 999999));

    // A frame the host presented 20 ms before now, on a host clock 5 s behind.
    const int64_t clientNow = 123456789;
    const uint64_t presented = uint64_t(clientNow - 5000000 - 20000);
    const uint32_t rtp = uint32_t(presented * 90 / 1000);
    assert(std::llabs(frameAgeUs(rtp, clientNow, 5000000) - 20000) <= 12);
}

static void hostReports()
{
    uint8_t timing[4 + 2 * 20] {1, 2, 0, 0};
    const uint32_t a[5] {10, 9000, 3000, 2000, 1000}, b[5] {11, 8000, 2000, 2000, 1500};
    for (int i = 0; i < 5; ++i) { putU32(timing + 4 + i * 4, a[i]); putU32(timing + 24 + i * 4, b[i]); }
    auto records = decodeHostTiming(timing, sizeof(timing));
    assert(records.size() == 2 && records[1].frame == 11 && records[1].sendUs == 1500);
    assert(decodeHostTiming(timing, sizeof(timing) - 1).empty());
    putU32(timing + 24 + 8, 9000); // stages over the whole
    assert(decodeHostTiming(timing, sizeof(timing)).empty());

    uint8_t rates[28] {1};
    putU32(rates + 4, 1000);
    putU32(rates + 8, 59750);
    putU32(rates + 24, PipelineRates::Unknown);
    const auto r = PipelineRates::decode(rates, 28);
    assert(r && r->windowMs == 1000 && r->sourceMhz == 59750 && r->repeatMhz == PipelineRates::Unknown);
    assert(!PipelineRates::decode(rates, 27));

    uint8_t notice[12] {1, 1, 1, 0};
    putU32(notice + 4, 300000);
    putU32(notice + 8, 255000);
    const auto n = LinkNotice::decode(notice, 12);
    assert(n && n->state == LinkNotice::Limited && n->currentKbps == 255000);
    notice[2] = 0; // limited without a reason
    assert(!LinkNotice::decode(notice, 12));
    notice[1] = 0;
    assert(LinkNotice::decode(notice, 12)->state == LinkNotice::Full);

    const auto request = pipelineRatesRequest(true, 100);
    assert(request[1] == 1 && u16At(&request[2]) == 500);
}

int main()
{
    earlyEdge();
    straddlingCluster();
    refreshChange();
    extraRoom();
    reportBytes();
    clockSync();
    hostReports();
    std::puts("rubylight protocol tests passed");
    return 0;
}
