#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Pi/PiMultiPhase.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/MpService.h>
#include <Protocol/SimpleFileSystem.h>

#define LOG_LINE_MAX 256

typedef enum {
  PHASE_FILL_CB = 0,
  PHASE_VERIFY_CB,
  PHASE_MOVEINV_FWD,
  PHASE_MOVEINV_BWD,
  PHASE_RANDOM_FILL,
  PHASE_RANDOM_VERIFY,
  PHASE_BLOCK_MOVE,
  PHASE_BITFADE_FILL0,
  PHASE_BITFADE_VERIFY0,
  PHASE_BITFADE_FILL1,
  PHASE_BITFADE_VERIFY1
} TEST_PHASE;

typedef struct {
  volatile UINT64 *Words;
  UINTN WordCount;
  UINTN CpuIndex;
  UINTN CpuCount;
  UINT64 Seed;
  volatile UINT64 FailCount;
  UINT64 *Scratch;
  UINTN ScratchWords;
} WORKER_CTX;

typedef struct {
  UINT64 TestedBytes;
  UINT64 FailCount;
  UINTN RangeCount;
  UINTN CpuCount;
} TEST_SUMMARY;

STATIC EFI_MP_SERVICES_PROTOCOL *gMp = NULL;
STATIC WORKER_CTX *gWorkers = NULL;
STATIC UINTN gBlockWords = 1024; // 8KB blocks
STATIC TEST_PHASE gPhase = PHASE_FILL_CB;

STATIC EFI_FILE_PROTOCOL *gLogFile = NULL;

STATIC
UINT64
XorShift64Star(IN OUT UINT64 *State) {
  UINT64 x = *State;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *State = x;
  return x * 2685821657736338717ULL;
}

STATIC
VOID
LogLine(IN CONST CHAR16 *Format, ...) {
  VA_LIST Marker;
  CHAR16 WideBuf[LOG_LINE_MAX];
  CHAR8 AsciiBuf[LOG_LINE_MAX];
  UINTN i;
  UINTN Size;

  VA_START(Marker, Format);
  UnicodeVSPrint(WideBuf, sizeof(WideBuf), Format, Marker);
  VA_END(Marker);

  Print(L"%s", WideBuf);

  for (i = 0; i < LOG_LINE_MAX - 1; ++i) {
    CHAR16 wc = WideBuf[i];
    if (wc == L'\0') {
      break;
    }
    AsciiBuf[i] = (wc < 0x80) ? (CHAR8)wc : '?';
  }
  AsciiBuf[i] = '\0';

  if (gLogFile != NULL) {
    Size = AsciiStrLen(AsciiBuf);
    gLogFile->Write(gLogFile, &Size, AsciiBuf);
  }
}

STATIC
VOID
InitLogFile(IN EFI_HANDLE ImageHandle) {
  EFI_STATUS Status;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL *Root;

  Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR(Status)) {
    return;
  }

  Status = gBS->HandleProtocol(Loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
  if (EFI_ERROR(Status)) {
    return;
  }

  Status = Sfs->OpenVolume(Sfs, &Root);
  if (EFI_ERROR(Status)) {
    return;
  }

  Status = Root->Open(
      Root,
      &gLogFile,
      L"memtest.log",
      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
      0);
  if (EFI_ERROR(Status)) {
    gLogFile = NULL;
  }
}

STATIC
VOID
CloseLogFile(VOID) {
  if (gLogFile != NULL) {
    gLogFile->Close(gLogFile);
    gLogFile = NULL;
  }
}

STATIC
VOID
EFIAPI
PhaseProc(IN VOID *Buffer) {
  UINTN CpuIndex;
  WORKER_CTX *Ctx;
  EFI_STATUS Status;
  UINTN block;
  UINTN totalBlocks;

  (VOID)Buffer;
  if (gMp == NULL || gWorkers == NULL) {
    return;
  }

  Status = gMp->WhoAmI(gMp, &CpuIndex);
  if (EFI_ERROR(Status)) {
    return;
  }

  Ctx = &gWorkers[CpuIndex];
  if (Ctx->WordCount == 0 || Ctx->CpuCount == 0) {
    return;
  }

  totalBlocks = (Ctx->WordCount + gBlockWords - 1U) / gBlockWords;

  for (block = Ctx->CpuIndex; block < totalBlocks; block += Ctx->CpuCount) {
    UINTN start = block * gBlockWords;
    UINTN end = start + gBlockWords;
    UINTN i;

    if (end > Ctx->WordCount) {
      end = Ctx->WordCount;
    }

    switch (gPhase) {
      case PHASE_FILL_CB:
        for (i = start; i < end; ++i) {
          Ctx->Words[i] = ((i & 1U) == 0U) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
        }
        break;
      case PHASE_VERIFY_CB:
        for (i = start; i < end; ++i) {
          UINT64 Expected = ((i & 1U) == 0U) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
          if (Ctx->Words[i] != Expected) {
            Ctx->FailCount++;
          }
        }
        break;
      case PHASE_MOVEINV_FWD:
        for (i = start; i < end; ++i) {
          UINT64 Expected = 0xAAAAAAAAAAAAAAAAULL;
          if (Ctx->Words[i] != Expected) {
            Ctx->FailCount++;
          }
          Ctx->Words[i] = ~Expected;
        }
        break;
      case PHASE_MOVEINV_BWD:
        for (i = end; i > start; --i) {
          UINTN idx = i - 1U;
          UINT64 Expected = 0x5555555555555555ULL;
          if (Ctx->Words[idx] != Expected) {
            Ctx->FailCount++;
          }
          Ctx->Words[idx] = ~Expected;
        }
        break;
      case PHASE_RANDOM_FILL: {
        UINT64 s = Ctx->Seed;
        for (i = start; i < end; ++i) {
          Ctx->Words[i] = XorShift64Star(&s);
        }
        Ctx->Seed = s;
        break;
      }
      case PHASE_RANDOM_VERIFY: {
        UINT64 s = Ctx->Seed;
        for (i = start; i < end; ++i) {
          UINT64 Expected = XorShift64Star(&s);
          if (Ctx->Words[i] != Expected) {
            Ctx->FailCount++;
          }
        }
        Ctx->Seed = s;
        break;
      }
      case PHASE_BLOCK_MOVE: {
        if (Ctx->Scratch == NULL || Ctx->ScratchWords < (end - start)) {
          break;
        }
        if (end == start) {
          break;
        }
        // Swap with adjacent block if available.
        if (block + 1U < totalBlocks) {
          UINTN peerStart = (block + 1U) * gBlockWords;
          UINTN peerEnd = peerStart + gBlockWords;
          UINTN count = end - start;
          if (peerEnd > Ctx->WordCount) {
            peerEnd = Ctx->WordCount;
          }
          if (peerEnd - peerStart < count) {
            count = peerEnd - peerStart;
          }
          if (count > 0) {
            CopyMem(Ctx->Scratch, (VOID *)&Ctx->Words[start], count * sizeof(UINT64));
            CopyMem((VOID *)&Ctx->Words[start], (VOID *)&Ctx->Words[peerStart], count * sizeof(UINT64));
            CopyMem((VOID *)&Ctx->Words[peerStart], Ctx->Scratch, count * sizeof(UINT64));
          }
        }
        break;
      }
      case PHASE_BITFADE_FILL0:
        for (i = start; i < end; ++i) {
          Ctx->Words[i] = 0x0000000000000000ULL;
        }
        break;
      case PHASE_BITFADE_VERIFY0:
        for (i = start; i < end; ++i) {
          if (Ctx->Words[i] != 0x0000000000000000ULL) {
            Ctx->FailCount++;
          }
        }
        break;
      case PHASE_BITFADE_FILL1:
        for (i = start; i < end; ++i) {
          Ctx->Words[i] = 0xFFFFFFFFFFFFFFFFULL;
        }
        break;
      case PHASE_BITFADE_VERIFY1:
        for (i = start; i < end; ++i) {
          if (Ctx->Words[i] != 0xFFFFFFFFFFFFFFFFULL) {
            Ctx->FailCount++;
          }
        }
        break;
      default:
        break;
    }
  }
}

STATIC
EFI_STATUS
BuildEnabledCpuList(
    IN EFI_MP_SERVICES_PROTOCOL *Mp,
    OUT UINTN **EnabledCpuList,
    OUT UINTN *EnabledCpuCount,
    OUT UINTN *TotalCpuCount,
    OUT UINTN *BspIndex) {
  EFI_STATUS Status;
  UINTN Cpu;
  UINTN Enabled;
  UINTN Total;
  UINTN *List;

  *EnabledCpuList = NULL;
  *EnabledCpuCount = 0;
  *TotalCpuCount = 0;
  *BspIndex = 0;

  Status = Mp->GetNumberOfProcessors(Mp, &Total, &Enabled);
  if (EFI_ERROR(Status) || Enabled == 0 || Total == 0) {
    return EFI_UNSUPPORTED;
  }

  Status = Mp->WhoAmI(Mp, BspIndex);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  List = (UINTN *)AllocateZeroPool(sizeof(UINTN) * Enabled);
  if (List == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Enabled = 0;
  for (Cpu = 0; Cpu < Total; ++Cpu) {
    EFI_PROCESSOR_INFORMATION Info;
    Status = Mp->GetProcessorInfo(Mp, Cpu, &Info);
    if (EFI_ERROR(Status)) {
      continue;
    }
    if ((Info.StatusFlag & PROCESSOR_ENABLED_BIT) != 0U) {
      List[Enabled++] = Cpu;
    }
  }

  if (Enabled == 0) {
    FreePool(List);
    return EFI_UNSUPPORTED;
  }

  *EnabledCpuList = List;
  *EnabledCpuCount = Enabled;
  *TotalCpuCount = Total;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
RunPhaseOnRange(
    IN EFI_MP_SERVICES_PROTOCOL *Mp,
    IN OUT WORKER_CTX *Workers,
    IN UINTN WorkerCount,
    IN volatile UINT64 *Base,
    IN UINTN WordCount,
    IN TEST_PHASE Phase,
    OUT UINT64 *RangeFailCount,
    OUT UINTN *UsedCpuCount) {
  EFI_STATUS Status;
  UINTN *EnabledCpuList;
  UINTN EnabledCpuCount;
  UINTN TotalCpuCount;
  UINTN BspIndex;
  UINTN k;

  *RangeFailCount = 0;
  *UsedCpuCount = 0;

  if (WordCount == 0) {
    return EFI_SUCCESS;
  }

  Status = BuildEnabledCpuList(Mp, &EnabledCpuList, &EnabledCpuCount, &TotalCpuCount, &BspIndex);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  if (WorkerCount < TotalCpuCount) {
    FreePool(EnabledCpuList);
    return EFI_BUFFER_TOO_SMALL;
  }

  for (k = 0; k < WorkerCount; ++k) {
    Workers[k].Words = Base;
    Workers[k].WordCount = WordCount;
    Workers[k].CpuIndex = k;
    Workers[k].CpuCount = EnabledCpuCount;
    Workers[k].FailCount = 0;
    Workers[k].Seed = (UINT64)(UINTN)Base ^ ((UINT64)k << 32) ^ 0x9E3779B97F4A7C15ULL;
  }

  gMp = Mp;
  gWorkers = Workers;
  gPhase = Phase;

  // BSP participates directly.
  PhaseProc(NULL);

  Status = Mp->StartupAllAPs(Mp, PhaseProc, FALSE, NULL, 0, NULL, NULL);
  if (EFI_ERROR(Status) && Status != EFI_NOT_STARTED) {
    FreePool(EnabledCpuList);
    return Status;
  }

  for (k = 0; k < EnabledCpuCount; ++k) {
    UINTN Cpu = EnabledCpuList[k];
    *RangeFailCount += Workers[Cpu].FailCount;
  }
  *UsedCpuCount = EnabledCpuCount;

  FreePool(EnabledCpuList);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
RunFullSuiteOnRange(
    IN EFI_MP_SERVICES_PROTOCOL *Mp,
    IN OUT WORKER_CTX *Workers,
    IN UINTN WorkerCount,
    IN volatile UINT64 *Base,
    IN UINTN WordCount,
    OUT UINT64 *RangeFailCount,
    OUT UINTN *UsedCpuCount) {
  EFI_STATUS Status;
  UINT64 Fails = 0;
  UINT64 PhaseFails = 0;
  UINTN CpuUsed = 0;

  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_FILL_CB, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_VERIFY_CB, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Fails += PhaseFails;

  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_MOVEINV_FWD, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_MOVEINV_BWD, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Fails += PhaseFails;

  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_RANDOM_FILL, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_RANDOM_VERIFY, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Fails += PhaseFails;

  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_BLOCK_MOVE, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Fails += PhaseFails;

  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_BITFADE_FILL0, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(2000000);
  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_BITFADE_VERIFY0, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Fails += PhaseFails;

  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_BITFADE_FILL1, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(2000000);
  Status = RunPhaseOnRange(Mp, Workers, WorkerCount, Base, WordCount, PHASE_BITFADE_VERIFY1, &PhaseFails, &CpuUsed);
  if (EFI_ERROR(Status)) return Status;
  Fails += PhaseFails;

  *RangeFailCount = Fails;
  *UsedCpuCount = CpuUsed;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
RunOfflineConventionalSweep(IN EFI_MP_SERVICES_PROTOCOL *Mp, OUT TEST_SUMMARY *Summary) {
  EFI_STATUS Status;
  UINTN MemMapSize;
  EFI_MEMORY_DESCRIPTOR *MemMap;
  EFI_MEMORY_DESCRIPTOR *Desc;
  UINTN MapKey;
  UINTN DescSize;
  UINT32 DescVersion;
  UINTN Count;
  UINTN i;
  UINTN TotalCpuCount;
  UINTN EnabledCpuCount;
  WORKER_CTX *Workers;
  UINT64 *ScratchPool;
  UINTN ScratchWordsPerCpu = 4096; // 32KB

  ZeroMem(Summary, sizeof(*Summary));

  MemMap = NULL;
  MemMapSize = 0;
  Status = gBS->GetMemoryMap(&MemMapSize, MemMap, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  MemMapSize += (DescSize * 16U);
  MemMap = (EFI_MEMORY_DESCRIPTOR *)AllocatePool(MemMapSize);
  if (MemMap == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->GetMemoryMap(&MemMapSize, MemMap, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR(Status)) {
    FreePool(MemMap);
    return Status;
  }

  Status = Mp->GetNumberOfProcessors(Mp, &TotalCpuCount, &EnabledCpuCount);
  if (EFI_ERROR(Status) || TotalCpuCount == 0) {
    FreePool(MemMap);
    return EFI_UNSUPPORTED;
  }

  Workers = (WORKER_CTX *)AllocateZeroPool(sizeof(WORKER_CTX) * TotalCpuCount);
  if (Workers == NULL) {
    FreePool(MemMap);
    return EFI_OUT_OF_RESOURCES;
  }

  ScratchPool = (UINT64 *)AllocateZeroPool(sizeof(UINT64) * ScratchWordsPerCpu * TotalCpuCount);
  if (ScratchPool == NULL) {
    FreePool(Workers);
    FreePool(MemMap);
    return EFI_OUT_OF_RESOURCES;
  }

  for (i = 0; i < TotalCpuCount; ++i) {
    Workers[i].Scratch = ScratchPool + (i * ScratchWordsPerCpu);
    Workers[i].ScratchWords = ScratchWordsPerCpu;
  }

  Count = MemMapSize / DescSize;
  Desc = MemMap;
  for (i = 0; i < Count; ++i) {
    UINT64 Bytes;
    UINTN Words;
    volatile UINT64 *Base;
    UINT64 RangeFails;
    UINTN UsedCpu;

    if (Desc->Type != EfiConventionalMemory || Desc->NumberOfPages == 0) {
      Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Desc + DescSize);
      continue;
    }

    Bytes = Desc->NumberOfPages * 4096ULL;
    if (Bytes < (64ULL * 1024ULL)) {
      Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Desc + DescSize);
      continue;
    }

    Base = (volatile UINT64 *)(UINTN)Desc->PhysicalStart;
    Words = (UINTN)(Bytes / sizeof(UINT64));

    Status = RunFullSuiteOnRange(Mp, Workers, TotalCpuCount, Base, Words, &RangeFails, &UsedCpu);
    if (EFI_ERROR(Status)) {
      LogLine(L"Range test failed at 0x%lx (%r)\n", Desc->PhysicalStart, Status);
      Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Desc + DescSize);
      continue;
    }

    Summary->RangeCount++;
    Summary->TestedBytes += Bytes;
    Summary->FailCount += RangeFails;
    Summary->CpuCount = UsedCpu;

    LogLine(
        L"[R%u] start=0x%lx pages=%lu fails=%lu\n",
        (UINT32)Summary->RangeCount,
        Desc->PhysicalStart,
        Desc->NumberOfPages,
        RangeFails);

    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Desc + DescSize);
  }

  FreePool(ScratchPool);
  FreePool(Workers);
  FreePool(MemMap);
  return EFI_SUCCESS;
}

STATIC
VOID
PrintSummary(IN CONST TEST_SUMMARY *Summary, IN EFI_STATUS Status) {
  LogLine(L"\n=== UEFI Offline MemTest Summary ===\n");
  LogLine(L"Conventional ranges tested : %lu\n", (UINT64)Summary->RangeCount);
  LogLine(L"Bytes tested               : %lu\n", Summary->TestedBytes);
  LogLine(L"CPUs used                  : %lu\n", (UINT64)Summary->CpuCount);
  LogLine(L"Total fail count           : %lu\n", Summary->FailCount);
  LogLine(L"Status                     : %r\n", Status);
}

EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  EFI_MP_SERVICES_PROTOCOL *Mp;
  TEST_SUMMARY Summary;

  (VOID)SystemTable;

  gBS->SetWatchdogTimer(0, 0, 0, NULL);
  InitLogFile(ImageHandle);

  Status = gBS->LocateProtocol(&gEfiMpServiceProtocolGuid, NULL, (VOID **)&Mp);
  if (EFI_ERROR(Status)) {
    LogLine(L"MP Services not available: %r\n", Status);
    CloseLogFile();
    return Status;
  }

  LogLine(L"Starting offline sweep (checkerboard, moving inversions, random, block move, bit fade)...\n");
  Status = RunOfflineConventionalSweep(Mp, &Summary);
  PrintSummary(&Summary, Status);
  CloseLogFile();
  return Status;
}
