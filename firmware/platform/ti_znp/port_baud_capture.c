/* SPDX-License-Identifier: MIT
 * CC2652P passive UART0 RX timing. IOC edge detection is independent of the
 * UART PORT_ID mux (TI SWCU185G 13.2/13.4). AON_PROG2 is private to this board
 * while armed; GPIO CPU interrupts are never disabled or consumed.
 */
#include "port_baud_capture.h"
#include "osal_port.h"
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26XX.h>
#include <ti/drivers/dma/UDMACC26XX.h>
#include <ti/drivers/dpl/ClockP.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_aon_event.h)
#include DeviceFamily_constructPath(inc/hw_event.h)
#include DeviceFamily_constructPath(inc/hw_gpio.h)
#include DeviceFamily_constructPath(inc/hw_ioc.h)
#include DeviceFamily_constructPath(inc/hw_gpt.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(driverlib/timer.h)
#include DeviceFamily_constructPath(driverlib/udma.h)
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)

#define RX_DIO 12u
#define RX_MASK (1u << RX_DIO)
#define CAP_CHANNEL 14u
#define CAP_MASK (1u << CAP_CHANNEL)
/* TI's control table is sparsely allocated. Reserve BOTH entries explicitly;
 * never assume its unused channel addresses are otherwise free RAM. */
ALLOCATE_CONTROL_TABLE_ENTRY(port_baud_dma_primary, CAP_CHANNEL);
ALLOCATE_CONTROL_TABLE_ENTRY(port_baud_dma_alternate, CAP_CHANNEL | UDMA_ALT_SELECT);
static volatile uint32_t captures[4];
static uint8_t active, saved_ioc, saved_aon, saved_dma_event;
static const uint32_t clear_rx = RX_MASK;
#define CAP_TIME(n) uDMATaskStructEntry(1, UDMA_SIZE_32, UDMA_SRC_INC_NONE, \
    (void *)(GPT0_BASE + GPT_O_TAR), UDMA_DST_INC_NONE, &captures[n], UDMA_ARB_1, UDMA_MODE_PER_SCATTER_GATHER)
#define CAP_CLEAR(mode) uDMATaskStructEntry(1, UDMA_SIZE_32, UDMA_SRC_INC_NONE, \
    &clear_rx, UDMA_DST_INC_NONE, (void *)(GPIO_BASE + GPIO_O_EVFLAGS31_0), UDMA_ARB_1, mode)
static const tDMAControlTable tasks[] = {
    CAP_TIME(0), CAP_CLEAR(UDMA_MODE_PER_SCATTER_GATHER),
    CAP_TIME(1), CAP_CLEAR(UDMA_MODE_PER_SCATTER_GATHER),
    CAP_TIME(2), CAP_CLEAR(UDMA_MODE_PER_SCATTER_GATHER),
    CAP_TIME(3), CAP_CLEAR(UDMA_MODE_BASIC)
};

uint32_t PortBaudCapture_clock(void) {
    ClockP_FreqHz hz;
    ClockP_getCpuFreq(&hz);
    return hz.hi ? 0u : hz.lo;
}
bool PortBaudCapture_start(void) {
    if (active) return true;
    uint32_t previous_event = HWREG(EVENT_BASE + EVENT_O_UDMACH14BSEL);
    /* GPT0, DMA14 and AON_PROG2 have no other users in the SONOFF build.
     * Refuse an unexpected allocation rather than disturb another peripheral. */
    if (Power_getDependencyCount(PowerCC26XX_PERIPH_GPT0) ||
        uDMAChannelIsEnabled(UDMA0_BASE, CAP_CHANNEL) ||
        /* Hardware reset selects AON_PROG0 (1), not NONE (0). An inactive,
         * exclusively allocated DMA14 may retain either of these idle routes. */
        (previous_event != EVENT_UDMACH14BSEL_EV_NONE && previous_event != EVENT_UDMACH14BSEL_EV_AON_PROG0) ||
        !(HWREG(GPIO_BASE + GPIO_O_DIN31_0) & RX_MASK)) return false;
    if ((HWREG(EVENT_BASE + EVENT_O_RFCSEL9) & 0x7fu) == EVENT_UDMACH14BSEL_EV_AON_PROG2 ||
        (HWREG(EVENT_BASE + EVENT_O_CPUIRQSEL30) & 0x7fu) == EVENT_UDMACH14BSEL_EV_AON_PROG2) return false;
    uint32_t aon_source = (HWREG(AON_EVENT_BASE + AON_EVENT_O_EVTOMCUSEL) >> 16) & 0x3fu;
    if (aon_source != 0x2bu && aon_source != 0x3fu && aon_source != 0u) return false;
    for (unsigned dio = 0; dio < 32u; dio++)
        if (dio != RX_DIO && (HWREG(IOC_BASE + 4u * dio) & IOC_IOCFG0_IOEV_AON_PROG2_EN)) return false;
    uint32_t key = OsalPort_enterCS();
    saved_ioc = HWREGB(IOC_BASE + 4u * RX_DIO + 2u);
    /* IRQ/event ownership of RX must be exclusively ours. */
    if (saved_ioc & 7u) { OsalPort_leaveCS(key); return false; }
    saved_aon = (uint8_t)aon_source;
    saved_dma_event = HWREGB(EVENT_BASE + EVENT_O_UDMACH14BSEL);
    Power_setDependency(PowerCC26XX_PERIPH_GPT0);
    Power_setConstraint(PowerCC26XX_DISALLOW_STANDBY);
    TimerDisable(GPT0_BASE, TIMER_BOTH);
    TimerConfigure(GPT0_BASE, TIMER_CFG_PERIODIC_UP);
    TimerLoadSet(GPT0_BASE, TIMER_A, UINT32_MAX);
    TimerEnable(GPT0_BASE, TIMER_A);
    /* AON IO events latch until explicitly cleared (SWCU185G 13.4). Each
     * timestamp task is followed by a W1C task, allowing the next RX edge.
     * At most eight 1-word tasks run per acquisition; none need a CPU ISR. */
    HWREG(GPIO_BASE + GPIO_O_EVFLAGS31_0) = RX_MASK;
    HWREG(AON_EVENT_BASE + AON_EVENT_O_EVTOMCUSEL) &= ~(0x3fu << 16);
    SysCtrlAonSync();
    HWREG(EVENT_BASE + EVENT_O_UDMACH14BSEL) = EVENT_UDMACH14BSEL_EV_AON_PROG2;
    uDMAChannelAttributeDisable(UDMA0_BASE, CAP_CHANNEL, UDMA_ATTR_ALL);
    uDMAChannelAttributeEnable(UDMA0_BASE, CAP_CHANNEL, UDMA_ATTR_USEBURST | UDMA_ATTR_HIGH_PRIORITY);
    uDMAChannelScatterGatherSet(UDMA0_BASE, CAP_CHANNEL, 8, (void *)tasks, true);
    uDMAChannelEnable(UDMA0_BASE, CAP_CHANNEL);
    active = 1;
    /* Byte write changes ONLY edge/event fields, retaining UART0 RX mux. */
    HWREGB(IOC_BASE + 4u * RX_DIO + 2u) = (saved_ioc & 0xf8u) | 0x83u;
    OsalPort_leaveCS(key);
    return true;
}
void PortBaudCapture_stop(void) {
    if (!active) return;
    uint32_t key = OsalPort_enterCS();
    uDMAChannelAttributeEnable(UDMA0_BASE, CAP_CHANNEL, UDMA_ATTR_REQMASK);
    uDMAChannelDisable(UDMA0_BASE, CAP_CHANNEL);
    HWREGB(IOC_BASE + 4u * RX_DIO + 2u) = saved_ioc;
    HWREG(GPIO_BASE + GPIO_O_EVFLAGS31_0) = RX_MASK;
    HWREG(AON_EVENT_BASE + AON_EVENT_O_EVTOMCUSEL) =
        (HWREG(AON_EVENT_BASE + AON_EVENT_O_EVTOMCUSEL) & ~(0x3fu << 16)) | ((uint32_t)saved_aon << 16);
    SysCtrlAonSync();
    HWREG(EVENT_BASE + EVENT_O_UDMACH14BSEL) = saved_dma_event;
    uDMAChannelAttributeDisable(UDMA0_BASE, CAP_CHANNEL, UDMA_ATTR_ALL);
    TimerDisable(GPT0_BASE, TIMER_BOTH);
    Power_releaseConstraint(PowerCC26XX_DISALLOW_STANDBY);
    Power_releaseDependency(PowerCC26XX_PERIPH_GPT0);
    active = 0;
    OsalPort_leaveCS(key);
}
bool PortBaudCapture_read(uint32_t edges[4]) {
    if (!active || uDMAChannelIsEnabled(UDMA0_BASE, CAP_CHANNEL)) return false;
    for (unsigned i = 0; i < 4; i++) edges[i] = captures[i];
    return true;
}
