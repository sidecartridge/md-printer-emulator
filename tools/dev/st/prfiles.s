; prfiles.s: sends the printer files TEXT.PRN then GRAPH.PRN, in the current
; folder, to the printer, byte by byte through BIOS Bconout(0), as a program
; that prints through TOS does (tools/dev/st/make_escp_pages.py makes them).
; Runs from a floppy's AUTO folder (the current folder is then the floppy's
; root) or from the desktop (its own folder); waits for a key before
; printing, and prints "PRFILES DONE" and waits for a key at the end, so the
; harness sees stable screens. Assembled by tools/dev/st/build.sh.

BUFSIZE	equ	16384

	section	text
start:	pea	ready(pc)
	move.w	#9,-(sp)		; Cconws
	trap	#1
	addq.l	#6,sp
	bsr	waitkey
	lea	textname(pc),a0
	bsr.s	sendfile
	lea	graphname(pc),a0
	bsr.s	sendfile
	pea	done(pc)
	move.w	#9,-(sp)		; Cconws
	trap	#1
	addq.l	#6,sp
	bsr	waitkey
	clr.w	-(sp)			; Pterm0
	trap	#1

; waitkey: drop the keys typed before, then wait for one (Crawcin)
waitkey:
.drain:	move.w	#11,-(sp)		; Cconis
	trap	#1
	addq.l	#2,sp
	tst.w	d0
	beq.s	.wait
	move.w	#7,-(sp)
	trap	#1
	addq.l	#2,sp
	bra.s	.drain
.wait:	move.w	#7,-(sp)		; Crawcin
	trap	#1
	addq.l	#2,sp
	rts

; sendfile: the file named at a0, a buffer at a time, to Bconout(0)
sendfile:
	clr.w	-(sp)			; read-only
	move.l	a0,-(sp)
	move.w	#61,-(sp)		; Fopen
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bmi.s	.out
	move.w	d0,d6			; the handle
.read:	pea	buffer
	move.l	#BUFSIZE,-(sp)
	move.w	d6,-(sp)
	move.w	#63,-(sp)		; Fread
	trap	#1
	lea	12(sp),sp
	tst.l	d0
	ble.s	.close
	move.l	d0,d5			; bytes read
	lea	buffer,a3
.byte:	moveq	#0,d0
	move.b	(a3)+,d0
	move.w	d0,-(sp)
	clr.w	-(sp)			; device 0: the printer
	move.w	#3,-(sp)		; Bconout
	trap	#13
	addq.l	#6,sp
	subq.l	#1,d5
	bne.s	.byte
	bra.s	.read
.close:	move.w	d6,-(sp)
	move.w	#62,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp
.out:	rts

	section	data
textname:	dc.b	"TEXT.PRN",0
graphname:	dc.b	"GRAPH.PRN",0
ready:	dc.b	27,"E","PRFILES: a key prints TEXT.PRN, GRAPH.PRN",13,10,0
done:	dc.b	13,10,"PRFILES DONE",13,10,0
	even

	section	bss
buffer:	ds.b	BUFSIZE
