/*
 * Regression: a window holding only orphaned fragments (the frame-head packet was
 * lost) can't be advanced by Discard(), which only stops at frame heads. Without
 * ReleaseAll() the window stays full forever and every later packet overflows.
 */

#include <assert.h>
#include <string.h>

#include "ihs_buffer.h"
#include "session/window.h"
#include "session/packet.h"

#define CAPACITY 8

static void AddFrag(IHS_SessionPacketsWindow *window, uint16_t packetId, uint32_t timestamp) {
    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = IHS_SessionPacketTypeUnreliableFrag;
    packet.header.channelId = IHS_SessionChannelIdDataStart;
    packet.header.packetId = packetId;
    packet.header.fragmentId = (int16_t) (packetId + 1);
    packet.header.sendTimestamp = timestamp;
    IHS_BufferInit(&packet.body, 16, 16);
    IHS_BufferFillMem(&packet.body, 0, 0, 16);
    assert(IHS_SessionPacketsWindowAdd(window, &packet));
    IHS_SessionPacketClear(&packet, true);
}

int main() {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(CAPACITY);

    for (uint16_t i = 0; i < CAPACITY; i++) {
        AddFrag(window, i, 1000 + i);
    }
    assert(IHS_SessionPacketsWindowAvailable(window) == 0);

    /* No frame head anywhere, so Discard can't advance past anything. */
    assert(IHS_SessionPacketsWindowDiscard(window, 0) == 0);
    assert(IHS_SessionPacketsWindowAvailable(window) == 0);

    IHS_SessionPacketsWindowReleaseAll(window);
    assert(IHS_SessionPacketsWindowAvailable(window) == CAPACITY);
    assert(IHS_SessionPacketsWindowSize(window) == 0);

    /* And the window is usable again afterwards. */
    AddFrag(window, 0, 2000);
    assert(IHS_SessionPacketsWindowSize(window) == 1);

    IHS_SessionPacketsWindowDestroy(window);
    return 0;
}
