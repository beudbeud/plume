/*
 * Regression: joining a session already in progress. The first reliable packet we
 * see is a fragment from the middle of a message the host was already sending;
 * its head is older than the window, so Add() will never accept it. Poll() must
 * skip the orphan instead of waiting for a head that can never arrive — otherwise
 * the window wedges full and every later packet reports "Frames window overflow".
 */

#include <assert.h>
#include <string.h>

#include "ihs_buffer.h"
#include "session/window.h"
#include "session/packet.h"

#define CAPACITY 8

static void Add(IHS_SessionPacketsWindow *window, IHS_SessionPacketType type,
                uint16_t packetId, int16_t fragmentId) {
    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = type;
    packet.header.channelId = IHS_SessionChannelIdControl;
    packet.header.packetId = packetId;
    packet.header.fragmentId = fragmentId;
    packet.header.sendTimestamp = 1000 + packetId;
    IHS_BufferInit(&packet.body, 16, 16);
    IHS_BufferFillMem(&packet.body, 0, 0, 16);
    assert(IHS_SessionPacketsWindowAdd(window, &packet));
    IHS_SessionPacketClear(&packet, true);
}

int main() {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(CAPACITY);
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 1024, 1024 * 1024);

    /* We joined mid-message: fragments 2 and 3 of a message whose head we missed. */
    Add(window, IHS_SessionPacketTypeReliableFrag, 0, 2);
    Add(window, IHS_SessionPacketTypeReliableFrag, 1, 3);

    /* Then a complete, self-contained message starts. */
    Add(window, IHS_SessionPacketTypeReliable, 2, 0);

    /* Poll must step over the two orphans and deliver the good message. */
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    assert(frame.header.packetId == 2);
    IHS_SessionPacketsWindowReleaseFrame(&frame);

    /* Window drained: nothing left, and it accepts new traffic. */
    assert(IHS_SessionPacketsWindowSize(window) == 0);
    Add(window, IHS_SessionPacketTypeReliable, 3, 0);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    assert(frame.header.packetId == 3);
    IHS_SessionPacketsWindowReleaseFrame(&frame);

    /* Orphans alone must not report a frame, and must not wedge the window. */
    Add(window, IHS_SessionPacketTypeReliableFrag, 4, 1);
    assert(!IHS_SessionPacketsWindowPoll(window, &frame));
    assert(IHS_SessionPacketsWindowSize(window) == 0);

    IHS_BufferClear(&frame.body, true);
    IHS_SessionPacketsWindowDestroy(window);
    return 0;
}
