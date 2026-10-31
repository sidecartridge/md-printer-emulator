; The printer hook's RAM block and its switch, shared by main.s (which
; reserves the block before GEMDOS starts) and userfw.s (which installs the
; hook in it at every boot). The switch must match rp/src/include/printer.h.
;
; The block sits just below _membot: the bit-26 cartridge init raises _membot
; by PRT_RAM_BYTES before GEMDOS builds its free memory, so GEMDOS never hands
; it out, and stamps it (PRT_MAGIC, then its own address). Layout:
;
;   +0     PRT_MAGIC
;   +4     the block's own address
;   +8     PRT_TAKEN    bytes taken from programs (long)
;   +12    PRT_DROPPED  bytes dropped, the buffer full (long)
;   +16    PRT_WIDX     the buffer's write index (long)
;   +32    four XBRA trampolines, PRT_TRAMP_SIZE bytes each: 'XBRA', the id,
;          the previous vector, then "lea block(pc),a0; jmp hook" (the hook in
;          the cartridge). The vectors point at the lea.
;   +256   the buffer, PRT_BUF_SIZE bytes

PRT_RAM_BYTES       equ 2048
PRT_MAGIC           equ $4D445052       ; 'MDPR'
PRT_XBRA_ID         equ $4D445052       ; 'MDPR'
PRT_SELF            equ 4
PRT_TAKEN           equ 8
PRT_DROPPED         equ 12
PRT_WIDX            equ 16
PRT_TRAMP           equ 32
PRT_TRAMP_SIZE      equ 24
PRT_TRAMP_OLD       equ 8               ; in a trampoline: the previous vector
PRT_TRAMP_CODE      equ 12              ; in a trampoline: where the vector points
PRT_BUF             equ 256
PRT_BUF_SIZE        equ 1024

; The trampolines, in this order.
PRT_T_CONOUT        equ 0               ; xconout[0]: the printer's output
PRT_T_COSTAT        equ 1               ; xcostat[0]: the printer's status
PRT_T_LST           equ 2               ; prv_lst: the screen dump's output
PRT_T_LSTO          equ 3               ; prv_lsto: the screen dump's status

; System variables the hook takes over.
xconout0            equ $57e            ; xconout[0], the printer (BIOS device 0)
xcostat0            equ $55e            ; xcostat[0]
prv_lst             equ $50a            ; the screen dump's output routine
prv_lsto            equ $506            ; the screen dump's status routine
_membot_var         equ $432
_memtop_var         equ $436

; The switch: the RP writes PRT_ENABLED into shared variable PRT_SVAR_ENABLED
; when the printer is on; anything else and the hook hands every call to the
; routine it replaced, so the ST prints as without the cartridge.
PRT_SVAR_ENABLED    equ 2
PRT_ENABLED         equ $50524F4E       ; 'PRON'
PRT_SVAR_ENABLED_ADDR equ (SHARED_VARIABLES + (PRT_SVAR_ENABLED * 4))
