/*
 * Regression: a packet permanently lost at the head of the window makes Poll()
 * fail forever. The age-based Discard() is the only way past it, so it must be
 * able to step over the gap once newer packets are >DISCARD_DIFF ahead.
 */

#include <assert.h>
#include <string.h>

#include "ihs_buffer.h"
#include "session/window.h"
#include "session/packet.h"

#define CAPACITY 16
#define DISCARD_DIFF IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(200)

/* A frame head (non-fragmented) packet: Poll() can yield it on its own. */
static void AddHead(IHS_SessionPacketsWindow *window, uint16_t packetId, uint32_t timestamp) {
    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = IHS_SessionPacketTypeUnreliable;
    packet.header.channelId = IHS_SessionChannelIdDataStart;
    packet.header.packetId = packetId;
    packet.header.fragmentId = 0;
    packet.header.sendTimestamp = timestamp;
    IHS_BufferInit(&packet.body, 16, 16);
    IHS_BufferFillMem(&packet.body, 0, 0, 16);
    assert(IHS_SessionPacketsWindowAdd(window, &packet));
    IHS_SessionPacketClear(&packet, true);
}

int main() {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(CAPACITY);
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 1024, 1024 * 1024); /* as DataThreadWorker does */

    /* Packet 0 arrives and is consumed; packet 1 is lost forever. */
    AddHead(window, 0, 1000);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    IHS_SessionPacketsWindowReleaseFrame(&frame);

    AddHead(window, 2, 1000 + DISCARD_DIFF / 2);
    assert(!IHS_SessionPacketsWindowPoll(window, &frame)); /* wedged on the gap at 1 */

    /* Still within the 200ms window: too early to give up on packet 1. */
    assert(IHS_SessionPacketsWindowDiscard(window, DISCARD_DIFF) == 0);
    assert(!IHS_SessionPacketsWindowPoll(window, &frame)); /* still wedged */

    /* A packet arrives more than DISCARD_DIFF after the stalled slot. */
    AddHead(window, 3, 1000 + DISCARD_DIFF * 2);
    assert(IHS_SessionPacketsWindowDiscard(window, DISCARD_DIFF) > 0);

    /* The gap is behind us: the window yields frames again. */
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    IHS_SessionPacketsWindowReleaseFrame(&frame);

    IHS_BufferClear(&frame.body, true);
    IHS_SessionPacketsWindowDestroy(window);
    return 0;
}
