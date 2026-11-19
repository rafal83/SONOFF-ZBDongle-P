#ifndef TEST_NPI_FRAME_H
#define TEST_NPI_FRAME_H
#include <stdint.h>
typedef enum { NPIMSG_Type_ASYNC, NPIMSG_Type_SYNCREQ, NPIMSG_Type_SYNCRSP } NPIMSG_Type;
typedef void (*npiIncomingFrameCBack_t)(uint8_t, uint8_t *, NPIMSG_Type);
#endif
