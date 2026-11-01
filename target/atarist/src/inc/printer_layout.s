; The printer hook, resident: its layout, its switch and its commands, for
; userfw.s, which installs it at every boot. The switch and the commands must
; match rp/src/include/printer.h.
;
; The hook is resident in RAM the installer takes with Malloc (userfw.s), so
; it runs from RAM: an RP that restarts stops serving the cartridge, and the
; ST must have no code to run there. The copy starts with the state, the
; routines follow, each after its XBRA header, then the senders, then the
; buffer. State offsets, from the start of the copy:
;
;   +0     PRT_MAGIC
;   +4     the state's own address
;   +8     PRT_TAKEN    bytes taken from programs (long)
;   +12    PRT_DROPPED  bytes lost: the RP never answered their block (long)
;   +16    PRT_WIDX     bytes in the buffer, not sent yet (long)
;   +20    PRT_SEQ      the last block's sequence number (long)
;   +24    PRT_LOCK     set while the hook or the VBL routine works (byte)
;   +25    PRT_CACHE    a Mega STE's cache setting during a send (byte)
;   +28    PRT_LAST     _frclock when the last byte came (long)
;   +32    PRT_SENT     blocks the RP took (long)
;   +36    PRT_FAILED   blocks the RP never answered, after the retries (long)
;   +40    PRT_BUFFER   the buffer's address (long)
;   +44    PRT_MACHINE  the machine (the _MCH cookie, as main.s published it),
;                       kept here: the hook must not read the cartridge for it
;   +64    the first routine's XBRA header, then the routine
;          (PRT_CONOUT_ENTRY), which xconout[0] points at

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
PRT_BUFFER          equ 40
PRT_MACHINE         equ 44
PRT_STATE_BYTES     equ 64
PRT_CONOUT_ENTRY    equ 76              ; PRT_STATE_BYTES, then 'XBRA', the id, the previous vector
PRT_XBRA_OLD        equ -4              ; from an entry: the previous vector
PRT_BUF_SIZE        equ 1024
PRT_IDLE_FRAMES     equ 25              ; half a second at 50 Hz: then the rest goes

; System variables the hook uses.
xconout0            equ $57e            ; xconout[0], the printer (BIOS device 0)
xcostat0            equ $55e            ; xcostat[0]
prv_lst             equ $50a            ; the screen dump's output routine
prv_lsto            equ $506            ; the screen dump's status routine
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
; The command the installer reports with, once per boot: d3 the resident
; state's address (0: not installed), d4 the VBL queue slot the idle flush took
; (-1: none), d5 0.
PRT_CMD_INSTALLED   equ $0802
