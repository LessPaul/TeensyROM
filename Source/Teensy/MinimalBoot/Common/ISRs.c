// MIT License
// 
// Copyright (c) 2023 Travis Smith
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software 
// and associated documentation files (the "Software"), to deal in the Software without 
// restriction, including without limitation the rights to use, copy, modify, merge, publish, 
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom 
// the Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all copies or 
// substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING 
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, 
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#ifdef Fab04_FullDMACapable
   extern void DMATransferISR();
#endif

FASTRUN void isrButton()
{
   BtnPressed = true;
}

#ifndef MinimumBuild 
FASTRUN void isrExtResetDetect()
{
   //IO1[rwRegPwrUpDefaults3] doesn't exist in minimal build, would use EEPROM.read(eepAdPwrUpDefaults3) instead from there/both
   if ((IO1[rwRegPwrUpDefaults3] & rpud3ResetDetectDisable) == 0) BtnPressed = true; //return to main menu
   //else
   //{
      //This triggers a condition where the main menu is defeated due to self-trigger
      //would need to re-validate these after if (doReset) in Teensy.ino
   // //release/vanish, allow native reset
   //   SetDMADeassert;
   //   SetIRQDeassert;
   //   SetNMIDeassert;
   //   SetGameDeassert;
   //   SetExROMDeassert; 
   //   LOROM_Image = HIROM_Image = NULL;
   //   CurrentIOHandler = IOH_None;
   //}
}
#endif

#ifdef DbgDMAEdgeTiming
//Right after the /DMA pin change: RiseCyc and FellBeforeRealign are taken in isrPHI2 before the re-align
#define LogDMAEdge(Log) do {                         \
   uint32_t EdgeCyc = ARM_DWT_CYCCNT;                \
   bool Phi2High = GP6_Phi2(ReadGPIO6);              \
   Log.RiseToFallCyc = StartCycCnt - RiseCyc;        \
   Log.RiseToEdgeCyc = EdgeCyc - RiseCyc;            \
   Log.FellBeforeRealign = FellBeforeRealign;        \
   Log.Phi2HighAtEdge = Phi2High;                    \
} while(0)
#endif

void DMAWaitAssertReady()
{
   WaitUntil_nS(nS_DMAAssert);
   SetDMAAssert;
   DMA_State = DMA_S_TransferReady;
}

#if defined(DbgDMAHoldDriven) && defined(Fab04_FullDMACapable)
//Called from isrPHI2 right after SetDMAAssert, in the VIC phase after the trigger write (StartCycCnt = that
//   Phi2 fall).  Reads DMAHoldAddr in every cycle the bus is ours, starting with this one, then releases DMA
//   at the Phi2 fall after the last: no CPU phase 2 is left floating at either end, which is what corrupted
//   C64 DRAM rows with the bus left floating (DMAPauseAuto RAM scan).  Blocks the ISR for the whole hold, as
//   DirectREU does.  Cycles with BA low are skipped and counted: the ~3 per badline before the VIC takes
//   the bus still float.
FASTRUN void DMAHoldDriven()
{
   uint8_t Discard;
   DMA_RnW = DMA_READ;
   DMA_FixC64Addr = DMA_ADDR_FIXED;
   DMA_StartAddr = DMAHoldAddr;
   DMA_Count = 0;
   DMAHoldBASkipped = 0;

   for (uint32_t Cycle = 0; Cycle < DMAHoldCycles; Cycle++)
   {
      if (DMAByte(&Discard)) LastCycCnt = StartCycCnt; //DMAByte stamps StartCycCnt just ahead of phi2 rising
      else
      {  //BA low, still in this VIC phase
         DMAHoldBASkipped++;
         while(!GP6_Phi2(ReadGPIO6)); //Find phi2 rising
         LastCycCnt = ARM_DWT_CYCCNT;
      }
      while(GP6_Phi2(ReadGPIO6)); //Find phi2 falling
      StartCycCnt = ARM_DWT_CYCCNT;
   }

   SetDMADeassert;  //at the fall after the last driven phase 2, as DMA_S_StartDisable releases
   #ifdef DbgDMAEdgeTiming
      const uint32_t RiseCyc = LastCycCnt;
      const bool FellBeforeRealign = false;
      LogDMAEdge(DMAReleaseLog);
      DMAEdgeLogged = true;
   #endif
   DMA_State = DMA_S_DisableReady;
   DMAHoldNext = false;
   //Phi2 edges during the hold left the interrupt pending: clear it so the next isrPHI2
   //is the CPU's first cycle, not a stale entry in this VIC phase (see DirectREU)
   GPIO6_ISR = CORE_PIN1_BITMASK;  //PHI2_PIN, write-1-to-clear
   NVIC_CLEAR_PENDING(IRQ_GPIO6789);
}
#endif

//Phi2 rising edge:
FASTRUN void isrPHI2() 
{
   StartCycCnt = ARM_DWT_CYCCNT;
   uint32_t CycSinceLast = StartCycCnt-LastCycCnt;
   LastCycCnt = StartCycCnt;
   
   if (CycSinceLast > nSToCyc(nS_MaxAdj)) // If we're late, adjust...
   {
      StartCycCnt += nSToCyc(nS_MaxAdj) - CycSinceLast; 
      #ifdef DbgCycAdjLog      
         if (BigBuf != NULL)
         {
            BigBuf[BigBufCount] = CycSinceLast | AdjustedCycleTiming;
            if (BigBufCount < BigBufSize-1) BigBufCount++; //last entry is scratch, rewritten once full
         }
      #endif
   }
   
#ifdef DbgSignalIsrPHI2
   SetDebugAssert;
#endif

#ifdef Fab04_FullDMACapable
   if (DMA_State == DMA_S_TransferExecuting) 
   {
#if !defined(MinimumBuild) || defined(FeatVMHost)  //temporary exclusion of minimal mode DMAs; the extension image has them
      DMATransferISR();
      return;
#endif
   }
#endif

   WaitUntil_nS(nS_RWnReady); 
   uint32_t GPIO_6 = ReadGPIO6; //Address bus and  R/*W are (almost) valid on Phi2 rising, Read now
   uint16_t Address = GP6_Address(GPIO_6); //parse out address
   bool R_Wn = GP6_R_Wn(GPIO_6);  //parse read/write bit
   
#ifndef Fab04_DataBufAlwaysEnabled
   if (R_Wn) SetDataBufOut; //set data buffer direction (on pcb v0.3+)
   else SetDataBufIn;
#endif

   if (fBusSnoop != NULL) if (fBusSnoop(Address, R_Wn)) return;
   
#ifdef Fab04_GlobalKernalReplace
   if (fKernRepl != NULL) if (fKernRepl(Address, R_Wn)) return;
#endif
   
   WaitUntil_nS(nS_PLAprop);
   uint32_t GPIO_9 = ReadGPIO9; //Now read the derived signals

   if (!GP9_ROML(GPIO_9)) //ROML: 8000-9FFF address space, *usually* read only
   {
      if (LOROM_Image!=NULL) DataPortWriteWait(LOROM_Image[Address & LOROM_Mask]); 
      if (IOHandler[CurrentIOHandler]->ROMLHndlr != NULL) IOHandler[CurrentIOHandler]->ROMLHndlr(Address, R_Wn);
      //Printf_dbg("roml addr: %04x\n", Address); //useful for HW debug of address lines
   }  //ROML
   else if (!GP9_ROMH(GPIO_9)) //ROMH: A000-BFFF or E000-FFFF address space, *usually* read only
   {
      if (HIROM_Image!=NULL) DataPortWriteWait(HIROM_Image[Address & HIROM_Mask]); 
      if (IOHandler[CurrentIOHandler]->ROMHHndlr != NULL) IOHandler[CurrentIOHandler]->ROMHHndlr(Address, R_Wn);
   }  //ROMH
   else if (!GP9_IO1n(GPIO_9)) //IO1: DExx address space
   {
      Address &= 0xFF;
      #ifdef DbgIOTraceLog
         BigBuf[BigBufCount] = Address; //initialize w/ address
      #endif

      if (IOHandler[CurrentIOHandler]->IO1Hndlr != NULL) IOHandler[CurrentIOHandler]->IO1Hndlr(Address, R_Wn);

      #ifdef DbgIOTraceLog
         if (R_Wn) BigBuf[BigBufCount] |= IOTLRead;
         if (BigBufCount < BigBufSize-1) BigBufCount++; //last entry is scratch, rewritten once full
      #endif
   }  //IO1
   
   else if (!GP9_IO2n(GPIO_9))  //IO2: DFxx address space
   {
      Address &= 0xFF;

      if (IOHandler[CurrentIOHandler]->IO2Hndlr != NULL) IOHandler[CurrentIOHandler]->IO2Hndlr(Address, R_Wn);
   }
   
   if (IOHandler[CurrentIOHandler]->CycleHndlr != NULL) IOHandler[CurrentIOHandler]->CycleHndlr(R_Wn);

   
   if (EmulateVicCycles || DMA_State > DMA_S_BeginStartStates)
   {
      #ifdef DbgDMAEdgeTiming
         const uint32_t RiseCyc = StartCycCnt;
         const bool FellBeforeRealign = !GP6_Phi2(ReadGPIO6);
      #endif
      while(GP6_Phi2(ReadGPIO6)); //Re-align to phi2 falling, if needed
      //phi2 has gone low..........................................................................
      
      StartCycCnt = ARM_DWT_CYCCNT;
      
      // activate/disable DMA during low phase of Phi2
      if (DMA_State > DMA_S_BeginStartStates)
      {
         switch (DMA_State)
         {
            case DMA_S_StartDisable:
               //WaitUntil_nS(nS_DMAAssert);  //Have seen corruption with this in place
               SetDMADeassert;
               #ifdef DbgDMAEdgeTiming
                  LogDMAEdge(DMAReleaseLog);
                  DMAEdgeLogged = true;
               #endif
               DMA_State = DMA_S_DisableReady;
               return;
               
            case DMA_S_StartAsynch:  //starting state for safe asynchronous DMA
               //based on "Safely freezing the C64 on an asynchronous event"  ©2008, by Gideon Zweijtzer
               //   https://codebase64.com/lib/exe/fetch.php?media=base:safely_freezing_the_c64.pdf
               
               DMACycleCount = 0; //initialize for timeout
               if (R_Wn) //Read
               {
                  if(!GP9_BA(ReadGPIO9)) DMAWaitAssertReady(); // start during any bad line read
                  else DMA_State = DMA_S_StartAsynch_Wait_LRd ;
               }
               else DMA_State = DMA_S_StartAsynch_Wait_LWr;
               return;
            case DMA_S_StartAsynch_Wait_LWr: //Last cycle was a write
               if (R_Wn) DMAWaitAssertReady(); //Last was a write, this is a read, assert!
                  //else another write, no state change
                  //Check for timeout? Continuous writes is not a real-worls situation
               return;             
            case DMA_S_StartAsynch_Wait_LRd: //Last cycle was a read
               if (R_Wn) //Read
               {
                  if(!GP9_BA(ReadGPIO9)) DMAWaitAssertReady(); // start during any bad line read
                  else 
                  {
                     //Another Read, no state change
                     if (DMACycleCount++ > DMA_TIMEOUT_CYCLES) DMAWaitAssertReady(); // Timeout, assume read loop and assert
                  }
               }
               else DMA_State = DMA_S_StartAsynch_Wait_LWr;
               return;    

            case DMA_S_StartImmediate:
               WaitUntil_nS(nS_DMAAssert);
               SetDMAAssert;
               #ifdef DbgDMAEdgeTiming
                  LogDMAEdge(DMAAssertLog);
               #endif
               #if defined(DbgDMAHoldDriven) && defined(Fab04_FullDMACapable)
                  if (DMAHoldNext)
                  {
                     DMAHoldDriven(); //holds, then releases
                     return;
                  }
               #endif
               DMA_State = DMA_S_ActiveReady;
               return;
         }
      }
      
      if (EmulateVicCycles)
      {
#ifndef Fab04_DataBufAlwaysEnabled
         SetDataBufOut;  //only read allowed in vic cycle, set data buf to output
#endif
         WaitUntil_nS(nS_VICStart);
         
         GPIO_6 = ReadGPIO6; //Address bus and R/*W 
         Address = GP6_Address(GPIO_6); //parse out address
         GPIO_9 = ReadGPIO9; //Now read the derived signals
         
         if (!GP9_ROMH(GPIO_9)) //ROMH: A000-BFFF or E000-FFFF address space, read only
         {
            if (HIROM_Image!=NULL) DataPortWriteWaitVIC(HIROM_Image[Address & 0x1FFF]); //uses nS_VICDHold hold time
         } 
      }
   }
   
   //leave time enough time to re-trigger on rising edge!
#ifdef DbgSignalIsrPHI2
   SetDebugDeassert;
#endif
}

