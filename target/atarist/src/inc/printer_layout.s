; The printer hook's RAM block, its switch and its command, shared by main.s
; (which reserves the block before GEMDOS starts) and userfw.s (which installs
; the hook in it at every boot). The switch and the command must match
; rp/src/include/printer.h.
;
; The block sits just below _membot: the bit-26 cartridge init raises _membot
; by PRT_RAM_BYTES before GEMDOS builds its free memory, so GEMDOS never hands
; it out, and stamps it (PRT_MAGIC, then its own address). Layout:
;
;   +0     PRT_MAGIC
;   +4     the block's own address
;   +8     PRT_TAKEN    bytes taken from programs (long)
;   +12    PRT_DROPPED  bytes lost: the RP never answered their block (long)
;   +16    PRT_WIDX     bytes in the buffer, not sent yet (long)
;   +20    PRT_SEQ      the last block's sequence number (long)
;   +24    PRT_LOCK     set while the hook or the VBL routine works (byte)
;   +25    PRT_CACHE    a Mega STE's cache setting during a send (byte)
;   +28    PRT_LAST     _frclock when the last byte came (long)
;   +32    PRT_SENT     blocks the RP took (long)
;   +36    PRT_FAILED   blocks the RP never answered, after the retries (long)
;   +64    five trampolines, PRT_TRAMP_SIZE bytes each: 'XBRA', the id, the
;          previous vector, then "lea block(pc),a0; jmp routine" (in the
;          cartridge). The vectors point at the lea.
;   +256   the buffer, PRT_BUF_SIZE bytes: a block, sent when full and when
;          printing has paused for PRT_IDLE_FRAMES

PRT_RAM_BYTES       equ 2048
PRT_MAGIC           equ $4D445052       ; 'MDPR'
PRT_XBRA_ID         equ $4D445052       ; 'MDPR'
PRT_SELF            equ 4
PRT_TAKEN           equ 8
PRT_DROPPED         equ 12
PRT_WIDX            equ 16
PRT_SEQ             equ 20
PRT_LOCK            equ 24
PRT_CACHE           equ 25
PRT_LAST            equ 28
PRT_SENT            equ 32
PRT_FAILED          equ 36
PRT_TRAMP           equ 64
PRT_TRAMP_SIZE      equ 24
PRT_TRAMP_OLD       equ 8               ; in a trampoline: the previous vector
PRT_TRAMP_CODE      equ 12              ; in a trampoline: where the vector points
PRT_BUF             equ 256
PRT_BUF_SIZE        equ 1024
PRT_IDLE_FRAMES     equ 25              ; half a second at 50 Hz: then the rest goes

; The trampolines, in this order.
PRT_T_CONOUT        equ 0               ; xconout[0]: the printer's output
PRT_T_COSTAT        equ 1               ; xcostat[0]: the printer's status
PRT_T_LST           equ 2               ; prv_lst: the screen dump's output
PRT_T_LSTO          equ 3               ; prv_lsto: the screen dump's status
PRT_T_VBL           equ 4               ; a slot of the VBL queue: the idle flush

; System variables the hook uses.
xconout0            equ $57e            ; xconout[0], the printer (BIOS device 0)
xcostat0            equ $55e            ; xcostat[0]
prv_lst             equ $50a            ; the screen dump's output routine
prv_lsto            equ $506            ; the screen dump's status routine
_membot_var         equ $432
_memtop_var         equ $436
nvbls_var           equ $454            ; the VBL queue's length (word)
vblqueue_var        equ $456            ; the VBL queue (pointer)
_frclock_var        equ $466            ; frames since boot (long)

; The switch: the RP writes PRT_ENABLED into shared variable PRT_SVAR_ENABLED
; when the printer is on; anything else and the hook hands every call to the
; routine it replaced, so the ST prints as without the cartridge.
PRT_SVAR_ENABLED    equ 2
PRT_ENABLED         equ $50524F4E       ; 'PRON'
PRT_SVAR_ENABLED_ADDR equ (SHARED_VARIABLES + (PRT_SVAR_ENABLED * 4))

; The command a block goes in: d3 its sequence number, d4 its length, d5 0,
; then the bytes.
PRT_CMD_BLOCK       equ $0801
