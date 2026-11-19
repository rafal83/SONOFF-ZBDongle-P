"""Exercise the independent C NWK security path with real host AES-CCM.

No dongle, network, production keys or mocked crypto. AES-CCM comes from
cryptography/OpenSSL and is cross-checked against PyCryptodome and RFC 3610.
The durable-store callback is a fault-injectable model, not real Flash.
"""
import ctypes as C
from importlib.metadata import version
import json
from pathlib import Path
import random
import struct
import sys
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from Crypto.Cipher import AES

ROOT=Path(__file__).resolve().parents[1]
P=C.POINTER(C.c_uint8)
CCM=C.CFUNCTYPE(C.c_bool,C.c_void_p,C.c_bool,P,P,P,C.c_size_t,P,C.c_size_t,P,P)
RESERVE=C.CFUNCTYPE(C.c_bool,C.c_void_p,C.c_uint32)

def bytes_at(ptr,n): return C.string_at(ptr,n)
def buffer(data): return (C.c_uint8*len(data)).from_buffer_copy(data)

class Test:
    def __init__(self,path):
        self.lib=C.CDLL(str(path.resolve()))
        signatures={
            'size':(C.c_size_t,[]),
            'init':(None,[C.c_void_p,C.c_uint32,CCM,RESERVE,C.c_void_p]),
            'key':(C.c_bool,[C.c_void_p,C.c_uint,C.c_uint8,P]),
            'peer':(C.c_bool,[C.c_void_p,P]),'delete':(C.c_bool,[C.c_void_p,P]),
            'restore':(C.c_bool,[C.c_void_p,P,C.c_uint8,C.c_uint32]),
            'seal':(C.c_bool,[C.c_void_p,C.c_uint,P,P,C.c_size_t,C.c_size_t,C.c_size_t,C.POINTER(C.c_size_t)]),
            'open':(C.c_bool,[C.c_void_p,P,C.c_size_t,C.c_size_t,C.POINTER(C.c_size_t)]),
            'tx_next':(C.c_uint32,[C.c_void_p]),
            'counter':(C.c_uint32,[C.c_void_p,P,C.c_uint]),'clear':(C.c_bool,[C.c_void_p])}
        for name,(result,args) in signatures.items():
            func=getattr(self.lib,'test_security_'+name); func.restype=result; func.argtypes=args; setattr(self,name,func)
        self.errors=[]; self.crypto_calls=0; self.failed_auth=0; self.state={}
        self.ccm=CCM(self.crypto); self.reserve_callback=RESERVE(self.persist)
    def persist(self,ctx,upper):
        state=self.state[ctx]; state['reserve_calls']+=1
        if state['fail_reserve']: return False
        if upper<=state['upper']: self.errors.append('Reservation did not advance'); return False
        state['upper']=upper; return True
    def crypto(self,ctx,encrypt,key,nonce,aad,aad_len,source,length,out,mic):
        try:
            self.crypto_calls+=1
            raw_key,raw_nonce,associated=bytes_at(key,16),bytes_at(nonce,13),bytes_at(aad,aad_len)
            text=bytes_at(source,length); cipher=AESCCM(raw_key,tag_length=4)
            if encrypt:
                if int.from_bytes(raw_nonce[8:12],'little')>=self.state[ctx]['upper']:
                    self.errors.append('Encryption preceded durable reservation'); return False
                if self.state[ctx]['fail_crypto']: return False
                sealed=cipher.encrypt(raw_nonce,text,associated)
                # Independent implementation checks every generated packet.
                other=AES.new(raw_key,AES.MODE_CCM,nonce=raw_nonce,mac_len=4)
                other.update(associated); ciphertext=other.encrypt(text)
                if sealed!=ciphertext+other.digest(): self.errors.append('Independent AES-CCM mismatch'); return False
                C.memmove(out,sealed[:-4],length); C.memmove(mic,sealed[-4:],4)
            else:
                try: plain=cipher.decrypt(raw_nonce,text+bytes_at(mic,4),associated)
                except InvalidTag:
                    self.failed_auth+=1
                    # Model a hardware driver that leaves untrusted output.
                    C.memset(out,0xa5,length); return False
                C.memmove(out,plain,length)
            return True
        except Exception as exc:
            self.errors.append(type(exc).__name__+': '+str(exc)); return False
    def create(self,identifier,upper=0):
        self.state[identifier]={'upper':upper,'reserve_calls':0,'fail_reserve':False,'fail_crypto':False}
        obj=C.create_string_buffer(self.size()); self.init(obj,upper,self.ccm,self.reserve_callback,identifier); return obj

def run(path):
    t=Test(path)
    # RFC 3610 section 8 packet vector 1 checks the host primitive separately.
    rfc_key=bytes(range(0xc0,0xd0)); nonce=bytes.fromhex('00000003020100a0a1a2a3a4a5')
    expected=bytes.fromhex('588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0')
    assert AESCCM(rfc_key,tag_length=8).encrypt(nonce,bytes(range(8,31)),bytes(range(8)))==expected
    tx,rx=t.create(1),t.create(2)
    key=buffer(bytes(range(16))); address=buffer(bytes.fromhex('0807060504030201'))
    for s in [tx,rx]: assert t.key(s,0,7,key)
    assert t.peer(rx,address)
    randomizer=random.Random(0x2652)
    packets=[]; tampered=0
    for i in range(1105):
        payload=randomizer.randbytes(i%101+1)
        header=bytes.fromhex('0800ffff00001e01')
        frame=buffer(header+payload+bytes(127-8-len(payload)))
        size=C.c_size_t()
        assert t.seal(tx,0,address,frame,8,len(payload),127,C.byref(size))
        encoded=bytes(frame[:size.value]); packets.append(encoded)
        assert int.from_bytes(encoded[9:13],'little')==i
        if i<30:
            # Alter every byte position of representative frames, including
            # the authenticated header, sender, counter, sequence and MIC.
            for j in range(len(encoded)):
                damaged=bytearray(encoded); damaged[j]^=0x80
                bad=buffer(damaged); old=bytes(bad); plain=C.c_size_t(999)
                assert not t.open(rx,bad,len(bad),8,C.byref(plain))
                assert bytes(bad)==old and plain.value==0
                tampered+=1
        restored=buffer(encoded); plain=C.c_size_t()
        assert t.open(rx,restored,len(restored),8,C.byref(plain))
        assert plain.value==len(payload) and bytes(restored[22:22+plain.value])==payload
        assert t.counter(rx,address,0)==i
        replay=buffer(encoded); old=bytes(replay)
        assert not t.open(rx,replay,len(replay),8,C.byref(plain)) and bytes(replay)==old
    assert t.state[1]['reserve_calls']==2 and t.state[1]['upper']==2002
    # Reservation failure leaves output and tx_next unchanged; crypto failure
    # consumes a nonce but does not expose a partial packet.
    failed=t.create(3,3000); assert t.key(failed,0,7,key)
    t.state[3]['fail_reserve']=True
    frame=buffer(bytes.fromhex('0800ffff00001e01')+b'hello'+bytes(114)); old=bytes(frame); size=C.c_size_t()
    assert not t.seal(failed,0,address,frame,8,5,127,C.byref(size)) and bytes(frame)==old and t.tx_next(failed)==3000
    t.state[3]['fail_reserve']=False; t.state[3]['fail_crypto']=True
    assert not t.seal(failed,0,address,frame,8,5,127,C.byref(size)) and bytes(frame)==old and t.tx_next(failed)==3001
    t.state[3]['fail_crypto']=False
    assert t.seal(failed,0,address,frame,8,5,127,C.byref(size)) and int.from_bytes(bytes(frame[9:13]),'little')==3001
    # Reboot starts at the previous durable exclusive bound, never at a
    # recently used volatile counter. Persisted incoming counters reject replay.
    reboot=t.create(4,t.state[1]['upper']); assert t.key(reboot,0,7,key)
    frame=buffer(bytes.fromhex('0800ffff00001e01')+b'hi'+bytes(117))
    assert t.seal(reboot,0,address,frame,8,2,127,C.byref(size)) and int.from_bytes(bytes(frame[9:13]),'little')==2002
    receiver=t.create(5); assert t.key(receiver,0,7,key) and t.peer(receiver,address)
    assert t.restore(receiver,address,7,1104)
    assert not t.restore(receiver,address,7,1000)
    assert not t.open(receiver,buffer(packets[-1]),len(packets[-1]),8,C.byref(size))
    # Active and alternate key contexts have separate replay state. Restoring
    # the same key is idempotent; rollover never resets the outgoing counter.
    alternate=buffer(bytes(range(16,32)))
    for s in [tx,rx]: assert t.key(s,1,8,alternate)
    assert not t.key(tx,1,7,alternate)
    assert t.key(rx,0,7,key) and t.counter(rx,address,0)==1104
    frame=buffer(bytes.fromhex('0800ffff00001e01')+b'abc'+bytes(116))
    assert t.seal(tx,1,address,frame,8,3,127,C.byref(size))
    assert t.open(rx,frame,size.value,8,C.byref(size)) and t.counter(rx,address,1)==1105
    # Valid optional NWK fields, including combined IEEE, multicast and a
    # two-relay source route. Header bytes participate in authentication.
    for options in [8,16,24,1,4,31]:
        header=bytearray([8,options,255,255,0,0,30,1])
        if options&8: header+=bytes(range(8))
        if options&16: header+=bytes(range(8,16))
        if options&1: header+=b'\x00'
        if options&4: header+=bytes.fromhex('020101000200')
        payload=b'APS-payload'; frame=buffer(bytes(header)+payload+bytes(127-len(header)-len(payload)))
        assert t.seal(tx,0,address,frame,len(header),len(payload),127,C.byref(size))
        wire=bytes(frame[:size.value]); plain=C.c_size_t()
        assert t.open(rx,frame,size.value,len(header),C.byref(plain))
        assert bytes(frame[len(header)+14:len(header)+14+plain.value])==payload
        if options&4:
            damaged=bytearray(wire); damaged[len(header)-6]=3
            assert not t.open(rx,buffer(damaged),len(damaged),len(header),C.byref(plain))
    empty=buffer(bytes.fromhex('0800ffff00001e01')+bytes(119))
    assert not t.seal(tx,0,address,empty,8,0,127,C.byref(size))
    # Last allowed counter and wrap refusal.
    last=t.create(6,0xfffffffe); assert t.key(last,0,7,key)
    frame=buffer(bytes.fromhex('0800ffff00001e01')+b'a'+bytes(118))
    assert t.seal(last,0,address,frame,8,1,127,C.byref(size))
    old=bytes(frame); assert not t.seal(last,0,address,frame,8,1,127,C.byref(size)) and bytes(frame)==old
    # Fully occupied TC-sized peer index and complete reuse.
    full=t.create(7)
    for i in range(200): assert t.peer(full,buffer(struct.pack('<Q',i)))
    assert not t.peer(full,buffer(struct.pack('<Q',200)))
    for i in range(200): assert t.delete(full,buffer(struct.pack('<Q',i)))
    for i in range(200): assert t.peer(full,buffer(struct.pack('<Q',i+200)))
    assert t.clear(full)
    assert not t.errors,t.errors
    return dict(passed=True,crypto='cryptography AESCCM / OpenSSL, cross-check PyCryptodome',
        backend_versions={name:version(name) for name in ['cryptography','pycryptodome']},
        RFC3610_vector_1_passed=True,encrypted_frames=1105,optional_header_frames=6,tampered_frames_rejected=tampered,
        crypto_calls=t.crypto_calls,invalid_tags=t.failed_auth,peer_capacity=200,
        durable_counter_model_tested=True,real_Flash_tested=False,security_context_bytes=t.size(),
        full_SSP_APS_Trust_Center_replacement=False,production_integration=False)

if __name__=='__main__':
    report=run(Path(sys.argv[1])); print(json.dumps(report))
    if len(sys.argv)>2: Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
