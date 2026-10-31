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
; TOS's own routine does, into the block's buffer (counting what the buffer
; has no room for) and answers -1. Until the RP turns the printer on (shared
; variable PRT_SVAR_ENABLED), both hand every call to the routine they
; replaced, so the ST prints as without the cartridge.
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

; take_byte: the byte into the buffer, or counted as dropped; answers -1.
take_byte	macro
	move.l (PRT_WIDX,a0), d0
	cmp.l #PRT_BUF_SIZE, d0
	bhs.s .\@full
	lea (PRT_BUF,a0), a1
	move.b 7(sp), (a1,d0.l)
	addq.l #1, (PRT_WIDX,a0)
	addq.l #1, (PRT_TAKEN,a0)
	moveq #-1, d0
	rts
.\@full:
	addq.l #1, (PRT_DROPPED,a0)
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
