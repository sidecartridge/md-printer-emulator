; autotest.s: the bench's check that a program in a floppy's AUTO folder runs
; at boot. It prints one line through GEMDOS and waits for a key, so the
; harness sees a stable screen, then ends. Keys typed before it
; starts are dropped first. Assembled by tools/dev/st/build.sh.

	section	text
	pea	message(pc)
	move.w	#9,-(sp)		; Cconws
	trap	#1
	addq.l	#6,sp
.drain:	move.w	#11,-(sp)		; Cconis: a key already waiting?
	trap	#1			; (an ESC meant for a Mega STE's
	addq.l	#2,sp			; memory test, on another machine)
	tst.w	d0
	beq.s	.wait
	move.w	#7,-(sp)		; Crawcin: take it
	trap	#1
	addq.l	#2,sp
	bra.s	.drain
.wait:	move.w	#7,-(sp)		; Crawcin: wait for a key
	trap	#1
	addq.l	#2,sp
	clr.w	-(sp)			; Pterm0
	trap	#1

	section	data
message:
	dc.b	27,"E",13,10,"AUTOTEST: the AUTO folder runs",13,10
	dc.b	"(press a key)",13,10,0
	even
