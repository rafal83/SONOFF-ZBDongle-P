/* Actual generated TI OSAL queue functions, including compressed tail state.
 * Model the MCU SRAM arena; NEXT access counts expose an accidental list walk.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *pthread_t;
typedef uintptr_t sem_t;
typedef void *OsalPort_MsgQ;
typedef struct { void *next; uint16_t len; uint8_t dest_id; } OsalPort_MsgHdr;
typedef struct { uint8_t event, status; } OsalPort_EventHdr;
typedef struct {
    uint8_t taskId; pthread_t taskHndl; OsalPort_MsgQ qHandle; sem_t *taskSem;
    bool conservePower; uint32_t *pEventFlag;
} OriginalTaskEntry;
static _Alignas(16) uint8_t arena[0x14000];
#define PORT_TASK_TAIL_BASE ((uintptr_t)arena)
#define OsalPort_TASK_NO_TASK 255
#define OsalPort_INVALID_TASK 3
#define OsalPort_INVALID_MSG_POINTER 5
#define OsalPort_SUCCESS 0
#define OsalPort_SYS_EVENT_MSG 0x8000
static unsigned critical, links, posts;
static uint32_t flags[15];
static void **next_link(void *p) { assert(p); links++; return &((OsalPort_MsgHdr *)p - 1)->next; }
#define OsalPort_MSG_NEXT(p) (*next_link(p))
#define OsalPort_MSG_ID(p) (((OsalPort_MsgHdr *)p - 1)->dest_id)
uint32_t OsalPort_enterCS(void) { return critical++; }
void OsalPort_leaveCS(uint32_t key) { assert(critical); critical = key; }
uint8_t OsalPort_setEvent(uint8_t id, uint32_t event) { assert(critical && id < 15); flags[id] |= event; posts++; return 0; }
void OsalPort_clearEvent(uint8_t id, uint32_t event) { assert(critical && id < 15); flags[id] &= ~event; }
void OsalPort_msgExtract(OsalPort_MsgQ *, void *, void *);
#include "osal_queue_under_test.inc"
_Static_assert(sizeof(OriginalTaskEntry) == sizeof(TaskEntry), "Table stride changed");
_Static_assert(offsetof(OriginalTaskEntry, qHandle) == offsetof(TaskEntry, qHandle), "Head offset changed");
_Static_assert(offsetof(OriginalTaskEntry, conservePower) == offsetof(TaskEntry, conservePower), "Power offset changed");
_Static_assert(offsetof(OriginalTaskEntry, pEventFlag) == offsetof(TaskEntry, pEventFlag), "Events offset changed");
static void *message(unsigned i, uint8_t event) {
    assert(i < 512);
    OsalPort_MsgHdr *header = (void *)(arena + i * 64);
    memset(header, 0, sizeof(*header) + 2); header->dest_id = 255; header->len = 2;
    OsalPort_EventHdr *p = (void *)(header + 1); p->event = event; return p;
}
static void drain(unsigned id) { while (OsalPort_msgReceive((uint8_t)id)) {} assert(!taskTbl[id].portTailOffset && !flags[id]); }
int main(void) {
    for (unsigned i = 0; i < 15; i++) assert(OsalPort_registerTask(NULL, NULL, &flags[i]) == i);
    assert(OsalPort_registerTask(NULL, NULL, NULL) == 255 && taskCnt == 15);
    assert(OsalPort_registerTask(NULL, NULL, NULL) == 255 && taskCnt == 15);
    assert(OsalPort_msgSend(15, message(0, 1)) == OsalPort_INVALID_TASK && !OsalPort_msgReceive(15));
    assert(OsalPort_msgSend(0, NULL) == OsalPort_INVALID_MSG_POINTER);
    for (unsigned size = 1; size <= 256; size *= 2) {
        for (unsigned i = 0; i < size; i++) assert(!OsalPort_msgSend(0, message(i, (uint8_t)i)));
        unsigned previous = links;
        void *last = message(size, 7);
        assert(!OsalPort_msgSend(0, last));
        assert(links - previous <= 2); /* Independent of queue length. */
        assert(port_task_tail_decode(taskTbl[0].portTailOffset) == last);
        for (unsigned i = 0; i < size; i++) assert(OsalPort_msgReceive(0) == arena + i * 64 + sizeof(OsalPort_MsgHdr));
        assert(OsalPort_msgReceive(0) == last && !OsalPort_msgReceive(0));
    }
    void *a = message(0, 1), *b = message(1, 2), *c = message(2, 3), *d = message(3, 4);
    OsalPort_msgPush(&taskTbl[1].qHandle, a);
    OsalPort_msgEnqueue(&taskTbl[1].qHandle, b);
    OsalPort_msgPush(&taskTbl[1].qHandle, c);
    assert(OsalPort_msgFindDequeue(1, 2) == b); /* Remove tail, retain a. */
    assert(port_task_tail_decode(taskTbl[1].portTailOffset) == a);
    OsalPort_msgEnqueue(&taskTbl[1].qHandle, d);
    assert(OsalPort_msgReceive(1) == c && OsalPort_msgReceive(1) == a && OsalPort_msgReceive(1) == d);
    OsalPort_msgEnqueue(&taskTbl[1].qHandle, a);
    assert(OsalPort_msgFindDequeue(1, 1) == a && !taskTbl[1].portTailOffset);
    OsalPort_msgPush(&taskTbl[1].qHandle, b);
    OsalPort_msgExtract(&taskTbl[1].qHandle, b, NULL);
    assert(!taskTbl[1].qHandle && !taskTbl[1].portTailOffset);
    OsalPort_MsgQ foreign = NULL;
    assert(!OsalPort_msgEnqueueMax(&foreign, a, 0));
    assert(OsalPort_msgEnqueueMax(&foreign, a, 1));
    assert(!OsalPort_msgEnqueueMax(&foreign, b, 1)); /* Old code admitted max+1. */
    assert(OsalPort_msgEnqueueMax(&foreign, b, 2));
    assert(!OsalPort_msgEnqueueMax(&foreign, c, 2));
    OsalPort_msgExtract(&foreign, b, a);
    assert(OsalPort_msgDequeue(&foreign) == a && !OsalPort_msgDequeue(&foreign));
    /* A foreign buffer cannot be encoded; one fallback traversal repairs the
     * tail when the next valid SRAM message arrives. No pointer truncation. */
    struct { OsalPort_MsgHdr hdr; OsalPort_EventHdr payload; } external = {0};
    assert(!port_task_tail_encode(&external.payload));
    OsalPort_msgEnqueue(&taskTbl[2].qHandle, &external.payload);
    assert(!taskTbl[2].portTailOffset);
    OsalPort_msgEnqueue(&taskTbl[2].qHandle, a);
    assert(port_task_tail_decode(taskTbl[2].portTailOffset) == a);
    assert(OsalPort_msgReceive(2) == (void *)&external.payload);
    assert(OsalPort_msgReceive(2) == a);
    for (unsigned round = 0; round < 10000; round++) {
        unsigned id = round % 15;
        assert(!OsalPort_msgSend((uint8_t)id, message(0, 1)));
        assert(!OsalPort_msgSend((uint8_t)id, message(1, 2)));
        assert((void *)OsalPort_msgFindDequeue((uint8_t)id, 2) == arena + 64 + sizeof(OsalPort_MsgHdr));
        assert(!OsalPort_msgSend((uint8_t)id, message(2, 3)));
        drain(id);
    }
    assert(!critical && posts && taskCnt == 15);
    printf("PASS: actual OSAL task queues, constant append at Q=0..256, head/tail/middle removals, 10000 reuse cycles, foreign queues/pointers, exact max bound, atomic events, registration saturation; TaskEntry=%zu unchanged bytes\n", sizeof(TaskEntry));
}
