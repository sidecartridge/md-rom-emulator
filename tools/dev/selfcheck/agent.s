; The remote reset agent, installed by the self-check cartridge.
;
; The cartridge port has no reset line, so the ST resets itself: this agent
; stays in the ST's RAM, survives every reset that keeps the RAM powered, and
; resets the ST when the probe writes AGENT_SIG1 and AGENT_SIG2 into the
; cartridge window at AGENT_SIG_ADDR (`swd.py st-reset`).
;
; ST, STE and Mega STE only: on a TT or a Falcon the RAM it takes is in use,
; and selfcheck.s does not install it there.
;
; Where it lives: a 512-byte block at the top of the ST RAM, taken out of
; TOS's hands by lowering phystop by 512 (TOS never clears or hands out RAM
; above phystop). It rides on two hooks every Atari TOS has:
;
; - resvector: TOS jumps there early in every reset, with no stack and the way
;   back in a6. The agent marks the RAM valid again, so a reset that
;   invalidated it (the v2.1.2 setup menu clears memvalid before it resets)
;   still boots warm and keeps phystop; and it writes a reset-resident page
;   just below where TOS will put the screen (phystop - 32 KB - 512).
; - The reset-resident page: 512 bytes on a 512-byte boundary below phystop,
;   starting with $12123456 and its own address, whose 256 words sum to $5678.
;   TOS calls page + 8 late in every boot (after the boot sector, before the
;   AUTO folder); the page jumps into the block, which arms resvector again
;   and puts the watcher in the VBL queue (from slot 1: GEM writes slot 0
;   over whatever is there).
;
; Why there: on a warm boot TOS clears the 32 KB from phystop - 32 KB up (the
; screen), so a page above the screen does not survive; the RAM below the
; screen does, until the page has run. A program may overwrite the page
; later: the next reset writes it again.
;
; The watcher compares the two longwords on every vertical blank. When they
; match, it waits until the probe has put the cartridge's own words back (or
; AGENT_WAIT reads), then jumps through the reset vector, as the setup menu
; does. A ROM that takes over the interrupts (a game) or tests the RAM
; (DiagROM) removes the agent until the self-check installs it again; so does
; a power cycle.

resvalid            equ $426
resvector           equ $42a
phystop             equ $42e
memvalid            equ $420
memval2             equ $43a
memval3             equ $51a
nvbls               equ $454
vblqueue            equ $456
RR_MAGIC            equ $12123456
RES_MAGIC           equ $31415926
AGENT_TAG           equ $41474e54       ; "AGNT": the block's first long
AGENT_BLOCK_BYTES   equ 512
AGENT_PAGE_BYTES    equ 512
SCREEN_BYTES        equ $8000           ; TOS puts the screen at phystop - 32 KB
AGENT_SIG_ADDR      equ $fa9f44         ; after the v2.1.2 menu's command word
AGENT_SIG1          equ $52535421       ; "RST!"
AGENT_SIG2          equ $4147454e       ; "AGEN"
AGENT_WAIT          equ $200000         ; reads before resetting anyway

; agent_install: put the block above phystop and run it once, so the watcher
; works from this boot on. When the agent is there already (a warm boot: the
; page below the screen is in place), only runs it. Reads nothing above
; phystop before that, since a machine whose RAM ends there would bus-error.
; Keeps d2-d7/a2-a6.
agent_install:
	move.l phystop.w,d0
	move.l d0,a1                    ; a1: phystop
	sub.l #SCREEN_BYTES + AGENT_PAGE_BYTES,d0
	move.l d0,a0                    ; a0: where the page is when installed
	cmp.l #RR_MAGIC,(a0)
	bne.s .copy
	cmpa.l 4(a0),a0
	bne.s .copy
	cmp.l #AGENT_TAG,(a1)           ; the block, just above phystop
	beq.s .run
.copy:
	lea -AGENT_BLOCK_BYTES(a1),a1   ; this boot's screen ends 768 bytes lower
	lea agent_block(pc),a0
	move.w #AGENT_BLOCK_BYTES / 4 - 1,d0
.copy_long:
	move.l (a0)+,(a1)+
	dbf d0,.copy_long
	lea -AGENT_BLOCK_BYTES(a1),a1
	move.l a1,phystop.w             ; out of TOS's reach from the next boot on
.run:
	jsr agent_boot - agent_block(a1)
	rts

; The block, copied as it is: everything in it is position-independent.
	even
agent_block:
	dc.l AGENT_TAG

; Arms resvector and puts the watcher in a free slot of the VBL queue, once.
; The page calls it late in every boot; agent_install calls it at the first.
agent_boot:
	movem.l d0/a0-a1,-(sp)
	lea agent_reset(pc),a0
	move.l a0,resvector.w
	move.l #RES_MAGIC,resvalid.w
	lea agent_vbl(pc),a1
	move.w nvbls.w,d0
	subq.w #1,d0
	move.l vblqueue.w,a0
.find:
	cmpa.l (a0)+,a1
	beq.s .out                      ; already in the queue
	dbf d0,.find
	move.w nvbls.w,d0
	subq.w #2,d0                    ; slot 0 is GEM's: the AES writes it
	move.l vblqueue.w,a0
	addq.l #4,a0
.free:
	tst.l (a0)+
	beq.s .put
	dbf d0,.free
	bra.s .out                      ; no free slot: no watcher this boot
.put:
	move.l a1,-(a0)
.out:
	movem.l (sp)+,d0/a0-a1
	rts

; resvector: TOS jumps here early in every reset, before it sizes the RAM,
; with no stack. Back through a6, which leads to TOS's resvalid test again:
; clear it first.
agent_reset:
	clr.l resvalid.w
	move.l #$752019f3,memvalid.w
	move.l #$237698aa,memval2.w
	move.l #$5555aaaa,memval3.w
	; The page TOS runs late in this boot: $12123456, its address, a jump to
	; agent_boot, zeros, and the word that makes the sum $5678.
	move.l phystop.w,d0
	sub.l #SCREEN_BYTES + AGENT_PAGE_BYTES,d0
	move.l d0,a0
	move.l #RR_MAGIC,(a0)+
	move.l d0,(a0)+
	move.w #$4ef9,(a0)+             ; jmp (xxx).l
	lea agent_boot(pc),a1
	move.l a1,(a0)+
	move.w #(AGENT_PAGE_BYTES - 16) / 2 - 1,d1
.zero:
	clr.w (a0)+
	dbf d1,.zero
	move.l d0,a1
	moveq #0,d2
	move.w #AGENT_PAGE_BYTES / 2 - 2,d1
.sum:
	add.w (a1)+,d2
	dbf d1,.sum
	move.w #$5678,d1
	sub.w d2,d1
	move.w d1,(a1)                  ; the last word
	jmp (a6)

; The watcher, in the VBL queue.
agent_vbl:
	cmp.l #AGENT_SIG1,AGENT_SIG_ADDR
	bne.s .idle
	cmp.l #AGENT_SIG2,AGENT_SIG_ADDR+4
	bne.s .idle
	move.w #$2700,sr
	move.l #AGENT_WAIT,d0
.wait:
	cmp.l #AGENT_SIG1,AGENT_SIG_ADDR
	bne.s .reset                    ; the probe put the cartridge back
	subq.l #1,d0
	bne.s .wait
.reset:
	move.l $4.w,a0
	jmp (a0)
.idle:
	rts

	dcb.b AGENT_BLOCK_BYTES - (* - agent_block),0
agent_block_end:
