/* SPDX-License-Identifier: MIT */
#include "board.h"
#include "diagnostic.h"
#include <inc/hw_memmap.h>
#include <inc/hw_fcfg1.h>
#include <inc/hw_types.h>
#include <driverlib/prcm.h>
#include <driverlib/ioc.h>
#include <driverlib/uart.h>
#include <driverlib/sys_ctrl.h>
#include <driverlib/osc.h>
#include <driverlib/systick.h>

static volatile uint32_t milliseconds;
static npi_parser parser;
static diagnostic device;
void SysTickIntHandler(void) { milliseconds++; }

int main(void) {
    /* ResetISR in TI startup_gcc.c has already run SetupTrimDevice(). */
    OSCClockSourceSet(OSC_SRC_CLK_HF, OSC_XOSC_HF);
    while (!OSCHfSourceReady()) { }
    OSCHfSourceSwitch();
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
    while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL)
           != PRCM_DOMAIN_POWER_ON) { }
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }
    IOCPortConfigureSet(BOARD_UART_TX, IOC_PORT_MCU_UART0_TX, IOC_STD_OUTPUT);
    IOCPortConfigureSet(BOARD_UART_RX, IOC_PORT_MCU_UART0_RX, IOC_STD_INPUT);
    UARTConfigSetExpClk(UART0_BASE, SysCtrlClockGet(), BOARD_UART_BAUD,
                       UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);
    uint32_t mac0 = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0);
    uint32_t mac1 = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_1);
    uint8_t ieee[8];
    for (unsigned i = 0; i < 4; i++) {
        ieee[i] = (uint8_t)(mac0 >> (8u * i));
        ieee[i + 4] = (uint8_t)(mac1 >> (8u * i));
    }
    diagnostic_init(&device, ieee);
    npi_init(&parser, diagnostic_receive, &device);
    SysTickPeriodSet(SysCtrlClockGet() / 1000u);
    SysTickIntEnable();
    SysTickEnable();
    for (;;) {
        /* Bounded work each turn: TX is serviced even under continuous RX. */
        for (unsigned i = 0; i < 32; i++) {
            int32_t byte = UARTCharGetNonBlocking(UART0_BASE);
            if (byte < 0) break;
            if ((byte & 0x0f00) != 0) {
                npi_rx_error(&parser);
                UARTRxErrorClear(UART0_BASE);
                continue;
            }
            npi_feed(&parser, (uint8_t)byte, milliseconds);
        }
        npi_expire(&parser, milliseconds);
        uint8_t byte;
        for (unsigned i = 0; i < 32 && npi_tx_peek(&device.tx, &byte); i++) {
            if (!UARTCharPutNonBlocking(UART0_BASE, byte)) break;
            npi_tx_consume(&device.tx);
        }
    }
}
