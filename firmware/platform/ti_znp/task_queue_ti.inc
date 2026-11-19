/* SPDX-License-Identifier: MIT
 * Included after TI's private TaskEntry/table definitions. A 16-bit aligned
 * SRAM offset fits the existing padding without changing any legacy field's
 * offset or the table stride. No per-message fields or wire formats change.
 */
#ifndef PORT_TASK_TAIL_BASE
#define PORT_TASK_TAIL_BASE ((uintptr_t)0x20000000u)
#endif
static uint16_t port_task_tail_encode(void *message) {
    if (!message) return 0;
    uintptr_t address = (uintptr_t)message, base = PORT_TASK_TAIL_BASE;
    if (address < base || address - base >= 0x14000u || (address - base) % 4u) return 0;
    return (uint16_t)(((address - base) >> 2) + 1u);
}
static void *port_task_tail_decode(uint16_t offset) {
    return offset ? (void *)(PORT_TASK_TAIL_BASE + ((uintptr_t)(offset - 1u) << 2)) : NULL;
}
static TaskEntry *port_task_queue(OsalPort_MsgQ *queue) {
    uintptr_t base = (uintptr_t)&taskTbl[0].qHandle, address = (uintptr_t)queue;
    if (address < base || (address - base) % sizeof(TaskEntry)) return NULL;
    uintptr_t index = (address - base) / sizeof(TaskEntry);
    return index < taskCnt ? &taskTbl[index] : NULL;
}
