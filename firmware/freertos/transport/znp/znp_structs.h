/* SPDX-License-Identifier: MIT
 * Generated from zigbee-herdsman 11.2.1 (aligned layout).
 * Copyright (c) 2019 Jack Wu, Simen Li, Hedy Wang and Koen Kanters.
 * See LICENSES/zigbee-herdsman-MIT.txt for the complete upstream notice.
 */
#ifndef ZNP_STRUCTS_H
#define ZNP_STRUCTS_H
#define ZNP_NIB_SIZE 116u
#define ZNP_NIB_SEQUENCENUM 0u /* uint8 */
#define ZNP_NIB_PASSIVEACKTIMEOUT 1u /* uint8 */
#define ZNP_NIB_MAXBROADCASTRETRIES 2u /* uint8 */
#define ZNP_NIB_MAXCHILDREN 3u /* uint8 */
#define ZNP_NIB_MAXDEPTH 4u /* uint8 */
#define ZNP_NIB_MAXROUTERS 5u /* uint8 */
#define ZNP_NIB_DUMMYNEIGHBORTABLE 6u /* uint8 */
#define ZNP_NIB_BROADCASTDELIVERYTIME 7u /* uint8 */
#define ZNP_NIB_REPORTCONSTANTCOST 8u /* uint8 */
#define ZNP_NIB_ROUTEDISCRETRIES 9u /* uint8 */
#define ZNP_NIB_DUMMYROUTINGTABLE 10u /* uint8 */
#define ZNP_NIB_SECUREALLFRAMES 11u /* uint8 */
#define ZNP_NIB_SECURITYLEVEL 12u /* uint8 */
#define ZNP_NIB_SYMLINK 13u /* uint8 */
#define ZNP_NIB_CAPABILITYFLAGS 14u /* uint8 */
#define ZNP_NIB_TRANSACTIONPERSISTENCETIME 16u /* uint16 */
#define ZNP_NIB_NWKPROTOCOLVERSION 18u /* uint8 */
#define ZNP_NIB_ROUTEDISCOVERYTIME 19u /* uint8 */
#define ZNP_NIB_ROUTEEXPIRYTIME 20u /* uint8 */
#define ZNP_NIB_NWKDEVADDRESS 22u /* uint16 */
#define ZNP_NIB_NWKLOGICALCHANNEL 24u /* uint8 */
#define ZNP_NIB_NWKCOORDADDRESS 26u /* uint16 */
#define ZNP_NIB_NWKCOORDEXTADDRESS 28u /* uint8array-reversed[8] */ /* reversed on the wire */
#define ZNP_NIB_NWKPANID 36u /* uint16 */
#define ZNP_NIB_NWKSTATE 38u /* uint8 */
#define ZNP_NIB_CHANNELLIST 40u /* uint32 */
#define ZNP_NIB_BEACONORDER 44u /* uint8 */
#define ZNP_NIB_SUPERFRAMEORDER 45u /* uint8 */
#define ZNP_NIB_SCANDURATION 46u /* uint8 */
#define ZNP_NIB_BATTLIFEEXT 47u /* uint8 */
#define ZNP_NIB_ALLOCATEDROUTERADDRESSES 48u /* uint32 */
#define ZNP_NIB_ALLOCATEDENDDEVICEADDRESSES 52u /* uint32 */
#define ZNP_NIB_NODEDEPTH 56u /* uint8 */
#define ZNP_NIB_EXTENDEDPANID 57u /* uint8array-reversed[8] */ /* reversed on the wire */
#define ZNP_NIB_NWKKEYLOADED 65u /* uint8 */
#define ZNP_NIB_SPARE3 100u /* uint8 */
#define ZNP_NIB_SPARE4 101u /* uint8 */
#define ZNP_NIB_NWKLINKSTATUSPERIOD 102u /* uint8 */
#define ZNP_NIB_NWKROUTERAGELIMIT 103u /* uint8 */
#define ZNP_NIB_NWKUSEMULTICAST 104u /* uint8 */
#define ZNP_NIB_NWKISCONCENTRATOR 105u /* uint8 */
#define ZNP_NIB_NWKCONCENTRATORDISCOVERYTIME 106u /* uint8 */
#define ZNP_NIB_NWKCONCENTRATORRADIUS 107u /* uint8 */
#define ZNP_NIB_NWKALLFRESH 108u /* uint8 */
#define ZNP_NIB_NWKMANAGERADDR 110u /* uint16 */
#define ZNP_NIB_NWKTOTALTRANSMISSIONS 112u /* uint16 */
#define ZNP_NIB_NWKUPDATEID 114u /* uint8 */
#define ZNP_KEYDESC_SIZE 18u
#define ZNP_KEYDESC_KEYSEQNUM 0u /* uint8 */
#define ZNP_KEYDESC_KEY 1u /* uint8array[16] */
#define ZNP_HASCFG_SIZE 2u
#define ZNP_HASCFG_HASCONFIGURED 0u /* uint8 */
#define ZNP_AME_SIZE 12u
#define ZNP_AME_USER 0u /* uint8 */
#define ZNP_AME_NWKADDR 2u /* uint16 */
#define ZNP_AME_EXTADDR 4u /* uint8array-reversed[8] */ /* reversed on the wire */
#define ZNP_TCLK_SIZE 20u
#define ZNP_TCLK_TXFRMCNTR 0u /* uint32 */
#define ZNP_TCLK_RXFRMCNTR 4u /* uint32 */
#define ZNP_TCLK_EXTADDR 8u /* uint8array-reversed[8] */ /* reversed on the wire */
#define ZNP_TCLK_KEYATTRIBUTES 16u /* uint8 */
#define ZNP_TCLK_KEYTYPE 17u /* uint8 */
#define ZNP_TCLK_SEEDSHIFT_ICINDEX 18u /* uint8 */
#define ZNP_SECMAT_SIZE 12u
#define ZNP_SECMAT_FRAMECOUNTER 0u /* uint32 */
#define ZNP_SECMAT_EXTENDEDPANID 4u /* uint8array-reversed[8] */ /* reversed on the wire */
#define ZNP_APSKEY_SIZE 24u
#define ZNP_APSKEY_KEY 0u /* uint8array[16] */
#define ZNP_APSKEY_TXFRMCNTR 16u /* uint32 */
#define ZNP_APSKEY_RXFRMCNTR 20u /* uint32 */
#define ZNP_SME_SIZE 6u
#define ZNP_SME_AMI 0u /* uint16 */
#define ZNP_SME_KEYNVID 2u /* uint16 */
#define ZNP_SME_AUTHENTICATIONOPTION 4u /* uint8 */
#endif
