/*
 * Regression: several reports accumulated into one message. dataBuffer, reportItems
 * and reportPointers all reallocate as they grow, so any pointer captured when the
 * report was added dangles. Only ever safe while a message held a single report.
 *
 * Run under ASan: the failure mode is a use-after-realloc, not a wrong assert.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "hid/report.h"

#define REPORT_LEN 48
#define N_REPORTS 32 /* far past dataBuffer's initial 256-byte capacity */

int main() {
    IHS_HIDReportHolder holder;
    IHS_HIDReportHolderInit(&holder, 1);
    IHS_HIDReportHolderSetReportLength(&holder, REPORT_LEN);

    uint8_t previous[REPORT_LEN], current[REPORT_LEN];
    memset(previous, 0, sizeof(previous));

    for (int i = 0; i < N_REPORTS; i++) {
        /* Worst case on purpose: EVERY byte changes, so the delta is a full bitmask
         * plus a full copy of the report — larger than the report itself. */
        memset(current, (uint8_t) (i + 1), sizeof(current));
        IHS_HIDReportHolderAddDelta(&holder, previous, current, REPORT_LEN);
        memcpy(previous, current, REPORT_LEN);
    }

    IHS_HIDDeviceReportMessage *msg = IHS_HIDReportHolderGetMessage(&holder);
    assert(msg != NULL);
    assert(msg->n_reports == N_REPORTS);

    /* Every item must be reachable and carry a non-empty, in-bounds payload. */
    for (size_t i = 0; i < msg->n_reports; i++) {
        CHIDDeviceInputReport *item = msg->reports[i];
        assert(item != NULL);
        assert(item->has_delta_report);
        assert(item->delta_report.data != NULL);
        /* mask + every byte, i.e. bigger than the report — the reserved size must cover it */
        assert(item->delta_report.len == ((REPORT_LEN + 7) / 8) + REPORT_LEN);
        /* Touch it: ASan traps here if the pointer outlived a realloc. */
        volatile uint8_t first = item->delta_report.data[0];
        (void) first;
    }

    /* The items must be distinct, i.e. pointers were rebound per item, not all
     * left pointing at the last appended chunk. */
    assert(msg->reports[0]->delta_report.data != msg->reports[N_REPORTS - 1]->delta_report.data);

    IHS_HIDReportHolderResetMessage(&holder);
    assert(IHS_HIDReportHolderGetMessage(&holder) == NULL);

    IHS_HIDReportHolderDeinit(&holder);
    puts("hid report batch OK");
    return 0;
}
