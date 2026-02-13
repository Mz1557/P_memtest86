#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Pi/PiMultiPhase.h>
#include <Protocol/MpService.h>

typedef struct {
  volatile UINT64 *Words;
  UINTN Start;
  UINTN End;
  volatile UINT64 FailCount;
} WORKER_CTX;

typedef struct {
  UINT64 TestedBytes;
  UINT64 FailCount;
  UINTN RangeCount;
  UINTN CpuCount;
} TEST_SUMMARY;

STATIC EFI_MP_SERVICES_PROTOCOL *gMp = NULL;
STATIC WORKER_CTX *gWorkers = NULL;

STATIC
VOID
EFIAPI
FillPatternProc(IN VOID *Buffer) {
  UINTN CpuIndex;
  WORKER_CTX *Ctx;
  UINTN i;
  EFI_STATUS Status;

  (VOID)Buffer;
  if (gMp == NULL || gWorkers == NULL) {
    return;
  }

  Status = gMp->WhoAmI(gMp, &CpuIndex);
  if (EFI_ERROR(Status)) {
    return;
  }

  Ctx = &gWorkers[CpuIndex];
  for (i = Ctx->Start; i < Ctx->End; ++i) {
    Ctx->Words[i] = ((i & 1U) == 0U) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
  }
}

STATIC
VOID
EFIAPI
VerifyPatternProc(IN VOID *Buffer) {
  UINTN CpuIndex;
  WORKER_CTX *Ctx;
  UINTN i;
  EFI_STATUS Status;

  (VOID)Buffer;
  if (gMp == NULL || gWorkers == NULL) {
    return;
  }

  Status = gMp->WhoAmI(gMp, &CpuIndex);
  if (EFI_ERROR(Status)) {
    return;
  }

  Ctx = &gWorkers[CpuIndex];
  for (i = Ctx->Start; i < Ctx->End; ++i) {
    UINT64 Expected = ((i & 1U) == 0U) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
    if (Ctx->Words[i] != Expected) {
      Ctx->FailCount++;
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
RunPatternOnRange(
    IN EFI_MP_SERVICES_PROTOCOL *Mp,
    IN OUT WORKER_CTX *Workers,
    IN UINTN WorkerCount,
    IN volatile UINT64 *Base,
    IN UINTN WordCount,
    OUT UINT64 *RangeFailCount,
    OUT UINTN *UsedCpuCount) {
  EFI_STATUS Status;
  UINTN *EnabledCpuList;
  UINTN EnabledCpuCount;
  UINTN TotalCpuCount;
  UINTN BspIndex;
  UINTN Chunk;
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
    Workers[k].Start = 0;
    Workers[k].End = 0;
    Workers[k].FailCount = 0;
  }

  Chunk = (WordCount + EnabledCpuCount - 1U) / EnabledCpuCount;
  for (k = 0; k < EnabledCpuCount; ++k) {
    UINTN Cpu = EnabledCpuList[k];
    UINTN Start = k * Chunk;
    UINTN End = Start + Chunk;
    if (Start > WordCount) {
      Start = WordCount;
    }
    if (End > WordCount) {
      End = WordCount;
    }
    Workers[Cpu].Words = Base;
    Workers[Cpu].Start = Start;
    Workers[Cpu].End = End;
    Workers[Cpu].FailCount = 0;
  }

  gMp = Mp;
  gWorkers = Workers;

  {
    WORKER_CTX *W = &Workers[BspIndex];
    UINTN i;
    for (i = W->Start; i < W->End; ++i) {
      W->Words[i] = ((i & 1U) == 0U) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
    }
  }

  Status = Mp->StartupAllAPs(Mp, FillPatternProc, FALSE, NULL, 0, NULL, NULL);
  if (EFI_ERROR(Status) && Status != EFI_NOT_STARTED) {
    FreePool(EnabledCpuList);
    return Status;
  }

  {
    WORKER_CTX *W = &Workers[BspIndex];
    UINTN i;
    for (i = W->Start; i < W->End; ++i) {
      UINT64 Expected = ((i & 1U) == 0U) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
      if (W->Words[i] != Expected) {
        W->FailCount++;
      }
    }
  }

  Status = Mp->StartupAllAPs(Mp, VerifyPatternProc, FALSE, NULL, 0, NULL, NULL);
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

    Status = RunPatternOnRange(Mp, Workers, TotalCpuCount, Base, Words, &RangeFails, &UsedCpu);
    if (EFI_ERROR(Status)) {
      Print(L"Range test failed at 0x%lx (%r)\n", Desc->PhysicalStart, Status);
      Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Desc + DescSize);
      continue;
    }

    Summary->RangeCount++;
    Summary->TestedBytes += Bytes;
    Summary->FailCount += RangeFails;
    Summary->CpuCount = UsedCpu;

    Print(
        L"[R%u] start=0x%lx pages=%lu fails=%lu\n",
        (UINT32)Summary->RangeCount,
        Desc->PhysicalStart,
        Desc->NumberOfPages,
        RangeFails);

    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Desc + DescSize);
  }

  FreePool(Workers);
  FreePool(MemMap);
  return EFI_SUCCESS;
}

STATIC
VOID
PrintSummary(IN CONST TEST_SUMMARY *Summary, IN EFI_STATUS Status) {
  Print(L"\n=== UEFI Offline MemTest Summary ===\n");
  Print(L"Conventional ranges tested : %lu\n", (UINT64)Summary->RangeCount);
  Print(L"Bytes tested               : %lu\n", Summary->TestedBytes);
  Print(L"CPUs used                  : %lu\n", (UINT64)Summary->CpuCount);
  Print(L"Total fail count           : %lu\n", Summary->FailCount);
  Print(L"Status                     : %r\n", Status);
}

EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  EFI_MP_SERVICES_PROTOCOL *Mp;
  TEST_SUMMARY Summary;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  gBS->SetWatchdogTimer(0, 0, 0, NULL);

  Status = gBS->LocateProtocol(&gEfiMpServiceProtocolGuid, NULL, (VOID **)&Mp);
  if (EFI_ERROR(Status)) {
    Print(L"MP Services not available: %r\n", Status);
    return Status;
  }

  Print(L"Starting offline sweep of EfiConventionalMemory ranges...\n");
  Status = RunOfflineConventionalSweep(Mp, &Summary);
  PrintSummary(&Summary, Status);
  return Status;
}
