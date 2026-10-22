
; DMA Pause Check, auto-start version of BASIC/DMACheck.prg
;   Starts as soon as the menu runs it: no key press, so it also works with CIA1 absent
;   (no keyboard scan or jiffy IRQ needed).  Runs under IOH_TR_BASIC, same as DMACheck.prg.
;   Each pass writes TR_BASCont_DmaTest, which holds the C64 in DMA for ~100mS, then
;   polls the status reg for ready.
;   ControlNoDMA=1 (DMAPauseAutoCtl.asm) builds the no-DMA control: same program and timing,
;   but each pass is a ~100mS CPU delay with no TR access, to see if corruption needs DMA at all.
;   MaskIRQ=1 (DMAPauseAutoSei.asm) masks IRQs from just before the trigger to after the gap
;   between passes, so no IRQ is pending at, or taken soon after, the DMA release.
;   ScanRAM=1 (DMAPauseAutoScan.asm) fills free RAM with a pattern before the first pass and
;   checks it after each one, listing what changed: where the stray writes land, and with what.

!ifndef ControlNoDMA { ControlNoDMA = 0 }
!ifndef MaskIRQ { MaskIRQ = 0 }
!ifndef ScanRAM { ScanRAM = 0 }

; ********************************   Symbols   ********************************
   !convtab pet   ;key in and text out conv to PetSCII throughout

   ;TR_BASIC registers/values (see IOH_TR_BASIC.c TR_BASregsMatching)
   TRBasContReg    = $deb4
   TRBasStatReg    = $deb6
   TRBasContDmaTest= $0a
   TRBasStatReady  = $55

   NumPasses       = 100

   ;VIC registers
   BorderColorReg  = $d020
   BackgndColorReg = $d021
   PokeBlack       = 0
   PokeDrkGrey     = 11

   ;Kernal routines:
   SendChar        = $ffd2

   ;BASIC routines:
   PrintString     = $ab1e
   PrintIntXA      = $bdcd ;print unsigned int, A=hi, X=lo

   ;chr$ symbols
   ChrReturn       = 13
   ChrClear        = 147
   ChrToLower      = 14
   ChrCRSRUp       = 145
   ChrCRSRDn       = 17
   ChrYellow       = 158
   ChrLtGreen      = 153
   ChrLtRed        = 150
   ChrLtGrey       = 155
   ChrSpace        = 32

!if ScanRAM {
   ;scanned: from the first page after this program to $9fff (BASIC ROM above), and $c000-$cfff
   ScanStartPg     = >(ProgramEnd + $ff)
   ScanPtr         = $fb   ;2 bytes, free zero page
   MaxListed       = 4     ;changed bytes listed per pass, the rest are only counted
}

   * = $0801    ;BasicStart

   !word BasicEnd                  ; BASIC pointer to next line
   !word 2026                      ; BASIC line number (setting to year)
   !byte $9e                       ; BASIC token code for "SYS" command
   !byte (SysAddress/1000)+$30     ; Calculate 1st digit of .run
   !byte ((SysAddress/100)%10)+$30 ; Calculate 2nd digit of .run
   !byte ((SysAddress/10)%10)+$30  ; Calculate 3rd digit of .run
   !byte SysAddress%10+$30         ; Calculate 4th digit of .run
   !byte $00                       ; BASIC end of line marker
BasicEnd
   !word $0000                     ; BASIC end of program

SysAddress
Init:
   ;screen setup:
   lda #PokeDrkGrey
   sta BorderColorReg
   lda #PokeBlack
   sta BackgndColorReg

   lda #<MsgTitle
   ldy #>MsgTitle
   jsr PrintString

   lda TRBasStatReg
   cmp #TRBasStatReady
   beq +
   lda #<MsgNotReady
   ldy #>MsgNotReady
   jmp PrintExit

+
!if ScanRAM {
   lda #ScanStartPg
   ldx #$a0
   jsr FillRegion
   lda #$c0
   ldx #$d0
   jsr FillRegion
   lda #0
   sta TotalHits
   sta TotalHits+1
}
   lda #0
   sta PassNum

NextPass:
   inc PassNum
   lda #<MsgPassNum
   ldy #>MsgPassNum
   jsr PrintString
   ldx PassNum
   lda #0
   jsr PrintIntXA
   lda #ChrReturn
   jsr SendChar
   lda #ChrCRSRUp     ;next pass overwrites this line
   jsr SendChar

!if ControlNoDMA {
   ;in place of the DMA hold: 78*~1285 cycles = ~101mS PAL, ~98mS NTSC
   ldy #78
   ldx #0
--
-  dex
   bne -
   dey
   bne --
} else {
!if MaskIRQ {
   sei                ;the IRQ CIA1 raises during the hold waits until the cli after the gap
}
   lda #TRBasContDmaTest
   sta TRBasContReg   ;DMA asserted from here, C64 held for ~100mS

   ;X = polls until ready, 1 = ready on the first read after release
   ;  255 polls is ~2.3mS: no ready by then means the CPU wasn't held (or TR stopped responding)
   ldx #0
-  inx
   beq PollTimeout
   lda TRBasStatReg
   cmp #TRBasStatReady
   bne -
}

   ;TR sets ready before it clears the pending action: a trigger right away could be wiped
   ;  out, leaving DMA asserted and the C64 hung.  ~20mS gap, timed by the CPU (no CIA)
   ldy #16
--
-  dex
   bne -
   dey
   bne --
!if MaskIRQ {
   cli                ;~20mS after the release: the pending IRQ is taken here
}
!if ScanRAM {
   jsr ScanAll
}

   lda PassNum
   cmp #NumPasses
   bne NextPass

!if ScanRAM {
   lda #ChrCRSRDn
   jsr SendChar
   lda #<MsgTotal
   ldy #>MsgTotal
   jsr PrintString
   ldx TotalHits
   lda TotalHits+1
   jsr PrintIntXA
   lda #ChrReturn
   jsr SendChar
}
   lda #<MsgPassed
   ldy #>MsgPassed
   jmp PrintExit

PollTimeout:
!if MaskIRQ {
   cli                ;back to BASIC with IRQs on, or the keyboard is dead
}
   lda #<MsgTimeout
   ldy #>MsgTimeout
PrintExit:
   jsr PrintString
   rts               ;back to BASIC READY.

!if ScanRAM {
;Pattern is address lo xor hi: a stray write of almost any constant value shows up somewhere

FillRegion:    ;A = first page, X = page after the last
   sta ScanPtr+1
   stx ScanEnd
   lda #0
   sta ScanPtr
-- ldy #0
-  tya
   eor ScanPtr+1
   sta (ScanPtr),y
   iny
   bne -
   inc ScanPtr+1
   lda ScanPtr+1
   cmp ScanEnd
   bne --
   rts

ScanRegion:    ;A = first page, X = page after the last
   sta ScanPtr+1
   stx ScanEnd
   lda #0
   sta ScanPtr
-- ldy #0
-  tya
   eor ScanPtr+1
   cmp (ScanPtr),y
   bne ScanHit
ScanNext:
   iny
   bne -
   inc ScanPtr+1
   lda ScanPtr+1
   cmp ScanEnd
   bne --
   rts

ScanHit:       ;A = expected value, Y = offset in the ScanPtr page
   inc HitCount
   bne +
   inc HitCount+1
+  ldx Listed
   cpx #MaxListed
   bcs +
   pha
   lda ScanPtr+1
   sta HitAddrHi,x
   tya
   sta HitAddrLo,x
   lda (ScanPtr),y
   sta HitVal,x
   inc Listed
   pla
+  sta (ScanPtr),y   ;repair, so the next pass reports only new writes
   jmp ScanNext

;Scans both regions, and reports any changes below this pass's line
ScanAll:
   lda #0
   sta HitCount
   sta HitCount+1
   sta Listed
   lda #ScanStartPg
   ldx #$a0
   jsr ScanRegion
   lda #$c0
   ldx #$d0
   jsr ScanRegion

   lda HitCount
   ora HitCount+1
   beq ++
   clc
   lda TotalHits
   adc HitCount
   sta TotalHits
   lda TotalHits+1
   adc HitCount+1
   sta TotalHits+1

   lda #ChrCRSRDn     ;keep the pass line, report on the next
   jsr SendChar
   lda #<MsgHits
   ldy #>MsgHits
   jsr PrintString
   ldx HitCount
   lda HitCount+1
   jsr PrintIntXA
   lda #ChrReturn
   jsr SendChar
   ldx #0
-  cpx Listed
   beq +
   lda #ChrSpace
   jsr SendChar
   lda HitAddrHi,x
   jsr PrintHexByte
   lda HitAddrLo,x
   jsr PrintHexByte
   lda #$3a           ;':'
   jsr SendChar
   lda HitVal,x
   jsr PrintHexByte
   inx
   bne -
+  lda #ChrReturn
   jsr SendChar
++ rts

PrintHexByte:  ;A as 2 hex digits, X and Y unchanged
   pha
   lsr
   lsr
   lsr
   lsr
   jsr PrintHexNibble
   pla
PrintHexNibble:
   and #$0f
   cmp #$0a
   bcc +
   adc #$06           ;carry set: 10-15 -> $11-$16, +$30 below = PETSCII a-f
+  adc #$30
   jmp SendChar

HitCount:
   !word 0
TotalHits:
   !word 0
Listed:
   !byte 0
ScanEnd:
   !byte 0
HitAddrHi:
   !fill MaxListed
HitAddrLo:
   !fill MaxListed
HitVal:
   !fill MaxListed
}

PassNum:
   !byte 0

MsgTitle:
   !tx ChrClear, ChrToLower, ChrYellow, ChrCRSRDn
!if ControlNoDMA {
   !tx "DMA Pause Check: NO-DMA CONTROL RUN", ChrReturn, ChrLtGrey, ChrCRSRDn
} else {
!if MaskIRQ {
   !tx "DMA Pause Check: IRQs MASKED AT RELEASE", ChrReturn, ChrLtGrey, ChrCRSRDn
} else {
!if ScanRAM {
   !tx "DMA Pause Check: RAM SCAN EACH PASS", ChrReturn, ChrLtGrey, ChrCRSRDn
} else {
   !tx "TeensyROM DMA Pause Check (auto start)", ChrReturn, ChrLtGrey, ChrCRSRDn
}
}
}
!if ScanRAM {
   !tx " Takes about 90 seconds to complete", ChrReturn
} else {
   !tx " Takes about 15 seconds to complete", ChrReturn
}
   !tx " Will display ", ChrLtGreen, "Check Passed", ChrLtGrey, " on success", ChrReturn
   !tx " Any other screen/msg indicates a fail", ChrReturn, ChrCRSRDn
   !tx "Testing 100 times", ChrReturn, ChrCRSRDn
   !tx 0

MsgPassNum:
   !tx " #", 0

MsgNotReady:
   !tx ChrLtRed, "Not ready", ChrReturn, 0

MsgTimeout:
   !tx ChrCRSRDn, ChrCRSRDn, ChrLtRed, "Check Failed! No ready after DMA", ChrReturn, 0

MsgPassed:
   !tx ChrCRSRDn, ChrCRSRDn, ChrLtGreen, "Check Passed", ChrReturn, 0

!if ScanRAM {
MsgHits:
   !tx ChrLtRed, "  RAM changed: ", ChrLtGrey, 0

MsgTotal:
   !tx ChrLtGrey, "Total RAM changed: ", 0
}

ProgramEnd:
