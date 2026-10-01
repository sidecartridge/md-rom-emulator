; Self-check cartridge for the ROM Emulator's tests.
;
; Every word from PATTERN_FROM to the end of the 128 KB window holds a known
; pattern: the word the ST reads at $FA0000 + 2*i is i ^ $A55A (the same
; pattern as tools/dev/make_rom_images.py, which pads this code with it). At
; boot, after GEMDOS is up and before the disks, this reads every one of those
; words through the cartridge port, prints the verdict on the console and
; sends it as a command frame through ROM3 reads, so a ROM3 capture on the RP
; can read it without anyone looking at the screen:
;
;   $7F01 pass, $7F02 fail; payload: a zero token, then the address of the
;   first word that did not match (0 on a pass).
;
; The frame has the shape the cartridge's senders use: $ABCD, command, payload
; size, payload words, checksum (the 16-bit sum of command, size and payload),
; each word sent by reading $FB8000 + the word. Nothing answers it in ROM
; mode, so it is sent FRAME_REPEATS times and never waits. The verdict then
; stays on screen for VERDICT_FRAMES frames before TOS boots on.

ROM4_ADDR           equ $FA0000
WINDOW_BYTES        equ $20000          ; ROM4 and ROM3, 128 KB
PATTERN_FROM        equ $1000           ; this code must end before it
PATTERN_XOR         equ $A55A
ROMCMD_MIDDLE       equ $FB8000         ; ROM3 + 32 KB: indexed by a signed word
CMD_MAGIC_NUMBER    equ $ABCD
CMD_SELFCHECK_PASS  equ $7F01
CMD_SELFCHECK_FAIL  equ $7F02
FRAME_REPEATS       equ 3
VERDICT_FRAMES      equ 250             ; the verdict stays up 5 s at 50 Hz
_vbclock            equ $462            ; TOS counts vertical blanks here

	section

	org ROM4_ADDR

	dc.l $abcdef42                  ; cartridge magic
	dc.l 0                          ; no next program
	dc.l $08000000 + selfcheck      ; after GEMDOS init, before the disks
	dc.l 0                          ; no program to run from the desktop
	dc.w 0                          ; time
	dc.w 0                          ; date
	dc.l selfcheck_end - selfcheck
	dc.b "SELFCHCK",0
	even

selfcheck:
	movem.l d0-d7/a0-a6,-(sp)

	lea msg_title(pc),a0
	bsr print

	; d3: the word index, d4: words left (for dbf), a3: the address read.
	; TOS calls clobber d0-d2 and a0-a2, so the loop state lives above them.
	lea ROM4_ADDR + PATTERN_FROM,a3
	move.w #PATTERN_FROM / 2,d3
	move.w #(WINDOW_BYTES - PATTERN_FROM) / 2 - 1,d4
.check:
	move.w d3,d5
	eor.w #PATTERN_XOR,d5           ; d5: the word that should be there
	move.w (a3)+,d6                 ; d6: the word the cartridge port returns
	cmp.w d5,d6
	bne.s .fail
	addq.w #1,d3
	dbf d4,.check

	lea msg_pass(pc),a0
	bsr print
	moveq #0,d3                     ; no failing address
	move.w #CMD_SELFCHECK_PASS,d4
	bra.s .report

.fail:
	subq.l #2,a3                    ; back to the word that did not match
	lea msg_fail(pc),a0
	bsr print
	move.l a3,d0
	moveq #6,d1                     ; $FBxxxx: six digits
	bsr print_hex
	lea msg_read(pc),a0
	bsr print
	move.w d6,d0
	moveq #4,d1
	bsr print_hex
	lea msg_expected(pc),a0
	bsr print
	move.w d5,d0
	moveq #4,d1
	bsr print_hex
	lea msg_newline(pc),a0
	bsr print
	move.l a3,d3                    ; the failing address
	move.w #CMD_SELFCHECK_FAIL,d4

.report:
	moveq #FRAME_REPEATS - 1,d5
.repeat:
	bsr send_frame
	dbf d5,.repeat

	; Leave the verdict on screen before TOS goes on booting over it.
	move.l _vbclock,d0
	add.l #VERDICT_FRAMES,d0
.hold:
	cmp.l _vbclock,d0
	bhi.s .hold

	movem.l (sp)+,d0-d7/a0-a6
	rts

; send_frame: d4.w the command, d3.l the address. Keeps d3-d6.
send_frame:
	lea ROMCMD_MIDDLE,a5
	move.w #CMD_MAGIC_NUMBER,d7
	tst.b (a5,d7.w)                 ; header
	clr.l d7
	add.w d4,d7
	tst.b (a5,d4.w)                 ; command
	moveq #8,d0                     ; payload: token (4 bytes), address (4)
	add.w d0,d7
	tst.b (a5,d0.w)                 ; payload size
	moveq #0,d0
	tst.b (a5,d0.w)                 ; token, low word
	tst.b (a5,d0.w)                 ; token, high word
	add.w d3,d7
	tst.b (a5,d3.w)                 ; address, low word
	swap d3
	add.w d3,d7
	tst.b (a5,d3.w)                 ; address, high word
	swap d3
	tst.b (a5,d7.w)                 ; checksum
	rts

; print: the zero-terminated string at a0, through GEMDOS Cconws.
print:
	move.l a0,-(sp)
	move.w #9,-(sp)
	trap #1
	addq.l #6,sp
	rts

; print_hex: the low d1.w hex digits (1 to 8) of d0.l, after a '$'. Uses d4,
; d7 and a4, which the TOS calls leave alone; like every TOS call it also
; clobbers d0-d2 and a0-a2.
print_hex:
	move.l d0,d7
	move.w d1,d4
	move.w #'$',d0
	bsr.s putc
	move.w d4,d0
	lsl.w #2,d0
	neg.w d0
	add.w #32,d0
	rol.l d0,d7                     ; the first digit to print at the top
	subq.w #1,d4
.digit:
	rol.l #4,d7
	move.w d7,d0
	and.w #$F,d0
	lea hex_digits(pc),a4
	move.b (a4,d0.w),d0
	bsr.s putc
	dbf d4,.digit
	rts

; putc: the character in d0.w, through GEMDOS Cconout.
putc:
	move.w d0,-(sp)
	move.w #2,-(sp)
	trap #1
	addq.l #4,sp
	rts

hex_digits:     dc.b "0123456789ABCDEF"
msg_title:      dc.b "ROM Emulator self-check: ",0
msg_pass:       dc.b "PASS",13,10,0
msg_fail:       dc.b "FAIL at ",0
msg_read:       dc.b ", read ",0
msg_expected:   dc.b ", expected ",0
msg_newline:    dc.b 13,10,0
	even

selfcheck_end:
