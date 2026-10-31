; prpaths.s: one line to the printer through each of TOS's print paths, a key
; between paths, so a run can tell from the printer's bytes and the traces
; which path went through which vector:
;   1 BIOS Bconout(0, c)        4 GEMDOS Fopen("PRN:") and Fwrite
;   2 GEMDOS Cprnout(c)         5 XBIOS Scrdmp (Alt-Help's screen dump)
;   3 GEMDOS Fwrite(3, ...)       (handle 3 is the standard printer)
; Each line starts with its number, so the printer's stream shows the order.
; Before each path the program prints "PATH n" on the screen and waits for a
; key; it ends with "PRPATHS DONE". Assembled by tools/dev/st/build.sh.

	section	text
start:	bsr	drain
	lea	title(pc),a0
	bsr	screen

; 1: BIOS Bconout(0, c), byte by byte
	moveq	#'1',d0
	bsr	announce
	lea	line1(pc),a3
.b1:	move.b	(a3)+,d0
	beq.s	.p2
	move.w	d0,-(sp)
	move.w	#0,-(sp)		; device 0: the printer
	move.w	#3,-(sp)		; Bconout
	trap	#13
	addq.l	#6,sp
	bra.s	.b1

; 2: GEMDOS Cprnout(c), byte by byte
.p2:	moveq	#'2',d0
	bsr	announce
	lea	line2(pc),a3
.b2:	move.b	(a3)+,d0
	beq.s	.p3
	move.w	d0,-(sp)
	move.w	#5,-(sp)		; Cprnout
	trap	#1
	addq.l	#4,sp
	bra.s	.b2

; 3: GEMDOS Fwrite to handle 3, the standard printer
.p3:	moveq	#'3',d0
	bsr	announce
	pea	line3(pc)
	move.l	#line3_end-line3,-(sp)
	move.w	#3,-(sp)		; handle 3
	move.w	#64,-(sp)		; Fwrite
	trap	#1
	lea	12(sp),sp

; 4: GEMDOS Fopen("PRN:"), Fwrite, Fclose
	moveq	#'4',d0
	bsr	announce
	move.w	#1,-(sp)		; write
	pea	prn(pc)
	move.w	#61,-(sp)		; Fopen
	trap	#1
	addq.l	#8,sp
	move.w	d0,d6			; the handle
	pea	line4(pc)
	move.l	#line4_end-line4,-(sp)
	move.w	d6,-(sp)
	move.w	#64,-(sp)		; Fwrite
	trap	#1
	lea	12(sp),sp
	move.w	d6,-(sp)
	move.w	#62,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp

; 5: XBIOS Scrdmp, the screen dump Alt-Help makes
	moveq	#'5',d0
	bsr	announce
	move.w	#20,-(sp)		; Scrdmp
	trap	#14
	addq.l	#2,sp

	lea	done(pc),a0
	bsr	screen
	bsr	waitkey
	clr.w	-(sp)			; Pterm0
	trap	#1

; announce: "PATH n" on the screen (n in d0), then wait for a key
announce:
	lea	pathno(pc),a0
	move.b	d0,(a0)
	lea	pathmsg(pc),a0
	bsr	screen
	bra.s	waitkey

; screen: print the string at a0 through GEMDOS
screen:	move.l	a0,-(sp)
	move.w	#9,-(sp)		; Cconws
	trap	#1
	addq.l	#6,sp
	rts

; waitkey: wait for a key (Crawcin)
waitkey:
	move.w	#7,-(sp)
	trap	#1
	addq.l	#2,sp
	rts

; drain: drop keys typed before the program started
drain:	move.w	#11,-(sp)		; Cconis
	trap	#1
	addq.l	#2,sp
	tst.w	d0
	beq.s	.done
	bsr.s	waitkey
	bra.s	drain
.done:	rts

	section	data
title:	dc.b	27,"E","PRPATHS: a key starts each path",13,10,0
pathmsg:
	dc.b	"PATH "
pathno:	dc.b	"0",13,10,0
done:	dc.b	"PRPATHS DONE",13,10,0
prn:	dc.b	"PRN:",0
line1:	dc.b	"1 BIOS BCONOUT(0)",13,10,0
line2:	dc.b	"2 GEMDOS CPRNOUT",13,10,0
line3:	dc.b	"3 GEMDOS FWRITE HANDLE 3",13,10
line3_end:
line4:	dc.b	"4 GEMDOS FOPEN PRN: FWRITE",13,10
line4_end:
	even
