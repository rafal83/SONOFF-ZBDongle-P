"""Static worst-case stack depth of the FreeRTOS firmware tasks (ELF analysis).

For every function of the ELF: frame size from its prologue (push / vpush /
sub sp), call graph from bl/b.w to functions. Indirect calls (blx rN) go to
the address-taken functions (Thumb pointers in literal pools, .rodata, .data
or built by movw/movt) of the group the CALLER's source file may call through
a pointer (INDIRECT_RULES: CCM* block hooks, CCM* backends, flash ops of the
store, coordinator/ZNP platform callbacks). Indirect calls from TI driverlib
code go to the ROM (ROM_ALLOWANCE bytes). Recursive components are costed with each member active once per chain.
The result is an upper bound under those rules, to compare with the static
allocation; measurement on the MCU (DIAG 0x1B high-water marks) remains the
reference.

  python tools/stack_analysis.py build/freertos/diag-1000000/sonoff-freertos-diag.elf
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".tooling/arm-tests"))
from elftools.elf.elffile import ELFFile  # noqa: E402
import capstone  # noqa: E402
from capstone import arm as carm  # noqa: E402

# Task entry -> allocated stack (words), see app.h / FreeRTOSConfig.h.
TASKS = {"radio": ("STACK_RADIO", 1280), "host": ("STACK_HOST", 512),
         "supervisor": ("STACK_SUPERVISOR", 256), "prvIdleTask": ("configMINIMAL_STACK_SIZE", 128),
         "prvTimerTask": ("configTIMER_TASK_STACK_DEPTH", 256)}
# Exception frame pushed on the task stack at a context switch / interrupt
# with the FPU in use (lazy stacking reserves the FP area): 26 words + the
# 16 callee-saved FP + 8 core registers saved by PendSV.
SWITCH_FRAME = (26 + 16 + 8) * 4
ROM_ALLOWANCE = 128   # TI ROM driverlib routines called through the ROM tables

# Source file of the caller -> functions it may call through a pointer
# (every callback pointer of the firmware, by the structure holding it).
INDIRECT_RULES = {
    # mac_radio_ops_t (radio_task); upper callbacks: see FUNCTION_RULES
    "mac_core": ["coord_radio_tx", "coord_source_match"],
    # CCM* backend (APS security); zb_coord_upper_t calls: see FUNCTION_RULES
    "zb_coord": ["zb_ccm_backend_sw", "zb_ccm_backend_hw"],
    # core_security: CCM* backend pointer and counter reservation
    "core_security": ["zb_ccm_backend_sw", "zb_ccm_backend_hw", "reserve_adapter"],
    "aps_security": ["zb_ccm_backend_sw", "zb_ccm_backend_hw"],
    "trust_center": ["zb_ccm_backend_sw", "zb_ccm_backend_hw"],
    "ccm_star": ["aes_sw_block", "hw_encrypt"],          # ccm_block_t
    "zb_ccm_backend": ["hal_aes_block"],                 # hardware block hook
    "nv_log": ["hal_flash_read", "hal_flash_program", "hal_flash_erase_sector"],
    "znp": ["znp_network_up", "znp_random", "znp_reset", "znp_send", "znp_tx_power"],   # znp_platform_t
    "npi": ["on_frame"],
    "mt": ["plat_diag", "plat_ieee", "plat_reset"],
}
# Functions that call a callback structure besides their file's default
# (from the call sites: grep "upper." in mac_core.c / zb_coord.c).
UPPER_ALL = ["up_aps", "up_confirm", "up_joined", "up_left", "up_random", "coord_aps", "coord_joined",
             "coord_key", "coord_random_key", "coord_reserve"]
FUNCTION_RULES = {
    "complete": ["on_data_confirm", "on_comm_status"],
    "serve_poll": ["on_poll"],
    "mac_rx": ["on_mac_data", "on_assoc", "on_poll"],
    "reserve_adapter": ["coord_reserve"],
    "on_data_confirm": ["up_confirm"],
    "handle_request_key": ["up_random", "coord_random_key"],
    "handle_verify_key": ["coord_key"],
    "handle_zdo_request": ["up_joined", "coord_joined"],
    "device_gone": ["up_left"],
    "on_mac_data": UPPER_ALL,        # inlines several handlers
}


def load(elf_path):
    f = ELFFile(open(elf_path, "rb"))
    text = f.get_section_by_name(".text")
    base, code = text["sh_addr"], text.data()
    funcs = {}
    for s in f.get_section_by_name(".symtab").iter_symbols():
        if s["st_info"]["type"] == "STT_FUNC" and s["st_size"]:
            a = s["st_value"] & ~1
            if base <= a < base + len(code):
                funcs[a] = (s.name, s["st_size"])
    blobs = [(text["sh_addr"], code)]
    for name in (".rodata", ".data"):
        sec = f.get_section_by_name(name)
        if sec and sec["sh_type"] != "SHT_NOBITS":
            blobs.append((sec["sh_addr"], sec.data()))
    taken = set()
    for _, data in blobs:
        for i in range(0, len(data) - 3, 2):
            v = struct.unpack_from("<I", data, i)[0]
            if v & 1 and (v & ~1) in funcs:
                taken.add(v & ~1)
    # movw/movt pairs building a function address
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    md.detail = True
    for addr, (name, size) in funcs.items():
        lo = {}
        for ins in md.disasm(code[addr - base: addr - base + size], addr):
            if ins.mnemonic.startswith("movw") and len(ins.operands) == 2:
                lo[ins.operands[0].reg] = ins.operands[1].imm & 0xFFFF
            elif ins.mnemonic.startswith("movt") and len(ins.operands) == 2 and ins.operands[0].reg in lo:
                v = lo.pop(ins.operands[0].reg) | ((ins.operands[1].imm & 0xFFFF) << 16)
                if v & 1 and (v & ~1) in funcs:
                    taken.add(v & ~1)
    # function -> source file, from the object files next to the ELF
    files = {}
    obj = Path(elf_path).parent / "obj"
    for o in sorted(obj.glob("*.o")):
        stem = o.stem.split("_", 1)[1] if "_" in o.stem else o.stem
        eo = ELFFile(open(o, "rb"))
        for sym in eo.get_section_by_name(".symtab").iter_symbols():
            if sym["st_info"]["type"] == "STT_FUNC":
                files.setdefault(sym.name, set()).add(stem)
    return base, code, funcs, taken, files


def msp_size(elf_path):
    sec = ELFFile(open(elf_path, "rb")).get_section_by_name(".stack")
    return sec["sh_size"] if sec else 0


def analyse(elf_path):
    base, code, funcs, taken, files = load(elf_path)

    def file_of(a):
        return files.get(funcs[a][0], set())

    def indirect_targets(a):
        allowed = set()
        for f in file_of(a):
            allowed.update(INDIRECT_RULES.get(f, []))
        allowed.update(FUNCTION_RULES.get(funcs[a][0], []))
        return {t for t in taken if funcs[t][0] in allowed}
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    md.detail = True
    frame, calls, indirect = {}, {}, {}
    for addr, (name, size) in funcs.items():
        body = code[addr - base: addr - base + size]
        f, cs, ind = 0, set(), False
        for ins in md.disasm(body, addr):
            m = ins.mnemonic
            if m in ("push", "push.w", "stmdb") and (m != "stmdb" or ins.op_str.startswith("sp!")):
                f += 4 * (len(ins.operands) - (1 if m == "stmdb" else 0))
            elif m in ("vpush", "vpush.64", "vpush.32"):
                f += sum(8 if ins.op_str.count("d") else 4 for _ in ins.operands)
            elif m.startswith("sub") and ins.op_str.startswith("sp, sp, #"):
                f += int(ins.op_str.split("#")[1], 0)
            elif m.startswith("sub") and ins.op_str.startswith("sp, #"):
                f += int(ins.op_str.split("#")[1], 0)
            if m in ("bl", "blx") and len(ins.operands) == 1 and ins.operands and ins.operands[0].type == carm.ARM_OP_IMM:
                t = ins.operands[0].imm & ~1
                if t in funcs:
                    cs.add(t)
            elif m in ("b.w", "b") and ins.operands and ins.operands[0].type == carm.ARM_OP_IMM:
                t = ins.operands[0].imm & ~1
                if t in funcs and t != addr:   # tail call
                    cs.add(t)
            elif m == "blx" and ins.operands and ins.operands[0].type == carm.ARM_OP_REG:
                ind = True
        frame[addr], calls[addr], indirect[addr] = f, cs, ind
    rom = {a for a in funcs if indirect[a] and not indirect_targets(a)}
    unresolved = sorted({funcs[a][0] for a in rom if not any(f in ("setup", "setup_rom", "chipinfo", "prcm", "osc",
                         "ddi", "adi", "sys_ctrl", "ioc", "uart", "interrupt", "cpu", "aon_rtc", "aux_sysif", "rfc",
                         "watchdog", "aon_pmctl", "vims", "trng", "flash", "aes") for f in file_of(a))})
    cycles = set()
    memo = {}
    succ = {a: sorted(set(calls[a]) | (indirect_targets(a) if indirect[a] else set())) for a in funcs}
    # Strongly connected components (iterative Tarjan): a recursion (e.g. MAC
    # completion -> upper callback -> new send -> MAC completion) is costed as
    # every member of its component active once on the same chain, which is a
    # deterministic upper bound under "each function at most once per chain".
    index, low, on, st, comp = {}, {}, set(), [], {}
    counter = [0]
    for root in sorted(funcs):
        if root in index:
            continue
        work = [(root, 0)]
        while work:
            v, i = work.pop()
            if i == 0:
                index[v] = low[v] = counter[0]
                counter[0] += 1
                st.append(v)
                on.add(v)
            recurse = False
            for j in range(i, len(succ[v])):
                w = succ[v][j]
                if w not in index:
                    work.append((v, j + 1))
                    work.append((w, 0))
                    recurse = True
                    break
                if w in on:
                    low[v] = min(low[v], index[w])
            if recurse:
                continue
            if low[v] == index[v]:
                members = []
                while True:
                    w = st.pop()
                    on.discard(w)
                    members.append(w)
                    if w == v:
                        break
                for w in members:
                    comp[w] = v
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[v])
    members_of = {}
    for a, c in comp.items():
        members_of.setdefault(c, []).append(a)
    recursive = {c for c, m in members_of.items() if len(m) > 1 or any(a in succ[a] for a in m)}
    for c in recursive:
        cycles.update(funcs[a][0] for a in members_of[c])

    def depth(a, stack=None):
        c = comp[a]
        if c in memo:
            return memo[c]
        own = sum(frame[m] for m in members_of[c])
        label = ("{" + ", ".join(sorted(f"{funcs[m][0]}({frame[m]})" for m in members_of[c])) + "}"
                 if c in recursive else f"{funcs[a][0]}({frame[a]})")
        best, path = 0, []
        if any(m in rom for m in members_of[c]):
            best, path = ROM_ALLOWANCE, ["<ROM %d>" % ROM_ALLOWANCE]
        for m in members_of[c]:
            for t in succ[m]:
                if comp[t] == c:
                    continue
                d, p = depth(t)
                if d > best:
                    best, path = d, p
        memo[c] = (own + best, [label] + path)
        return memo[c]

    by_name = {n: a for a, (n, _) in funcs.items()}
    report = {"elf": str(elf_path), "switch_frame_bytes": SWITCH_FRAME, "tasks": {},
              "address_taken_functions": len(taken)}
    for task, (macro, words) in TASKS.items():
        if task not in by_name:
            continue
        memo.clear()
        d, path = depth(by_name[task])
        need = d + SWITCH_FRAME
        report["tasks"][task] = {"allocated_bytes": words * 4, "allocation": macro,
                                 "static_worst_bytes": need, "margin_bytes": words * 4 - need,
                                 "worst_path": path[:24]}
    # Interrupts run on the main stack (MSP, .stack section); worst case: every
    # handler nested once (distinct priorities), each with its own frame.
    isr_total, isr = 0, {}
    for name in ("RFC_CPE0_IRQHandler", "RFC_CPE1_IRQHandler", "UART0_IRQHandler", "WDT_IRQHandler",
                 "TRNG_IRQHandler", "SysTick_Handler", "PendSV_Handler", "SVC_Handler", "HardFault_Handler"):
        if name in by_name:
            memo.clear()
            d, path = depth(by_name[name])
            isr[name] = {"bytes": d + 32, "path": path[:10]}   # + basic exception frame
            isr_total += d + 32
    report["interrupts"] = {"msp_allocated_bytes": msp_size(elf_path), "nested_worst_bytes": isr_total,
                            "handlers": isr}
    report["recursive_functions"] = sorted(cycles)
    report["indirect_calls_without_rule"] = unresolved
    big = sorted(((frame[a], funcs[a][0]) for a in funcs), reverse=True)[:15]
    report["largest_frames"] = [{"function": n, "bytes": b} for b, n in big]
    return report


def summary(r):
    lines = []
    for t, v in r["tasks"].items():
        lines.append(f"{t}: {v['static_worst_bytes']} / {v['allocated_bytes']} B (margin {v['margin_bytes']})")
    i = r["interrupts"]
    lines.append(f"interrupts (all nested): {i['nested_worst_bytes']} / {i['msp_allocated_bytes']} B")
    return "; ".join(lines)


if __name__ == "__main__":
    r = analyse(sys.argv[1])
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(json.dumps(r, indent=1) + "\n")
    print(json.dumps(r, indent=1) if len(sys.argv) == 2 else summary(r))
