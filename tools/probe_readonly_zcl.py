"""Read one authorized bulb's OnOff attribute, without Z2M or commissioning.

Only runtime AF endpoint registration and a ZCL Read Attributes are sent.
No ZCL set, NV write, network startup, reset or permitJoin is provided.
"""
import argparse
import json
from pathlib import Path
import struct
import time
from dongle_probe import MT,extract,identify_port

ROOT=Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ['port','usb-serial','expect-ieee','session-dir']: ap.add_argument('--'+name,required=True)
    ap.add_argument('--expected-revision',type=int,required=True)
    args=ap.parse_args(); folder=Path(args.session_dir).resolve()
    if not folder.is_relative_to((ROOT/'private').resolve()): raise ValueError('Private session required')
    target='0x842e14fffe2fadf0'
    rows=[json.loads(s) for s in (folder/'z2m-data/database.db').read_text(encoding='utf-8').splitlines() if s.strip()]
    peers=[r for r in rows if r.get('ieeeAddr')==target]
    if len(peers)!=1 or peers[0].get('type')!='Router': raise ValueError('Authorized test bulb absent')
    address=peers[0]['nwkAddr']; identify_port(args.port,args.usb_serial)
    report=dict(expected_revision=args.expected_revision,Z2M_used=False,commissioning_commands_sent=False,
        NV_writes_sent=False,state_changing_ZCL_commands_sent=0,read_attributes_requests=0)
    mt=MT(args.port,1000000)
    try:
        version,_=mt.request(0x21,2); ieee,_=mt.request(0x21,4)
        if int.from_bytes(version[5:9],'little')!=args.expected_revision or int.from_bytes(ieee,'little')!=int(args.expect_ieee,16):
            raise ValueError('Firmware/IEEE identity mismatch')
        registered,_=mt.request(0x24,0,struct.pack('<BHHBBBBH',1,0x104,0,0,0,0,1,6))
        if registered!=b'\0': raise ValueError('Runtime endpoint registration failed')
        seq=0x5d; read=bytes([0x10,seq,0,0,0])
        request=struct.pack('<HBBHBBBB',address,1,1,6,seq,0,30,len(read))+read
        status,_=mt.request(0x24,1,request)
        report.update(data_request_status=status.hex(),data_request_accepted=status==b'\0',read_attributes_requests=1,
            radio_peer_readback_verified=False)
        if status!=b'\0': raise ValueError('AF rejected the read request')
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            mt.buffer.extend(mt.serial.read(max(1,min(mt.serial.in_waiting,1024))))
            for cmd0,cmd1,payload in extract(mt.buffer):
                if (cmd0,cmd1)==(0x44,0x80) and len(payload)>=3 and payload[2]==seq:
                    report['AF_data_confirm_status']=payload[0]
                if (cmd0,cmd1)!=(0x44,0x81) or len(payload)<17: continue
                cluster,source=struct.unpack_from('<HH',payload,2); zcl=payload[17:17+payload[16]]
                if cluster!=6 or source!=address or len(zcl)<8: continue
                if zcl[1:3]!=bytes([seq,1]) or zcl[3:7]!=b'\0\0\0\x10' or zcl[7] not in (0,1): continue
                report.update(radio_peer_readback_verified=True,OnOff_value=bool(zcl[7]),link_quality=payload[9])
            if report['radio_peer_readback_verified']: break
    finally:
        mt.close()
        (folder/'readonly-ZCL-probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(report))

if __name__=='__main__': main()
