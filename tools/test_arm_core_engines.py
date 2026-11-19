"""Run TI-compiled core functions in Cortex-M4F Unicorn, with host AES-CCM.

The isolated ELF has no coordinator/radio/ROM simulation or recovery config.
Only the crypto and persistence boundary calls are intercepted.
"""
import hashlib
from pathlib import Path
import random
import struct
import sys
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'.tooling/arm-tests'))
import unicorn
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_XPSR, UC_ARM_REG_PC

INPUT=0x21000000
END=0x5f000
STACK=0x20013e00
REGISTERS=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3]

class Image:
    def __init__(self,path):
        if unicorn.__version__!='2.1.3': raise ValueError('Expected pinned Unicorn 2.1.3')
        self.vm=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
        self.vm.mem_map(0,0x60000)
        self.vm.mem_map(0x20000000,0x14000)
        self.vm.mem_map(INPUT,0x1000)
        with path.open('rb') as stream:
            elf=ELFFile(stream)
            self.symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
            for section in elf.iter_sections():
                if section['sh_flags']&2 and section['sh_size'] and section['sh_type']!='SHT_NOBITS':
                    self.vm.mem_write(section['sh_addr'],section.data())
        self.crypto=self.address('core_test_ccm')&~1
        self.persist=self.address('core_test_reserve')&~1
        self.upper=0; self.crypto_calls=0; self.reservations=[]; self.calls=0; self.stack_high_water=0
        self.vm.hook_add(UC_HOOK_CODE,self.hook)
    def address(self,name): return self.symbols[name]['st_value']
    def read(self,address,size): return bytes(self.vm.mem_read(address,size))
    def u32(self,address): return int.from_bytes(self.read(address,4),'little')
    def hook(self,vm,address,size,userdata):
        self.stack_high_water=max(self.stack_high_water,STACK-vm.reg_read(UC_ARM_REG_SP))
        if address==END:
            self.stopped=True; vm.emu_stop(); return
        if address not in [self.crypto,self.persist]: return
        args=[vm.reg_read(r) for r in REGISTERS]
        if address==self.persist:
            assert args[1]>self.upper
            self.upper=args[1]; self.reservations.append(self.upper); ok=True
        else:
            self.crypto_calls+=1
            encrypt,key,nonce=args[1:]
            aad,aadlen,source,length,out,mic=struct.unpack('<6I',self.read(vm.reg_read(UC_ARM_REG_SP),24))
            cipher=AESCCM(self.read(key,16),tag_length=4)
            raw_nonce=self.read(nonce,13); associated=self.read(aad,aadlen); data=self.read(source,length)
            if encrypt:
                assert int.from_bytes(raw_nonce[8:12],'little')<self.upper
                sealed=cipher.encrypt(raw_nonce,data,associated)
                vm.mem_write(out,sealed[:-4]); vm.mem_write(mic,sealed[-4:]); ok=True
            else:
                try:
                    plain=cipher.decrypt(raw_nonce,data+self.read(mic,4),associated)
                    vm.mem_write(out,plain); ok=True
                except InvalidTag:
                    vm.mem_write(out,b'\xa5'*length); ok=False
        vm.reg_write(UC_ARM_REG_R0,int(ok)); vm.reg_write(UC_ARM_REG_PC,vm.reg_read(UC_ARM_REG_LR))
    def call(self,name,*args):
        self.calls+=1; self.stopped=False
        for i,register in enumerate(REGISTERS): self.vm.reg_write(register,args[i] if i<len(args) else 0)
        self.vm.reg_write(UC_ARM_REG_SP,STACK)
        if len(args)>4: self.vm.mem_write(STACK,struct.pack('<'+'I'*(len(args)-4),*args[4:]))
        self.vm.reg_write(UC_ARM_REG_XPSR,0x01000000)
        self.vm.reg_write(UC_ARM_REG_LR,END|1)
        self.vm.emu_start(self.address(name)|1,END+2,count=200000)
        if not self.stopped: raise AssertionError('ARM function exceeded instruction bound: '+name)
        return self.vm.reg_read(UC_ARM_REG_R0)
    def result64(self): return self.vm.reg_read(UC_ARM_REG_R0)|(self.vm.reg_read(UC_ARM_REG_R1)<<32)

def run(path):
    vm=Image(path); indirect=vm.address('core_test_indirect'); bcast=vm.address('core_test_broadcast'); security=vm.address('core_test_security')
    vm.call('core_indirect_init',indirect)
    tokens=[]
    for i in range(24):
        vm.call('core_indirect_enqueue',indirect,i//3,INPUT+0x900+i)
        token=vm.result64(); assert token!=0xffffffffffffffff; tokens.append(token)
    vm.call('core_indirect_enqueue',indirect,200,INPUT+0x900)
    assert vm.result64()==0xffffffffffffffff
    for i in range(8): assert vm.call('core_indirect_count',indirect,i)==3
    vm.call('core_indirect_poll',indirect,0,INPUT+0x80); assert vm.result64()==tokens[0]
    assert vm.u32(INPUT+0x80)==INPUT+0x900
    assert vm.call('core_indirect_retry',indirect,0,tokens[0]&0xffffffff,tokens[0]>>32)
    for token in tokens: assert vm.call('core_indirect_release',indirect,0,token&0xffffffff,token>>32,INPUT+0x80)
    # Cortex-M4 AAPCS aligns a uint64_t argument at r2/r3, verified above.
    vm.call('core_broadcast_init',bcast,0xfffffffe)
    vm.vm.mem_write(INPUT+0x100,struct.pack('<4I',1,0,0,0x80000000))
    assert vm.call('core_broadcast_admit',bcast,0x1234,255,3,1,1,INPUT+0x100,INPUT+0x80)==0
    slot=vm.u32(INPUT+0x80)&0xffff
    assert vm.call('core_broadcast_admit',bcast,0x1234,255,3,1,1,INPUT+0x100,INPUT+0x80)==1
    assert vm.call('core_broadcast_ack',bcast,0x1234,255,127)
    assert not vm.call('core_broadcast_ack',bcast,0x1234,255,127)
    assert vm.call('core_broadcast_confirm',bcast,0x1234,255)
    vm.call('core_broadcast_tick',bcast,INPUT+0x140)
    assert int.from_bytes(vm.read(INPUT+0x148,8),'little')==1<<slot
    vm.call('core_broadcast_tick',bcast,INPUT+0x140)
    vm.call('core_broadcast_tick',bcast,INPUT+0x140)
    assert int.from_bytes(vm.read(INPUT+0x140,8),'little')==1<<slot
    # Sleeping-child delivery shares the 24-buffer quota without duplicating
    # one payload per child. A failed MAC confirmation retains its recipient.
    vm.call('core_indirect_broadcast_enqueue',indirect,INPUT+0x900,INPUT+0x100)
    shared=vm.result64(); assert shared!=0xffffffffffffffff
    low,high=shared&0xffffffff,shared>>32
    assert vm.call('core_indirect_broadcast_pending',indirect,0,low,high)==2
    vm.call('core_indirect_broadcast_poll',indirect,0x1234,127,INPUT+0x80)
    assert vm.result64()==shared
    assert vm.call('core_indirect_broadcast_confirm',indirect,0,low,high,0)
    assert vm.call('core_indirect_broadcast_pending',indirect,0,low,high)==2
    vm.call('core_indirect_broadcast_poll',indirect,0x1234,127,INPUT+0x80)
    assert vm.result64()==shared
    assert vm.call('core_indirect_broadcast_confirm',indirect,0,low,high,1)
    assert vm.call('core_indirect_broadcast_pending',indirect,0,low,high)==1
    assert vm.call('core_indirect_release',indirect,0,low,high,INPUT+0x80)
    # Actual TI-compiled frame parser, replay state and reservation ordering.
    vm.call('core_security_init',security,0,vm.address('core_test_ccm'),vm.address('core_test_reserve'),0)
    vm.vm.mem_write(INPUT,bytes(range(16))); vm.vm.mem_write(INPUT+0x20,bytes.fromhex('0807060504030201'))
    assert vm.call('core_security_set_key',security,0,7,INPUT)
    assert vm.call('core_security_register',security,INPUT+0x20)
    for i in range(30):
        plain=bytes(range(i+1)); header=bytes.fromhex('0800ffff00001e01')
        vm.vm.mem_write(INPUT+0x200,header+plain+bytes(119-len(plain)))
        assert vm.call('core_security_seal',security,0,INPUT+0x20,INPUT+0x200,8,len(plain),127,INPUT+0x80)
        length=vm.u32(INPUT+0x80); packet=vm.read(INPUT+0x200,length)
        assert int.from_bytes(packet[9:13],'little')==i
        vm.vm.mem_write(INPUT+0x200+length-1,bytes([packet[-1]^1]))
        assert not vm.call('core_security_open',security,INPUT+0x200,length,8,INPUT+0x80)
        vm.vm.mem_write(INPUT+0x200,packet)
        assert vm.call('core_security_open',security,INPUT+0x200,length,8,INPUT+0x80)
        assert vm.u32(INPUT+0x80)==len(plain) and vm.read(INPUT+0x216,len(plain))==plain
        vm.vm.mem_write(INPUT+0x200,packet)
        calls=vm.crypto_calls
        assert not vm.call('core_security_open',security,INPUT+0x200,length,8,INPUT+0x80)
        assert vm.crypto_calls==calls
    assert vm.reservations==[1001]
    # Place each malformed input at the very end of a mapped page, so a read
    # beyond the declared packet faults instead of reading adjacent test data.
    rng=random.Random(0x20261117)
    for i in range(1500):
        length=rng.randrange(0,151); header=rng.randrange(0,180)
        raw=rng.randbytes(length); pointer=INPUT+0x1000-length
        if raw: vm.vm.mem_write(pointer,raw)
        assert not vm.call('core_security_open',security,pointer,length,header,INPUT+0x80)
    sizes={name:vm.symbols['core_test_'+name]['st_size'] for name in ['indirect','broadcast','security']}
    return dict(passed=True,elf_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),compiler='TI Arm Clang 4.0.2.LTS',
        ARM_calls=vm.calls,ARM_security_frames=30,page_boundary_malformed_inputs=1500,
        host_AES_CCM=True,real_crypto_peripheral_tested=False,real_Flash_tested=False,
        observed_stack_bytes=vm.stack_high_water,context_bytes=sizes,total_context_bytes=sum(sizes.values()),
        production_integration=False,hardware_tested=False)
