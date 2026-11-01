; User firmware module: the printer hook
; (C) 2026 by Diego Parrilla
; License: GPL v3
;
; main.s hands control here (rom_function: jmp USERFW) once the RP signals
; CMD_START, at every boot; this module runs from the cartridge at $FA0800 and
; is linked at offset $0800, so everything in it is PC-relative.
;
; It installs the printer hook and returns to TOS. Every way a program prints
; through TOS ends in four system vectors (measured on TOS 1.04, 1.62 and
; 2.06): the BIOS printer device's output and status, xconout[0] and
; xcostat[0], which GEMDOS's Cprnout and Fwrite to PRN: also reach, and the
; screen dump's own, prv_lst and prv_lsto (Alt-Help, XBIOS Scrdmp). Each
; vector is pointed at an XBRA trampoline in the RAM block main.s reserved
; before GEMDOS started (inc/printer_layout.s); the trampoline loads the
; block's address into a0 and jumps to the hook below, in the cartridge.
; The trampolines are the only code in RAM: XBRA keeps the previous vector
; next to the entry point, which the cartridge cannot hold.
;
; The hook: status answers ready (-1); output takes the byte at 6(sp), as
; TOS's own routine does, into the block's buffer and answers -1. A full
; buffer goes to the RP at once as a block (PRT_CMD_BLOCK, numbered: a retry
; resends the number, and the RP drops a block it has just taken); a fifth
; trampoline, in the VBL queue, sends what is left once printing has paused
; for PRT_IDLE_FRAMES. A lock keeps the two from sending at once: the VBL
; routine leaves the buffer alone while the hook works. Until the RP turns
; the printer on (shared variable PRT_SVAR_ENABLED), the hook hands every
; call to the routine it replaced, so the ST prints as without the cartridge.
;
; Machines: ST, STE, Mega ST, Mega STE (68000). A 68030 would need its
; instruction cache cleared after the trampolines are written.

	include inc/sidecart_layout.s
	include inc/printer_layout.s
	include inc/sidecart_macros.s
	include inc/tos.s

	section text

GEMDOS_Cconws		equ 9

; install_trampoline INDEX, VECTOR, HOOK: trampoline INDEX of the block at a0
; takes over VECTOR (a system variable) and jumps to HOOK.
install_trampoline	macro
	move.l #$58425241, (PRT_TRAMP+(\1*PRT_TRAMP_SIZE),a0)		; 'XBRA'
	move.l #PRT_XBRA_ID, (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+4,a0)
	move.l \2.w, (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+PRT_TRAMP_OLD,a0)
	move.w #$41FA, (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+12,a0)		; lea d16(pc),a0
	move.w #-(PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+14), (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+14,a0)
	move.w #$4EF9, (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+16,a0)		; jmp abs.l
	lea \3(pc), a1
	move.l a1, (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+18,a0)
	lea (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+PRT_TRAMP_CODE,a0), a1
	move.l a1, \2.w							; the vector, last
	endm

userfw:
	move.l _membot_var.w, d0				; the block main.s reserved
	sub.l #PRT_RAM_BYTES, d0
	move.l d0, a0
	cmp.l #PRT_MAGIC, (a0)
	bne .no_block
	cmpa.l PRT_SELF(a0), a0
	bne .no_block
	install_trampoline PRT_T_CONOUT, xconout0, prt_conout
	install_trampoline PRT_T_COSTAT, xcostat0, prt_costat
	install_trampoline PRT_T_LST, prv_lst, prt_lst
	install_trampoline PRT_T_LSTO, prv_lsto, prt_lsto
	bsr install_vbl
	lea installed_msg(pc), a0
	bra.s .say
.no_block:
	lea no_block_msg(pc), a0
.say:
	move.l a0, -(sp)
	move.w #GEMDOS_Cconws, -(sp)
	trap #1
	addq.l #6, sp
	rts

; The hook. TOS calls these as its own printer routines: the byte at 6(sp),
; d0 the answer, d0-d2 and a0-a2 free. The trampoline has put the block's
; address in a0.

; take_byte: the byte into the buffer, the buffer to the RP when full;
; answers -1. Holds the lock meanwhile, so the VBL routine waits.
take_byte	macro
	st (PRT_LOCK,a0)
	move.l (PRT_WIDX,a0), d0
	lea (PRT_BUF,a0), a1
	move.b 7(sp), (a1,d0.l)
	addq.l #1, d0
	move.l d0, (PRT_WIDX,a0)
	addq.l #1, (PRT_TAKEN,a0)
	move.l _frclock_var.w, (PRT_LAST,a0)
	cmp.l #PRT_BUF_SIZE, d0
	blo.s .\@kept
	bsr send_block
.\@kept:
	sf (PRT_LOCK,a0)
	moveq #-1, d0
	rts
	endm

; chain INDEX: the call to the routine trampoline INDEX replaced.
chain		macro
	move.l (PRT_TRAMP+(\1*PRT_TRAMP_SIZE)+PRT_TRAMP_OLD,a0), a0
	jmp (a0)
	endm

prt_conout:
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	bne.s .off
	take_byte
.off:
	chain PRT_T_CONOUT

prt_lst:
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	bne.s .off
	take_byte
.off:
	chain PRT_T_LST

prt_costat:
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	bne.s .off
	moveq #-1, d0
	rts
.off:
	chain PRT_T_COSTAT

prt_lsto:
	cmp.l #PRT_ENABLED, PRT_SVAR_ENABLED_ADDR
	bne.s .off
	moveq #-1, d0
	rts
.off:
	chain PRT_T_LSTO

; prt_vbl: from the VBL queue, every frame. Sends what the buffer holds once
; printing has paused for PRT_IDLE_FRAMES, unless the hook holds the lock.
; The VBL handler has saved every register.
prt_vbl:
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

; send_block: the buffer (a0 the block) to the RP, numbered, retried with
; the same number; emptied either way (what the RP never took is counted as
; dropped). Keeps every register but d0.
send_block:
	movem.l d1-d7/a0-a6, -(sp)
	move.l a0, a5
	megaste_cache_off (PRT_CACHE,a5)
	addq.l #1, (PRT_SEQ,a5)
	move.w #CMD_RETRIES_COUNT, -(sp)
.send_try:
	move.l (PRT_SEQ,a5), d3			; the number, the same on a retry
	move.l (PRT_WIDX,a5), d4			; the length
	moveq #0, d5
	move.l d4, d6
	lea (PRT_BUF,a5), a4
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
	megaste_cache_back (PRT_CACHE,a5)
	movem.l (sp)+, d1-d7/a0-a6
	rts

; install_vbl: a trampoline (a0 the block) in a free slot of the VBL queue,
; searched from the last (TOS does not call the first: measured on TOS 1.04,
; where a routine there was never run); none free, no idle flush (only full
; buffers go).
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
	rts
.vbl_free:
	move.l #$58425241, (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE),a0)		; 'XBRA'
	move.l #PRT_XBRA_ID, (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+4,a0)
	clr.l (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+PRT_TRAMP_OLD,a0)
	move.w #$41FA, (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+12,a0)		; lea d16(pc),a0
	move.w #-(PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+14), (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+14,a0)
	move.w #$4EF9, (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+16,a0)		; jmp abs.l
	lea prt_vbl(pc), a2
	move.l a2, (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+18,a0)
	lea (PRT_TRAMP+(PRT_T_VBL*PRT_TRAMP_SIZE)+PRT_TRAMP_CODE,a0), a2
	move.l a2, (a1)							; the slot, last
	rts

installed_msg:
	dc.b	27,"E","MD printer emulator: hook installed",13,10,0
no_block_msg:
	dc.b	27,"E","MD printer emulator: no RAM, no printer",13,10,0
	even

	include "inc/sidecart_functions.s"

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
