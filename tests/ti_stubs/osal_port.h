#include <stdint.h>
#include <stddef.h>
typedef void (*OsalPort_TimerCback)(void *);
typedef void *OsalPort_TimerID;
#define OsalPort_SUCCESS 0
#define OsalPort_INVALIDPARAMETER 2
#define OsalPort_NO_TIMER_AVAIL 8
uint32_t OsalPort_enterCS(void);
void OsalPort_leaveCS(uint32_t);
void *OsalPort_msgAllocate(uint16_t);
void OsalPort_msgDeallocate(void *);
uint8_t OsalPort_setEvent(uint8_t task, uint32_t event);
