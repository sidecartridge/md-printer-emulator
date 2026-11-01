; User firmware module: the printer hook
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; main.s hands control here (rom_function: jmp USERFW) once the RP signals
; CMD_START, at every boot, after GEMDOS has started; this module runs from
; the cartridge at $FA0800 and is linked at offset $0800, so everything in it
; is PC-relative.
;
; It installs the printer hook resident in RAM and returns to TOS: the hook
; (resident_start to resident_end) is copied into memory it takes with
; Malloc, owned by GEMDOS's initial process, which never ends (as
; md-drives-emulator's GEMDRIVE takes its stack). A real TSR cannot be made
; here: Pexec and Ptermres at this point of the boot crashed TOS 1.04, 1.62
; and 2.06 further on. Once installed, the ST runs no code in the cartridge.
; An RP that restarts (a reflash, SELECT) stops serving the cartridge, and an
; ST running code there crashes; the hook only reads the cartridge, as data,
; and a cartridge that is not there reads as a switch that is off and a send
; that is never answered.
;
; Every way a program prints through TOS ends in four system vectors
; (measured on TOS 1.04, 1.62 and 2.06): the BIOS printer device's output and
; status, xconout[0] and xcostat[0], which GEMDOS's Cprnout and Fwrite to PRN:
; also reach, and the screen dump's own, prv_lst and prv_lsto (Alt-Help,
; XBIOS Scrdmp). Each points at a routine of the hook, after its XBRA header.
;
; The hook: status answers ready (-1); output takes the byte at 6(sp), as
; TOS's own routine does, into the buffer and answers -1. A full buffer goes
; to the RP at once as a block (PRT_CMD_BLOCK, numbered: a retry resends the
; number, and the RP drops a block it has just taken); a routine in the VBL
; queue sends what is left once printing has paused for PRT_IDLE_FRAMES. A
; lock keeps the two from sending at once: the VBL routine leaves the buffer
; alone while the hook works. Until the RP turns the printer on (shared
; variable PRT_SVAR_ENABLED), the hook hands every call to the routine it
; replaced, so the ST prints as without the cartridge.
;
; Machines: ST, STE, Mega ST, Mega STE (68000). A 68030 would need its
; instruction cache cleared after the hook is copied.

	include inc/sidecart_layout.s
	include inc/printer_layout.s
	include inc/sidecart_macros.s
	include inc/tos.s

	section text

GEMDOS_Cconws		equ $09
GEMDOS_Malloc		equ $48
RESIDENT_BYTES		equ (resident_end - resident_start)

userfw:
	bra.w install

; ---------------------------------------------------------------------------
; The resident hook: copied into RAM, never run from the cartridge.
; ---------------------------------------------------------------------------
	even
resident_start:
prt_state:
	ds.b PRT_STATE_BYTES				; set by the installer

; xbra NAME: the XBRA header of the routine NAME; NAME_old, the vector it
; replaced.
xbra		macro
	dc.l $58425241						; 'XBRA'
	dc.l PRT_XBRA_ID
\1_old:
	dc.l 0
	endm

; The hook. TOS calls these as its own printer routines: the byte at 6(sp),
; d0 the answer, d0-d2 and a0-a2 free.

	xbra prt_conout
prt_conout:
	lea prt_state(pc), a0
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	beq.s take_byte
	move.l prt_conout_old(pc), a0
	jmp (a0)

	xbra prt_lst
prt_lst:
	lea prt_state(pc), a0
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	beq.s take_byte
	move.l prt_lst_old(pc), a0
	jmp (a0)

; take_byte: reached by a branch from prt_conout and prt_lst, so the
; caller's frame is as TOS made it: the byte at 7(sp). Into the buffer, the
; buffer to the RP when full; answers -1. Holds the lock meanwhile, so the
; VBL routine waits.
take_byte:
	st (PRT_LOCK,a0)
	move.l (PRT_WIDX,a0), d0
	move.l (PRT_BUFFER,a0), a1
	move.b 7(sp), (a1,d0.l)
	addq.l #1, d0
	move.l d0, (PRT_WIDX,a0)
	addq.l #1, (PRT_TAKEN,a0)
	move.l _frclock_var.w, (PRT_LAST,a0)
	cmp.l #PRT_BUF_SIZE, d0
	blo.s .kept
	bsr send_block
.kept:
	sf (PRT_LOCK,a0)
	moveq #-1, d0
	rts

	xbra prt_costat
prt_costat:
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	bne.s .off
	moveq #-1, d0
	rts
.off:
	move.l prt_costat_old(pc), a0
	jmp (a0)

	xbra prt_lsto
prt_lsto:
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	bne.s .off
	moveq #-1, d0
	rts
.off:
	move.l prt_lsto_old(pc), a0
	jmp (a0)

; prt_vbl: from the VBL queue, every frame. Sends what the buffer holds once
; printing has paused for PRT_IDLE_FRAMES, unless the hook holds the lock.
; The VBL handler has saved every register.
	xbra prt_vbl
prt_vbl:
	lea prt_state(pc), a0
	tst.b (PRT_LOCK,a0)
	bne.s .vbl_done
	tst.l (PRT_WIDX,a0)
	beq.s .vbl_done
	move.l _frclock_var.w, d0
	sub.l (PRT_LAST,a0), d0
	cmp.l #PRT_IDLE_FRAMES, d0
	blo.s .vbl_done
	st (PRT_LOCK,a0)
	bsr.s send_block
	sf (PRT_LOCK,a0)
.vbl_done:
	rts

; cache_off, cache_back: a Mega STE's cache off around a send, as
; megaste_cache_off and megaste_cache_back do, with the machine from the
; state (a5) instead of the shared variable, which is in the cartridge.
cache_off	macro
	cmp.l #COOKIE_JAR_MEGASTE, (PRT_MACHINE,a5)
	bne.s .\@done
	move.b MEGASTE_SPEED_CACHE_REG.w, (PRT_CACHE,a5)
	bclr #0, MEGASTE_SPEED_CACHE_REG.w
.\@done:
	endm

cache_back	macro
	cmp.l #COOKIE_JAR_MEGASTE, (PRT_MACHINE,a5)
	bne.s .\@done
	move.b (PRT_CACHE,a5), MEGASTE_SPEED_CACHE_REG.w
.\@done:
	endm

; send_block: the buffer (a0 the state) to the RP, numbered, retried with the
; same number; emptied either way (what the RP never took is counted as
; dropped). Keeps every register but d0.
send_block:
	movem.l d1-d7/a0-a6, -(sp)
	move.l a0, a5
	cache_off
	addq.l #1, (PRT_SEQ,a5)
	move.w #CMD_RETRIES_COUNT, -(sp)	; the sender changes d1-d6 and a0-a3
.send_try:
	move.l (PRT_SEQ,a5), d3			; the number, the same on a retry
	move.l (PRT_WIDX,a5), d4			; the length
	moveq #0, d5
	move.l d4, d6
	move.l (PRT_BUFFER,a5), a4
	move.w #PRT_CMD_BLOCK, d0
	bsr send_sync_write_command_to_sidecart
	tst.w d0
	beq.s .send_ok
	subq.w #1, (sp)
	bpl.s .send_try
	addq.l #1, (PRT_FAILED,a5)
	move.l (PRT_WIDX,a5), d0
	add.l d0, (PRT_DROPPED,a5)
	bra.s .send_done
.send_ok:
	addq.l #1, (PRT_SENT,a5)
.send_done:
	addq.l #2, sp
	clr.l (PRT_WIDX,a5)
	cache_back
	movem.l (sp)+, d1-d7/a0-a6
	rts

	include "inc/sidecart_functions.s"

	even
resident_end:

	ifne (prt_conout-prt_state)-PRT_CONOUT_ENTRY
	fail "prt_conout must be at PRT_CONOUT_ENTRY: the harness finds the state from xconout[0]"
	endc

; ---------------------------------------------------------------------------
; The installer, from the cartridge.
; ---------------------------------------------------------------------------

; hook VECTOR, NAME: the vector (a system variable) to the routine NAME in the
; copy at a3, after its old value into the routine's XBRA header.
hook		macro
	move.l \1.w, (\2_old-resident_start,a3)
	lea (\2-resident_start,a3), a1
	move.l a1, \1.w							; the vector, last
	endm

install:
	move.l #(RESIDENT_BYTES + PRT_BUF_SIZE), -(sp)	; the hook, then its buffer
	move.w #GEMDOS_Malloc, -(sp)
	trap #1
	addq.l #6, sp
	tst.l d0
	ble .no_memory
	move.l d0, a3						; the copy
	lea resident_start(pc), a0
	move.l a3, a1
	move.w #(RESIDENT_BYTES/2)-1, d0
.copy:
	move.w (a0)+, (a1)+
	dbf d0, .copy
	lea (RESIDENT_BYTES,a3), a0			; the buffer
	move.l #PRT_MAGIC, (a3)
	move.l a3, (PRT_SELF,a3)
	move.l a0, (PRT_BUFFER,a3)
	move.l HARDWARE_TYPE_ADDR, (PRT_MACHINE,a3)
	hook xconout0, prt_conout
	hook xcostat0, prt_costat
	hook prv_lst, prt_lst
	hook prv_lsto, prt_lsto
	bsr install_vbl
	move.l a3, d3
	move.l d0, d4
	bsr report_install
	lea installed_msg(pc), a0
	bra.s .say
.no_memory:
	moveq #0, d3
	moveq #-1, d4
	bsr report_install
	lea no_memory_msg(pc), a0
.say:
	move.l a0, -(sp)
	move.w #GEMDOS_Cconws, -(sp)
	trap #1
	addq.l #6, sp
	rts

; report_install: tells the RP what the installer did (PRT_CMD_INSTALLED),
; d3 the state (0: none) and d4 the VBL slot (-1: none). Not answered, the
; hook works as before.
report_install:
	moveq #0, d5
	subq.l #2, sp
	megaste_cache_off (sp)
	send_sync PRT_CMD_INSTALLED, 12
	megaste_cache_back (sp)
	addq.l #2, sp
	rts

; install_vbl: prt_vbl, in the copy at a3, in a free slot of the VBL queue,
; searched from the last (TOS does not call the first: measured on TOS 1.04,
; where a routine there was never run); d0 the slot, or -1 when none is free:
; no idle flush then (only full buffers go).
install_vbl:
	move.w nvbls_var.w, d0
	beq.s .vbl_none
	subq.w #1, d0
	move.l vblqueue_var.w, a1
	move.w d0, d1
	lsl.w #2, d1
	add.w d1, a1						; the last slot
.vbl_slot:
	tst.w d0
	beq.s .vbl_none						; slot 0: not used
	tst.l (a1)
	beq.s .vbl_free
	subq.l #4, a1
	subq.w #1, d0
	bra.s .vbl_slot
.vbl_none:
	moveq #-1, d0
	rts
.vbl_free:
	lea (prt_vbl-resident_start,a3), a2
	move.l a2, (a1)
	rts

installed_msg:
	dc.b	27,"E","MD printer emulator: hook installed",13,10,0
no_memory_msg:
	dc.b	27,"E","MD printer emulator: no memory, no printer",13,10,0
	even

; The NOP tail: this module is the last in the cartridge image, and the
; senders' wait loop must not be the image's last code (see main.s).
	even
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
userfw_end:
