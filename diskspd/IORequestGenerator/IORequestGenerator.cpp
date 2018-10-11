/*

DISKSPD

Copyright(c) Microsoft Corporation
All rights reserved.

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

// IORequestGenerator.cpp : Defines the entry point for the DLL application.
//

//FUTURE EXTENSION: make it compile with /W4

#ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
#endif

#ifndef _WIN32_IE
    #define _WIN32_IE    0x0500
#endif

#include "common.h"
#include "IORequestGenerator.h"

#include <stdio.h>
#include <stdlib.h>
#include <Winioctl.h>   //DISK_GEOMETRY
#include <windows.h>
#include <stddef.h>

#include <Wmistr.h>     //WNODE_HEADER

#include "etw.h"
#include <assert.h>
#include <list>
#include "ThroughputMeter.h"
#include "OverlappedQueue.h"

/*****************************************************************************/
// gets partition size, return zero on failure
//
UINT64 GetPartitionSize(HANDLE hFile)
{
    assert(NULL != hFile && INVALID_HANDLE_VALUE != hFile);

    PARTITION_INFORMATION pinf;
    OVERLAPPED ovlp = {};

    ovlp.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (ovlp.hEvent == nullptr)
    {
        PrintError("ERROR: Failed to create event (error code: %u)\n", GetLastError());
        return 0;
    }

    DWORD rbcnt = 0;
    DWORD status = ERROR_SUCCESS;
    BOOL rslt;

    rslt = DeviceIoControl(hFile,
        IOCTL_DISK_GET_PARTITION_INFO,
        NULL,
        0,
        &pinf,
        sizeof(pinf),
        &rbcnt,
        &ovlp);

    if (!rslt)
    {
        status = GetLastError();
        if (status == ERROR_IO_PENDING)
        {
            if (WAIT_OBJECT_0 != WaitForSingleObject(ovlp.hEvent, INFINITE))
            {
                PrintError("ERROR: Failed while waiting for event to be signaled (error code: %u)\n", GetLastError());
            }
            else
            {
                rslt = TRUE;
            }
        }
        else
        {
            PrintError("ERROR: Could not obtain partition info (error code: %u)\n", status);
        }
    }

    CloseHandle(ovlp.hEvent);

    if (!rslt)
    {
        return 0;
    }

    return pinf.PartitionLength.QuadPart;
}

/*****************************************************************************/
// gets physical drive size, return zero on failure
//
UINT64 GetPhysicalDriveSize(HANDLE hFile)
{
    assert(NULL != hFile && INVALID_HANDLE_VALUE != hFile);

    DISK_GEOMETRY geom;
    OVERLAPPED ovlp = {};

    ovlp.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (ovlp.hEvent == nullptr)
    {
        PrintError("ERROR: Failed to create event (error code: %u)\n", GetLastError());
        return 0;
    }

    DWORD rbcnt = 0;
    DWORD status = ERROR_SUCCESS;
    BOOL rslt;

    rslt = DeviceIoControl(hFile,
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        NULL,
        0,
        &geom,
        sizeof(geom),
        &rbcnt,
        &ovlp);

    if (!rslt)
    {
        status = GetLastError();
        if (status == ERROR_IO_PENDING)
        {
            if (WAIT_OBJECT_0 != WaitForSingleObject(ovlp.hEvent, INFINITE))
            {
                PrintError("ERROR: Failed while waiting for event to be signaled (error code: %u)\n", GetLastError());
            }
            else
            {
                rslt = TRUE;
            }
        }
        else
        {
            PrintError("ERROR: Could not obtain drive geometry (error code: %u)\n", status);
        }
    }

    CloseHandle(ovlp.hEvent);

    if (!rslt)
    {
        return 0;
    }

    return (UINT64)geom.BytesPerSector *
        (UINT64)geom.SectorsPerTrack   *
        (UINT64)geom.TracksPerCylinder *
        (UINT64)geom.Cylinders.QuadPart;
}

/*****************************************************************************/
// activates specified privilege in process token
//
bool SetPrivilege(LPCSTR pszPrivilege)
{
    TOKEN_PRIVILEGES TokenPriv;
    HANDLE hToken = INVALID_HANDLE_VALUE;
    DWORD dwError;
    bool fOk = true;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
    {
        PrintError("Error opening process token (error code: %u)\n", GetLastError());
        fOk = false;
        goto cleanup;
    }

    TokenPriv.PrivilegeCount = 1;
    TokenPriv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValue(nullptr, pszPrivilege, &TokenPriv.Privileges[0].Luid))
    {
        PrintError("Error looking up privilege value %s (error code: %u)\n", pszPrivilege, GetLastError());
        fOk = false;
        goto cleanup;
    }

    if (!AdjustTokenPrivileges(hToken, FALSE, &TokenPriv, 0, nullptr, nullptr))
    {
        PrintError("Error adjusting token privileges for %s (error code: %u)\n", pszPrivilege, GetLastError());
        fOk = false;
        goto cleanup;
    }

    if (ERROR_SUCCESS != (dwError = GetLastError()))
    {
        PrintError("Error adjusting token privileges for %s (error code: %u)\n", pszPrivilege, dwError);
        fOk = false;
        goto cleanup;
    }

cleanup:
    if (hToken != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hToken);
    }
    
    return fOk;
}

/*****************************************************************************/
// structures and global variables
//
struct ETWEventCounters g_EtwEventCounters;

__declspec(align(4)) static LONG volatile g_lRunningThreadsCount = 0;   //must be aligned on a 32-bit boundary, otherwise InterlockedIncrement
                                                                        //and InterlockedDecrement will fail on 64-bit systems

static ULONG volatile g_ulProcCount = 0;        //number of CPUs present in the system
static BOOL volatile g_bRun;                    //used for letting threads know that they should stop working

typedef NTSTATUS (__stdcall *NtQuerySysInfo)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
static NtQuerySysInfo g_pfnNtQuerySysInfo;

static PRINTF g_pfnPrintOut = nullptr;
static PRINTF g_pfnPrintError = nullptr;
static PRINTF g_pfnPrintVerbose = nullptr;

static BOOL volatile g_bThreadError = FALSE;    //true means that an error has occured in one of the threads
BOOL volatile g_bTracing = TRUE;                //true means that ETW is turned on

// TODO: is this still needed?
__declspec(align(4)) static LONG volatile g_lGeneratorRunning = 0;  //used to detect if GenerateRequests is already running

static BOOL volatile g_bError = FALSE;                              //true means there was fatal error during intialization and threads shouldn't perform their work

/*****************************************************************************/
//structure and functions to get system groups and processors information
#define MAXIMUM_GROUPS_LARGE 32

typedef struct {
    WORD wActiveGroupCount;                             //number of groups in the system
    DWORD dwaActiveProcsCount[MAXIMUM_GROUPS_LARGE];    //number of processors per group
} ACTIVE_GROUPS_AND_PROCS, *PACTIVE_GROUPS_AND_PROCS;

PACTIVE_GROUPS_AND_PROCS g_pActiveGroupsAndProcs;

//Win7 kernel32.dll groups and processors exported functions
#define GET_ACTIVE_PROCESSOR_GROUP_COUNT ("GetActiveProcessorGroupCount")
#define GET_ACTIVE_PROCESSOR_COUNT ("GetActiveProcessorCount")
#define SET_THREAD_GROUP_AFFINITY ("SetThreadGroupAffinity")

typedef WORD (WINAPI *PFN_GET_ACTIVE_PROCESSOR_GROUP_COUNT) (VOID);
typedef DWORD (WINAPI *PFN_GET_ACTIVE_PROCESSOR_COUNT) (WORD GroupNumber);
typedef DWORD (WINAPI *PFN_SET_THREAD_GROUP_AFFINITY) (HANDLE hThread, const GROUP_AFFINITY * pGroupAffinity, PGROUP_AFFINITY PreviousGroupAffinity);

// for XP/2003 support
#define SET_FILE_INFORMATION_BY_HANDLE ("SetFileInformationByHandle")
typedef BOOL(WINAPI *NT6_SET_FILE_INFORMATION_BY_HANDLE) (HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS FileInformationClass, LPVOID lpFileInformation, DWORD dwBufferSize);

bool IORequestGenerator::_GetActiveGroupsAndProcs() const
{
    HMODULE kernel32;
    PFN_GET_ACTIVE_PROCESSOR_GROUP_COUNT GetActiveProcessorGroupCount;
    PFN_GET_ACTIVE_PROCESSOR_COUNT GetActiveProcessorCount;
    WORD wActiveGroupCtr = 0;
    SYSTEM_INFO SystemInfo;

    //load kernel32.dll
    kernel32 = LoadLibraryExW(L"kernel32.dll", NULL, 0);
    if (kernel32 == NULL)
    {
        PrintError("ERROR: kernel32.dll library failed to load!\r\n");
        return false;
    }

    //get function address from kernel32.dll for groups
    GetActiveProcessorGroupCount = (PFN_GET_ACTIVE_PROCESSOR_GROUP_COUNT)GetProcAddress(kernel32, GET_ACTIVE_PROCESSOR_GROUP_COUNT);

    if (GetActiveProcessorGroupCount != NULL)
    {
        g_pActiveGroupsAndProcs->wActiveGroupCount = GetActiveProcessorGroupCount();

        //verify that group count number is not bigger than maximume groups supported by the OS
        if (g_pActiveGroupsAndProcs->wActiveGroupCount > MAXIMUM_GROUPS_LARGE)
        {
            PrintError("ERROR: pActiveGroupsAndProcs->wActiveGroupCount = %d\r\n", g_pActiveGroupsAndProcs->wActiveGroupCount);
            PrintError("ERROR: Incorrect value; there can be max %d groups in the system\r\n", MAXIMUM_GROUPS_LARGE);
            return false;
        }

        //get function address from kernel32.dll for processors per group
        GetActiveProcessorCount = (PFN_GET_ACTIVE_PROCESSOR_COUNT)GetProcAddress(kernel32, GET_ACTIVE_PROCESSOR_COUNT);

        if (GetActiveProcessorCount != NULL)
        {
            g_ulProcCount = 0;

            //get number of processors per group
            for (wActiveGroupCtr = 0; wActiveGroupCtr < g_pActiveGroupsAndProcs->wActiveGroupCount; wActiveGroupCtr++)
            {
                g_pActiveGroupsAndProcs->dwaActiveProcsCount[wActiveGroupCtr] = 
                    GetActiveProcessorCount(wActiveGroupCtr);
                g_ulProcCount += g_pActiveGroupsAndProcs->dwaActiveProcsCount[wActiveGroupCtr];
            }
        }
        else
        {
            PrintError("ERROR: GetActiveProcessorCount address not obtained with error: %d.!\r\n", GetLastError());
            return false;
        }
    }
    else
    {
        g_pActiveGroupsAndProcs->wActiveGroupCount = 1;
        g_pActiveGroupsAndProcs->dwaActiveProcsCount[0] = 0;

        GetSystemInfo(&SystemInfo);
        g_pActiveGroupsAndProcs->dwaActiveProcsCount[0] = SystemInfo.dwNumberOfProcessors;
        g_ulProcCount = g_pActiveGroupsAndProcs->dwaActiveProcsCount[0];

        if (g_ulProcCount < 1)
        {
            PrintError("ERROR: Processor count = %d\n", g_pActiveGroupsAndProcs->dwaActiveProcsCount[0]);
            PrintError("ERROR: Incorrect value; there has to be at least 1 processor in the system\n");
            return false;
        }
    }
    if (g_pActiveGroupsAndProcs->wActiveGroupCount > 1 || g_ulProcCount > 64)
    {
        PrintError("WARNING: Complete CPU utilization cannot currently be gathered within DISKSPD for this system.\n"
            "         Use alternate mechanisms to gather this data such as perfmon/logman.\n"
            "         Active KGroups %u > 1 and/or processor count %u > 64.\n",
            g_pActiveGroupsAndProcs->wActiveGroupCount,
            g_ulProcCount);
    }
    return true;
}

VOID SetProcGroupMask(WORD wGroupNum, DWORD dwProcNum, GROUP_AFFINITY *pGroupAffinity)
{
    //must zero this structure first, otherwise it fails to set affinity
    memset(pGroupAffinity, 0, sizeof(GROUP_AFFINITY));

    pGroupAffinity->Group = (USHORT)wGroupNum;
    pGroupAffinity->Mask = (KAFFINITY)1<<dwProcNum;
}

BOOL SetThreadGroupAndProcAffinity(HANDLE hThread, const GROUP_AFFINITY *pGroupAffinity, PGROUP_AFFINITY pPreviousGroupAffinity)
{
    HMODULE kernel32;
    PFN_SET_THREAD_GROUP_AFFINITY SetThreadGroupAffinity;
    DWORD_PTR dwpPrevMask;
    BOOL bStatus;

    //load kernel32.dll
    kernel32 = LoadLibraryExW(L"kernel32.dll", NULL, 0);
    if (kernel32 == NULL)
    {
        PrintError("ERROR: kernel32.dll library failed to load!\r\n");
        return FALSE;
    }

    //get function address from kernel32.dll 
    SetThreadGroupAffinity = (PFN_SET_THREAD_GROUP_AFFINITY)GetProcAddress(
        kernel32, SET_THREAD_GROUP_AFFINITY);

    if (SetThreadGroupAffinity != NULL)
    {
        bStatus = SetThreadGroupAffinity(hThread, pGroupAffinity, pPreviousGroupAffinity);
        if (bStatus == FALSE)
        {
            PrintError("ERROR: SetThreadGroupAffinity failed with error: %d.!\r\n", GetLastError());
            return FALSE;
        }

        return TRUE;
    }
    else
    {
        dwpPrevMask = SetThreadAffinityMask(hThread, (DWORD_PTR)(pGroupAffinity->Mask));
        if (dwpPrevMask == 0)
        {
            PrintError("ERROR: SetThreadAffinityMask failed with error: %d.!\r\n", GetLastError());
            return FALSE;
        }

        return TRUE;
    }
}

/*****************************************************************************/
void IORequestGenerator::_CloseOpenFiles(vector<HANDLE>& vhFiles) const
{
    for (size_t x = 0; x < vhFiles.size(); ++x)
    {
        if ((INVALID_HANDLE_VALUE != vhFiles[x]) && (nullptr != vhFiles[x]))
        {
            if (!CloseHandle(vhFiles[x]))
            {
                PrintError("Warning: unable to close file handle (error code: %u)\n", GetLastError());
            }
            vhFiles[x] = nullptr;
        }
    }
}

/*****************************************************************************/
// wrapper for pfnPrintOut. printf cannot be used directly, because IORequestGenerator.dll
// may be consumed by gui app which doesn't have stdout
static void print(const char *format, ...)
{
    assert(NULL != format);

    if( NULL != g_pfnPrintOut )
    {
        va_list listArg;
        va_start(listArg, format);
        g_pfnPrintOut(format, listArg);
        va_end(listArg);
    }
}

/*****************************************************************************/
// wrapper for pfnPrintError. fprintf(stderr) cannot be used directly, because IORequestGenerator.dll
// may be consumed by gui app which doesn't have stdout
void PrintError(const char *format, ...)
{
    assert(NULL != format);

    if( NULL != g_pfnPrintError )
    {
        va_list listArg;

        va_start(listArg, format);
        g_pfnPrintError(format, listArg);
        va_end(listArg);
    }
}

/*****************************************************************************/
// prints the string only if verbose mode is set to true
//
static void printfv(bool fVerbose, const char *format, ...)
{
    assert(NULL != format);

    if( NULL != g_pfnPrintVerbose && fVerbose )
    {
        va_list argList;
        va_start(argList, format);
        g_pfnPrintVerbose(format, argList);
        va_end(argList);
    }
}

/*****************************************************************************/
// thread for gathering ETW data (etw functions are defined in etw.cpp)
//
DWORD WINAPI etwThreadFunc(LPVOID cookie)
{
    UNREFERENCED_PARAMETER(cookie);

    g_bTracing = TRUE;
    BOOL result = TraceEvents();
    g_bTracing = FALSE;

    return result ? 0 : 1;
}

/*****************************************************************************/
// display file size in a user-friendly form using 'verbose' stream
//
void IORequestGenerator::_DisplayFileSizeVerbose(bool fVerbose, UINT64 fsize) const
{
    if( fsize > (UINT64)10*1024*1024*1024 )     // > 10GB
    {
        printfv(fVerbose, "%I64uGB", fsize >> 30);
    }
    else if( fsize > (UINT64)10*1024*1024 )     // > 10MB
    {
        printfv(fVerbose, "%I64uMB", fsize >> 20);
    }
    else if( fsize > 10*1024 )                  // > 10KB
    {
        printfv(fVerbose, "%I64uKB", fsize >> 10);
    }
    else
    {
        printfv(fVerbose, "%I64uB", fsize);
    }
}

/*****************************************************************************/
// generate 64-bit random number
static ULONG64 rand64()
{
    return
        ((((ULONG64)rand()) & 0x7fff) |
        ((((ULONG64)rand()) & 0x7fff) << 15) |
        ((((ULONG64)rand()) & 0x7fff) << 30) |
        (((((ULONG64)rand()) & 0x7fff) << 30) << 15) |
        (((((ULONG64)rand()) & 0xF) << 30) << 30));
}

/*****************************************************************************/
bool IORequestGenerator::_LoadDLLs()
{
    _hNTDLL = LoadLibraryExW(L"ntdll.dll", nullptr, 0);
    if( nullptr == _hNTDLL )
    {
        return false;
    }

    g_pfnNtQuerySysInfo = (NtQuerySysInfo)GetProcAddress(_hNTDLL, "NtQuerySystemInformation");
    if( nullptr == g_pfnNtQuerySysInfo )
    {
        return false;
    }

    return true;
}

/*****************************************************************************/
// returns the number of CPUs present in the system
//
static ULONG getProcessorCount()
{
  SYSTEM_INFO SystemInfo;

  // get system information
  GetSystemInfo(&SystemInfo);

  // and extract number or processors from there
  return (ULONG)SystemInfo.dwNumberOfProcessors; 
}

/*****************************************************************************/
// returns affinity mask
//
static DWORD_PTR getCPUMask(ULONG ulProcNum)
{
    assert(ulProcNum < 8 * sizeof(DWORD_PTR));

    return ((DWORD_PTR)1) << ulProcNum;
}

/*****************************************************************************/
bool IORequestGenerator::_GetSystemPerfInfo(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION *pInfo, UINT32 uCpuCount) const
{
    NTSTATUS Status = NO_ERROR;

    assert(NULL != pInfo);
    assert(uCpuCount > 0);

    Status = g_pfnNtQuerySysInfo(SystemProcessorPerformanceInformation,
                                    pInfo,
                                    sizeof(*pInfo) * uCpuCount,
                                    NULL);

    return NT_SUCCESS(Status);
}

/*****************************************************************************/
// calculate the offset of the next I/O operation
//

__inline UINT64 IORequestGenerator::GetNextFileOffset(ThreadParameters& tp, size_t targetNum, UINT64 prevOffset)
{
    Target &target = tp.vTargets[targetNum];

    UINT64 blockAlignment = target.GetBlockAlignmentInBytes();
    UINT64 baseFileOffset = target.GetBaseFileOffsetInBytes();
    UINT64 blockSize = target.GetBlockSizeInBytes();
    UINT64 nextBlockOffset;

    // increment/produce - note, logically relative to base offset
    if (target.GetUseRandomAccessPattern())
    {
        nextBlockOffset = rand64();
        nextBlockOffset -= (nextBlockOffset % blockAlignment);
    }
    else if (target.GetUseParallelAsyncIO())
    {
        nextBlockOffset = prevOffset - baseFileOffset + blockAlignment;
    }
    else if (target.GetUseInterlockedSequential())
    {
        nextBlockOffset = InterlockedAdd64((PLONGLONG) &tp.pullSharedSequentialOffsets[targetNum], blockAlignment) - blockAlignment;
    }
    else // normal sequential access pattern
    {
        nextBlockOffset = (tp.vullPrivateSequentialOffsets[targetNum] += blockAlignment);
    }

    // now apply bounds for IO offset
    // aligned target size is the closed interval of byte offsets at which it is legal to issue IO
    // ISSUE IMPROVEMENT: much of this should be precalculated. It belongs within Target, which will
    //      need discovery of target sizing moved from its current just-in-time at thread launch.
    UINT64 alignedTargetSize = tp.vullFileSizes[targetNum] - baseFileOffset - blockSize;
    if (target.GetUseRandomAccessPattern() ||
        target.GetUseInterlockedSequential())
    {
        // these access patterns occur on blockaligned boundaries relative to base
        // convert aligned target size to the open interval
        alignedTargetSize = ((alignedTargetSize / blockAlignment) + 1) * blockAlignment;
        nextBlockOffset %= alignedTargetSize;
    }
    else
    {
        // parasync and seq bases are potentially modified by threadstride and loop back to the
        // file base offset + increment which will return them to their initial base offset.
        if (nextBlockOffset > alignedTargetSize)
        {
            nextBlockOffset = (IORequestGenerator::GetThreadBaseFileOffset(tp, targetNum) - baseFileOffset) % blockAlignment;
            tp.vullPrivateSequentialOffsets[targetNum] = nextBlockOffset;
        }
    }

    // Convert into the next full offset
    nextBlockOffset += baseFileOffset;

#ifndef NDEBUG
    // Don't overrun the end of the file
    UINT64 fileSize = tp.vullFileSizes[targetNum];
    assert(nextBlockOffset + blockSize <= fileSize);
#endif

    return nextBlockOffset;
}

__inline UINT64 IORequestGenerator::GetThreadBaseFileOffset(ThreadParameters& tp, size_t targetNum)
{
    const Target &target = tp.vTargets[targetNum];

    UINT64 baseFileOffset = target.GetBaseFileOffsetInBytes();
    UINT64 nextBlockOffset;

    if (target.GetUseRandomAccessPattern())
    {
        nextBlockOffset = IORequestGenerator::GetNextFileOffset(tp, targetNum, 0);
    }
    else
    {
        // interlocked sequential   - thread stride is always zero, enforced during profile validation
        // parallel async           - apply thread stride
        // sequential               - apply thread stride
        nextBlockOffset = baseFileOffset + tp.ulRelativeThreadNo * target.GetThreadStrideInBytes();
    }

    return nextBlockOffset;
}

__inline UINT64 IORequestGenerator::GetStartingFileOffset(ThreadParameters& tp, size_t targetNum)
{
    const Target &target = tp.vTargets[targetNum];

    UINT64 baseFileOffset = target.GetBaseFileOffsetInBytes();
    UINT64 nextBlockOffset;

    if (target.GetUseRandomAccessPattern())
    {
        nextBlockOffset = IORequestGenerator::GetNextFileOffset(tp, targetNum, 0);
    }
    else
    {
        // interlocked sequential   - getnext starts the clock from zero, thread independent
        // parallel async           - getthreadbase, thread dependent
        // sequential               - "", and initialize private counter
        if (target.GetUseInterlockedSequential())
        {
            nextBlockOffset = IORequestGenerator::GetNextFileOffset(tp, targetNum, 0);
        }
        else
        {
            nextBlockOffset = IORequestGenerator::GetThreadBaseFileOffset(tp, targetNum);

            if (!target.GetUseParallelAsyncIO())
            {
                tp.vullPrivateSequentialOffsets[targetNum] = nextBlockOffset - baseFileOffset;
            }
        }
    }

    return nextBlockOffset;
}

/*****************************************************************************/
// Decide the kind of IO to issue during a mix test
// Future Work: Add more types of distribution in addition to random
__inline static IOOperation DecideIo(UINT32 ulWriteRatio)
{
    return (((UINT32)abs(rand() % 100 + 1)) > ulWriteRatio) ? IOOperation::ReadIO : IOOperation::WriteIO;
 }

/*****************************************************************************/
// function called from worker thread
// performs asynch I/O using IO Completion Ports
//
__inline static bool doWorkUsingIOCompletionPorts(ThreadParameters *p, HANDLE hCompletionPort)
{
    assert(nullptr!= p);
    assert(nullptr != hCompletionPort);

    bool fOk = true;

    LARGE_INTEGER li;
    BOOL rslt = FALSE;
    OVERLAPPED * pCompletedOvrp;
    ULONG_PTR ulCompletionKey;
    DWORD dwBytesTransferred;
    DWORD dwIOCnt = 0;
    OverlappedQueue overlappedQueue;
    size_t cOverlapped = p->vOverlapped.size();

    bool fMeasureLatency = p->pTimeSpan->GetMeasureLatency();

    size_t cTargets = p->vTargets.size();
    vector<ThroughputMeter> vThroughputMeters(cTargets);
    bool fUseThrougputMeter = false;
    // TODO: move to a separate function
    for (size_t i = 0; i < cTargets; i++)
    {
        Target *pTarget = &p->vTargets[i];
        DWORD dwBurstSize = pTarget->GetBurstSize();
        if (p->pTimeSpan->GetThreadCount() > 0)
        {
            dwBurstSize /= p->pTimeSpan->GetThreadCount();
        }
        else
        {
            dwBurstSize /= pTarget->GetThreadsPerFile();
        }

        if (pTarget->GetThroughputInBytesPerMillisecond() > 0 || pTarget->GetThinkTime() > 0)
        {
            fUseThrougputMeter = true;
            vThroughputMeters[i].Start(pTarget->GetThroughputInBytesPerMillisecond(), pTarget->GetBlockSizeInBytes(), pTarget->GetThinkTime(), dwBurstSize);
        }
    }

    //start IO operations
    for (size_t i = 0; i < cOverlapped; i++)
    {
        overlappedQueue.Add(&p->vOverlapped[i]);
    }

    //
    // perform work
    //
    while(g_bRun && !g_bThreadError)
    {
        DWORD dwMinSleepTime = ~((DWORD)0);
        for (size_t i = 0; i < overlappedQueue.GetCount(); i++)
        {
            OVERLAPPED *pReadyOverlapped = overlappedQueue.Remove();
            DWORD iOverlapped = (DWORD)(pReadyOverlapped - &p->vOverlapped[0]);
            size_t iTarget = p->vOverlappedIdToTargetId[iOverlapped];
            size_t iRequest = iOverlapped - p->vFirstOverlappedIdForTargetId[iTarget];
            Target *pTarget = &p->vTargets[iTarget];
            ThroughputMeter *pThroughputMeter = &vThroughputMeters[iTarget];

            DWORD dwSleepTime = pThroughputMeter->GetSleepTime();
            if (pThroughputMeter->IsRunning() && dwSleepTime > 0)
            {
                dwMinSleepTime = min(dwMinSleepTime, dwSleepTime);
                overlappedQueue.Add(pReadyOverlapped);
                continue;
            }

            if (fMeasureLatency)
            {
                p->vIoStartTimes[iOverlapped] = PerfTimer::GetTime(); // record IO start time 
            }

            IOOperation readOrWrite;
            readOrWrite = p->vdwIoType[iOverlapped] = DecideIo(pTarget->GetWriteRatio());
            if (readOrWrite == IOOperation::ReadIO)
            {
                rslt = ReadFile(p->vhTargets[iTarget], p->GetReadBuffer(iTarget, iRequest), pTarget->GetBlockSizeInBytes(), nullptr, pReadyOverlapped);
            }
            else
            {
                rslt = WriteFile(p->vhTargets[iTarget], p->GetWriteBuffer(iTarget, iRequest), pTarget->GetBlockSizeInBytes(), nullptr, pReadyOverlapped);
            }

            if (!rslt && GetLastError() != ERROR_IO_PENDING)
            {
                PrintError("t[%u] error during %s error code: %u)\n", iOverlapped, (readOrWrite == IOOperation::ReadIO ? "read" : "write"), GetLastError());
                fOk = false;
                goto cleanup;
            }

            if (pThroughputMeter->IsRunning())
            {
                pThroughputMeter->Adjust(pTarget->GetBlockSizeInBytes());
            }
        }

        // if no IOs are in flight, wait for the next scheduling time
        if (fUseThrougputMeter && (overlappedQueue.GetCount() == p->vOverlapped.size()) && dwMinSleepTime != ~((DWORD)0))
        {
            Sleep(dwMinSleepTime);
        }

        // wait till one of the IO operations finishes
        if (GetQueuedCompletionStatus(hCompletionPort, &dwBytesTransferred, &ulCompletionKey, &pCompletedOvrp, 1) != 0)
        {
            //find which I/O operation it was (so we know to which buffer should we use)
            DWORD iOverlapped = (DWORD)(pCompletedOvrp - &p->vOverlapped[0]);
            size_t iTarget = p->vOverlappedIdToTargetId[iOverlapped];

            //check if I/O transferred all of the requested bytes
            Target *pTarget = &p->vTargets[iTarget];
            if (dwBytesTransferred != pTarget->GetBlockSizeInBytes())
            {
                PrintError("Warning: thread %u transferred %u bytes instead of %u bytes\n",
                    p->ulThreadNo,
                    dwBytesTransferred,
                    pTarget->GetBlockSizeInBytes());
            }

            li.HighPart = pCompletedOvrp->OffsetHigh;
            li.LowPart = pCompletedOvrp->Offset;

            if (*p->pfAccountingOn)
            {
                p->pResults->vTargetResults[iTarget].Add(dwBytesTransferred,
                    p->vdwIoType[iOverlapped],
                    &p->vIoStartTimes[iOverlapped],
                    p->pullStartTime,
                    fMeasureLatency,
                    p->pTimeSpan->GetCalculateIopsStdDev());
            }

            // TODO: move to a separate function
            // check if we should print a progress dot
            if (p->pProfile->GetProgress() != 0)
            {
                ++dwIOCnt;
                if (dwIOCnt == p->pProfile->GetProgress())
                {
                    print(".");
                    dwIOCnt = 0;
                }
            }

            //restart the I/O operation that just completed
            li.QuadPart = IORequestGenerator::GetNextFileOffset(*p, iTarget, li.QuadPart);

            pCompletedOvrp->Offset = li.LowPart;
            pCompletedOvrp->OffsetHigh = li.HighPart;

            printfv(p->pProfile->GetVerbose(), "t[%u:%u] new I/O op at %I64u (starting in block: %I64u)\n",
                p->ulThreadNo,
                iTarget,
                li.QuadPart,
                li.QuadPart / pTarget->GetBlockSizeInBytes());

            overlappedQueue.Add(pCompletedOvrp);
        }
        else
        {
            DWORD err = GetLastError();
            if (err != WAIT_TIMEOUT)
            {
                PrintError("error during overlapped IO operation (error code: %u)\n", err);
                fOk = false;
                goto cleanup;
            }
        }
    } // end work loop

cleanup:
    return fOk;
}

/*****************************************************************************/
// I/O completion routine. used by ReadFileEx and WriteFileEx
//

VOID CALLBACK fileIOCompletionRoutine(DWORD dwErrorCode, DWORD dwBytesTransferred, LPOVERLAPPED pOverlapped)
{
    assert(NULL != pOverlapped);

    BOOL rslt = FALSE;
    LARGE_INTEGER li;

    ThreadParameters *p = (ThreadParameters *)pOverlapped->hEvent;
    bool fMeasureLatency = p->pTimeSpan->GetMeasureLatency();

    assert(NULL != p);

    //check error code
    if (0 != dwErrorCode)
    {
        PrintError("Thread %u failed executing an I/O operation (error code: %u)\n", p->ulThreadNo, dwErrorCode);
        goto cleanup;
    }

    size_t iOverlapped = (pOverlapped - &p->vOverlapped[0]);
    size_t iTarget = p->vOverlappedIdToTargetId[iOverlapped];
    Target *pTarget = &p->vTargets[iTarget];

    //check if I/O operation transferred requested number of bytes
    if (dwBytesTransferred != pTarget->GetBlockSizeInBytes())
    {
        PrintError("Warning: thread %u transferred %u bytes instead of %u bytes\n",
            p->ulThreadNo,
            dwBytesTransferred,
            pTarget->GetBlockSizeInBytes());
    }

    // check if we should print a progress dot
    // BUGBUG: does not work ... io counter must be global
    DWORD cdwIO = 0;
    if (p->pProfile->GetProgress() != 0)
    {
        ++cdwIO;
        if (cdwIO == p->pProfile->GetProgress())
        {
            print(".");
            cdwIO = 0;
        }
    }

    if (*p->pfAccountingOn)
    {
        p->pResults->vTargetResults[iTarget].Add(dwBytesTransferred,
            p->vdwIoType[iOverlapped],
            &p->vIoStartTimes[iOverlapped],
            p->pullStartTime,
            fMeasureLatency,
            p->pTimeSpan->GetCalculateIopsStdDev());
    }

    //restart the I/O operation that just completed
    li.HighPart = pOverlapped->OffsetHigh;
    li.LowPart = pOverlapped->Offset;

    li.QuadPart = IORequestGenerator::GetNextFileOffset(*p, iTarget, li.QuadPart);

    pOverlapped->Offset = li.LowPart;
    pOverlapped->OffsetHigh = li.HighPart;

    printfv(p->pProfile->GetVerbose(), "t[%u:%u] new I/O op at %I64u (starting in block: %I64u)\n",
        p->ulThreadNo,
        iTarget,
        li.QuadPart,
        li.QuadPart / pTarget->GetBlockSizeInBytes());

    // start a new IO operation
    if (g_bRun && !g_bThreadError)
    {
        size_t iRequest = iOverlapped - p->vFirstOverlappedIdForTargetId[iTarget];
        if (fMeasureLatency)
        {
            p->vIoStartTimes[iOverlapped] = PerfTimer::GetTime(); // record IO start time 
        }

        IOOperation readOrWrite;
        readOrWrite = p->vdwIoType[iOverlapped] = DecideIo(pTarget->GetWriteRatio());
        if (readOrWrite == IOOperation::ReadIO)
        {
            rslt = ReadFileEx(p->vhTargets[iTarget], p->GetReadBuffer(iTarget, iRequest), pTarget->GetBlockSizeInBytes(), pOverlapped, fileIOCompletionRoutine);
        }
        else
        {
            rslt = WriteFileEx(p->vhTargets[iTarget], p->GetWriteBuffer(iTarget, iRequest), pTarget->GetBlockSizeInBytes(), pOverlapped, fileIOCompletionRoutine);
        }

        if (!rslt)
        {
            PrintError("t[%u:%u] error during %s error code: %u)\n", p->ulThreadNo, iTarget, (readOrWrite == IOOperation::ReadIO ? "read" : "write"), GetLastError());
            goto cleanup;
        }
    }

cleanup:
    return;
}

/*****************************************************************************/
// function called from worker thread
// performs asynch I/O using IO Completion Routines (ReadFileEx, WriteFileEx)
//
__inline static bool doWorkUsingCompletionRoutines(ThreadParameters *p)
{
    assert(NULL != p);
    bool fOk = true;
    BOOL rslt = FALSE;
    
    //start IO operations
    size_t iOverlapped = 0;

    bool fMeasureLatency = p->pTimeSpan->GetMeasureLatency();

    for (size_t iTarget = 0; iTarget < p->vTargets.size(); iTarget++)
    {
        Target *pTarget = &p->vTargets[iTarget];
        for (size_t iRequest = 0; iRequest < pTarget->GetRequestCount(); ++iRequest)
        {
            if (fMeasureLatency)
            {
                p->vIoStartTimes[iOverlapped] = PerfTimer::GetTime(); // record IO start time 
            }

            IOOperation readOrWrite;
            readOrWrite = p->vdwIoType[iOverlapped] = DecideIo(pTarget->GetWriteRatio());
            if (readOrWrite == IOOperation::ReadIO)
            {
                rslt = ReadFileEx(p->vhTargets[iTarget], p->GetReadBuffer(iTarget, iRequest), pTarget->GetBlockSizeInBytes(), &p->vOverlapped[iOverlapped], fileIOCompletionRoutine);
            }
            else
            {
                rslt = WriteFileEx(p->vhTargets[iTarget], p->GetWriteBuffer(iTarget, iRequest), pTarget->GetBlockSizeInBytes(), &p->vOverlapped[iOverlapped], fileIOCompletionRoutine);
            }

            if (!rslt)
            {
                PrintError("t[%u:%u] error during %s error code: %u)\n", p->ulThreadNo, iTarget, (readOrWrite == IOOperation::ReadIO ? "read" : "write"), GetLastError());
                fOk = false;
                goto cleanup;
            }
            iOverlapped++;
        }
    }

    DWORD dwWaitResult = 0;
    while( g_bRun && !g_bThreadError )
    {
        dwWaitResult = WaitForSingleObjectEx(p->hEndEvent, INFINITE, TRUE);

        assert(WAIT_IO_COMPLETION == dwWaitResult || (WAIT_OBJECT_0 == dwWaitResult && (!g_bRun || g_bThreadError)));

        //check WaitForSingleObjectEx status
        if( WAIT_IO_COMPLETION != dwWaitResult && WAIT_OBJECT_0 != dwWaitResult )
        {
            PrintError("Error in thread %u during WaitForSingleObjectEx (in completion routines)\n", p->ulThreadNo);
            fOk = false;
            goto cleanup;
        }
    }
cleanup:
    return fOk;
}

/*****************************************************************************/
// worker thread function
//
DWORD WINAPI threadFunc(LPVOID cookie)
{
	/// for XP/20003 support
	static bool once = true;
	NT6_SET_FILE_INFORMATION_BY_HANDLE Nt6SetFileInformationByHandle = NULL;

	if (once)
	{
		//load kernel32.dll
		HMODULE kernel32;
		kernel32 = LoadLibraryExW(L"kernel32.dll", NULL, 0);
		if (kernel32 == NULL)
		{
			PrintError("ERROR: kernel32.dll library failed to load!\r\n");
			return FALSE;
		}

		//get function address from kernel32.dll 
		Nt6SetFileInformationByHandle = (NT6_SET_FILE_INFORMATION_BY_HANDLE) GetProcAddress(
			kernel32, SET_FILE_INFORMATION_BY_HANDLE);

		once = false;
	}
	///


    bool fOk = true;
    ThreadParameters *p = reinterpret_cast<ThreadParameters *>(cookie);
    HANDLE hCompletionPort = nullptr;

    bool fMeasureLatency = p->pTimeSpan->GetMeasureLatency();
    bool fCalculateIopsStdDev = p->pTimeSpan->GetCalculateIopsStdDev();
    UINT64 ioBucketDuration = 0;
    UINT32 expectedNumberOfBuckets = 0;
    if(fCalculateIopsStdDev)
    {
        UINT32 ioBucketDurationInMilliseconds = p->pTimeSpan->GetIoBucketDurationInMilliseconds();
        ioBucketDuration = PerfTimer::MillisecondsToPerfTime(ioBucketDurationInMilliseconds);
        expectedNumberOfBuckets = Util::QuotientCeiling(p->pTimeSpan->GetDuration() * 1000, ioBucketDurationInMilliseconds);
    }

    //set random seed (each thread has a different one)
    srand(p->ulRandSeed);

    //affinity
    ULONG ulGroupProcs = 0;
    if (!p->pTimeSpan->GetGroupAffinity())
    {
        assert(g_ulProcCount > 0);
        ulGroupProcs = getProcessorCount();
    }

    //simple affinity
    if (!p->pTimeSpan->GetDisableAffinity() && (p->pTimeSpan->GetAffinityAssignments().size() == 0) && !p->pTimeSpan->GetGroupAffinity())
    {
        HANDLE hThread = GetCurrentThread();
        ULONG ulProcNum = p->ulThreadNo % ulGroupProcs;
        printfv(p->pProfile->GetVerbose(), "affinitizing thread %u to CPU%u\n", p->ulThreadNo, ulProcNum);

        // set thread affinity
        if (0 == SetThreadAffinityMask(hThread, getCPUMask(ulProcNum)))
        {
            PrintError("Error setting affinity mask in thread %u\n", p->ulThreadNo);
            fOk = false;
            goto cleanup;
        }

        // set thread ideal processor
        if ((DWORD)-1 == SetThreadIdealProcessor(hThread, ulProcNum))
        {
            PrintError("Error setting ideal processor in thread %u\n", p->ulThreadNo);
            fOk = false;
            goto cleanup;
        }
    }

    //advanced affinity
    if (!p->pTimeSpan->GetDisableAffinity() && (p->pTimeSpan->GetAffinityAssignments().size() > 0) && !p->pTimeSpan->GetGroupAffinity())
    {
        vector<UINT32> vAffinity(p->pTimeSpan->GetAffinityAssignments());

        ULONG ulProcNum = p->ulThreadNo % vAffinity.size();
        UINT32 proc = vAffinity[ulProcNum];

        assert(proc < g_ulProcCount);
        if (ulProcNum >= g_ulProcCount)
        {
            PrintError("Invalid affinity mask (CPU id cannot be larger than the number of CPUs in the system)\n");
            fOk = false;
            goto cleanup;
        }

        printfv(p->pProfile->GetVerbose(), "affinitizing thread %u to CPU%u\n", p->ulThreadNo, proc);

        HANDLE hThread = GetCurrentThread();

        // set thread affinity
        if (0 == SetThreadAffinityMask(hThread, getCPUMask(proc)))
        {
            PrintError("Error setting affinity mask in thread %u\n", p->ulThreadNo);
            fOk = false;
            goto cleanup;
        }

        // set thread ideal processor
        if ((DWORD)-1 == SetThreadIdealProcessor(hThread, proc))
        {
            PrintError("Error setting ideal processor in thread %u\n", p->ulThreadNo);
            fOk = false;
            goto cleanup;
        }
    }

    //group affinity
    if (!p->pTimeSpan->GetDisableAffinity() && (p->pTimeSpan->GetAffinityAssignments().size() == 0) && p->pTimeSpan->GetGroupAffinity())
    {
        SetProcGroupMask(p->wGroupNum, p->dwProcNum, &p->GroupAffinity);

        HANDLE hThread = GetCurrentThread();
        if (SetThreadGroupAndProcAffinity(hThread, &p->GroupAffinity, nullptr) == FALSE)
        {
            PrintError("Error setting affinity mask in thread %u\n", p->ulThreadNo);
            fOk = false;
            goto cleanup;
        }
    }

    // adjust thread token if large pages are needed
    for (auto pTarget = p->vTargets.begin(); pTarget != p->vTargets.end(); pTarget++)
    {
        if (pTarget->GetUseLargePages())
        {
            if (!SetPrivilege(SE_LOCK_MEMORY_NAME))
            {
                fOk = false;
                goto cleanup;
            }
            break;
        }
    }

    // TODO: open files
    size_t iTarget = 0;
    for (auto pTarget = p->vTargets.begin(); pTarget != p->vTargets.end(); pTarget++)
    {
        bool fPhysical = false;
        bool fPartition = false;

        string sPath(pTarget->GetPath());
        const char *filename = sPath.c_str();

        const char *fname = nullptr;    //filename (can point to physFN)
        char physFN[32];                //disk/partition name

        if (NULL == filename || NULL == *(filename))
        {
            PrintError("FATAL ERROR: invalid filename\n");
            fOk = false;
            goto cleanup;
        }

        //check if it is a physical drive
        if ('#' == *filename && NULL != *(filename + 1))
        {
            UINT32 nDriveNo = (UINT32)atoi(filename + 1);
            fPhysical = true;
            sprintf_s(physFN, 32, "\\\\.\\PhysicalDrive%u", nDriveNo);
            fname = physFN;
        }

        //check if it is a partition
        if (!fPhysical && NULL != *(filename + 1) && NULL == *(filename + 2) && isalpha((unsigned char)filename[0]) && ':' == filename[1])
        {
            fPartition = true;

            sprintf_s(physFN, 32, "\\\\.\\%c:", filename[0]);
            fname = physFN;
        }

        //check if it is a regular file
        if (!fPhysical && !fPartition)
        {
            fname = sPath.c_str();
        }

        //set file flags
        DWORD dwFlags = FILE_ATTRIBUTE_NORMAL;

        if (pTarget->GetSequentialScanHint())
        {
            dwFlags |= FILE_FLAG_SEQUENTIAL_SCAN;
        }

        if (pTarget->GetRandomAccessHint())
        {
            dwFlags |= FILE_FLAG_RANDOM_ACCESS;
        }

        if ((pTarget->GetRequestCount() > 1) || (p->vTargets.size() > 1))
        {
            dwFlags |= FILE_FLAG_OVERLAPPED;
        }

        if (pTarget->GetDisableOSCache())
        {
            dwFlags |= FILE_FLAG_NO_BUFFERING;
        }

        if (pTarget->GetDisableAllCache())
        {
            dwFlags |= (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
        }

        DWORD dwDesiredAccess = 0;
        if (pTarget->GetWriteRatio() == 0)
        {
            dwDesiredAccess = GENERIC_READ;
        }
        else if (pTarget->GetWriteRatio() == 100)
        {
            dwDesiredAccess = GENERIC_WRITE;
        }
        else
        {
            dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
        }

        HANDLE hFile = CreateFile(fname,
            dwDesiredAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,        //security
            OPEN_EXISTING,
            dwFlags,        //flags (add overlapping)
            nullptr);       //template file
        if (INVALID_HANDLE_VALUE == hFile)
        {
            // TODO: error out
            PrintError("Error opening file: %s [%u]\n", sPath.c_str(), GetLastError());
            fOk = false;
            goto cleanup;
        }

        p->vhTargets.push_back(hFile);

        //set IO priority
		if (Nt6SetFileInformationByHandle && pTarget->GetIOPriorityHint() != IoPriorityHintNormal)
        {
            FILE_IO_PRIORITY_HINT_INFO hintInfo;
            hintInfo.PriorityHint = pTarget->GetIOPriorityHint();
			if (!Nt6SetFileInformationByHandle(hFile, FileIoPriorityHintInfo, &hintInfo, sizeof(hintInfo)))
            {
                PrintError("Error setting IO priority for file: %s [%u]\n", sPath.c_str(), GetLastError());
                fOk = false;
                goto cleanup;
            }
        }

        // obtain file/disk/partition size
        {
            UINT64 fsize = 0;   //file size

            //check if it is a disk
            if (fPhysical)
            {
                fsize = GetPhysicalDriveSize(hFile);
            }
            // check if it is a partition
            else if (fPartition)
            {
                fsize = GetPartitionSize(hFile);
            }
            // it has to be a regular file
            else
            {
                ULARGE_INTEGER ulsize;

                ulsize.LowPart = GetFileSize(hFile, &ulsize.HighPart);
                if (INVALID_FILE_SIZE == ulsize.LowPart && GetLastError() != NO_ERROR)
                {
                    PrintError("Error getting file size\n");
                    fOk = false;
                    goto cleanup;
                }
                else
                {
                    fsize = ulsize.QuadPart;
                }
            }

            // check if file size is valid (if it's == 0, it won't be useful)
            if (0 == fsize)
            {
                // TODO: error out
                PrintError("The file is too small or there has been an error during getting file size\n");
                fOk = false;
                goto cleanup;
            }

            if (fsize < pTarget->GetMaxFileSize())
            {
                PrintError("Warning - file size is less than MaxFileSize\n");
            }

            if (pTarget->GetMaxFileSize() > 0)
            {
                // user wants to use only a part of the target
                // if smaller, of course use the entire content
                p->vullFileSizes.push_back(pTarget->GetMaxFileSize() > fsize ? fsize : pTarget->GetMaxFileSize());
            }
            else
            {
                // the whole file will be used
                p->vullFileSizes.push_back(fsize);
            }

            UINT64 startingFileOffset = IORequestGenerator::GetThreadBaseFileOffset(*p, iTarget);

            // test whether the file is large enough for this thread to do work
            if (startingFileOffset + pTarget->GetBlockSizeInBytes() >= p->vullFileSizes[iTarget])
            {
                PrintError("The file is too small. File: '%s' relative thread %u size: %I64u, base offset: %I64u block size: %u\n",
                    pTarget->GetPath().c_str(),
                    p->ulRelativeThreadNo,
                    fsize,
                    pTarget->GetBaseFileOffsetInBytes(),
                    pTarget->GetBlockSizeInBytes());
                fOk = false;
                goto cleanup;
            }

            if (pTarget->GetUseRandomAccessPattern())
            {
                printfv(p->pProfile->GetVerbose(), "thread %u starting: file '%s' relative thread %u random pattern\n",
                    p->ulThreadNo,
                    pTarget->GetPath().c_str(),
                    p->ulRelativeThreadNo);
            }
            else
            {
                printfv(p->pProfile->GetVerbose(), "thread %u starting: file '%s' relative thread %u file offset: %I64u (starting in block: %I64u)\n",
                    p->ulThreadNo,
                    pTarget->GetPath().c_str(),
                    p->ulRelativeThreadNo,
                    startingFileOffset,
                    startingFileOffset / pTarget->GetBlockSizeInBytes());
            }
        }

        // allocate memory for a data buffer
        if (!p->AllocateAndFillBufferForTarget(*pTarget))
        {
            PrintError("FATAL ERROR: Could not allocate a buffer bytes for target '%s'. Error code: 0x%x\n", pTarget->GetPath().c_str(), GetLastError());
            fOk = false;
            goto cleanup;
        }
        iTarget++;
    }
 
    // TODO: copy parameters for better memory locality?    
    // TODO: tell the main thread we're ready
    // TODO: wait for a signal to start

    printfv(p->pProfile->GetVerbose(), "thread %u started (random seed: %u)\n", p->ulThreadNo, p->ulRandSeed);
    
    // TODO: check if it's still used
    LARGE_INTEGER li;        //used for setting file positions, etc.
    DWORD dwIOCnt = 0;        //number of completed I/O operations since last progress dot

    p->vullPrivateSequentialOffsets.clear();
    p->vullPrivateSequentialOffsets.resize(p->vTargets.size());
    p->pResults->vTargetResults.clear();
    p->pResults->vTargetResults.resize(p->vTargets.size());
    for (size_t i = 0; i < p->vullFileSizes.size(); i++)
    {
        p->pResults->vTargetResults[i].sPath = p->vTargets[i].GetPath();
        p->pResults->vTargetResults[i].ullFileSize = p->vullFileSizes[i];
        if(fCalculateIopsStdDev) 
        {
            p->pResults->vTargetResults[i].readBucketizer.Initialize(ioBucketDuration, expectedNumberOfBuckets);
            p->pResults->vTargetResults[i].writeBucketizer.Initialize(ioBucketDuration, expectedNumberOfBuckets);
        }
    }

    //
    // synchronous access
    //
    //FUTURE EXTENSION: enable asynchronous I/O even if only 1 outstanding I/O per file (requires another parameter)

    if (p->vTargets.size() == 1 && p->vTargets[0].GetRequestCount() == 1)
    {
        Target *pTarget = &p->vTargets[0];
        DWORD dwBytesTransferred = 0;

        //advance file pointer to base file offset
        li.QuadPart = IORequestGenerator::GetStartingFileOffset(*p, 0);
        printfv(p->pProfile->GetVerbose(), "t[%u] initial I/O op at %I64u (starting in block: %I64u)\n", 
            p->ulThreadNo, 
            li.QuadPart,
            li.QuadPart / pTarget->GetBlockSizeInBytes());
        //FUTURE EXTENSION: file pointer should be set through OVERLAPPED stucture for consistency with other scenarios (unless this is suspected to be the common way in real scenarios)
        if (!SetFilePointerEx(p->vhTargets[0], li, NULL, FILE_BEGIN))
        {
            PrintError("Error setting file pointer. Error code: %d.\n", GetLastError());
            fOk = false;
            goto cleanup;
        }

        BOOL rslt = FALSE;

        assert(nullptr != p->hStartEvent);

        //wait for a signal to start
        printfv(p->pProfile->GetVerbose(), "thread %u: waiting for a signal to start\n", p->ulThreadNo);
        if (WAIT_FAILED == WaitForSingleObject(p->hStartEvent, INFINITE))
        {
            PrintError("Waiting for a signal to start failed (error code: %u)\n", GetLastError());
            fOk = false;
            goto cleanup;
        }
        printfv(p->pProfile->GetVerbose(), "thread %u: received signal to start\n", p->ulThreadNo);

        // TODO: check if this is needed
        //check if everything is ok
        if (g_bError)
        {
            fOk = false;
            goto cleanup;
        }

        //
        // perform work
        //

        assert(nullptr != p->vhTargets[0] );
        assert(pTarget->GetBlockSizeInBytes() > 0);

        ThroughputMeter throughputMeter;
        DWORD dwSleepTime;

        DWORD dwBurstSize = pTarget->GetBurstSize();
        if (p->pTimeSpan->GetThreadCount() > 0)
        {
            dwBurstSize /= p->pTimeSpan->GetThreadCount();
        }
        else
        {
            dwBurstSize /= pTarget->GetThreadsPerFile();
        }
        throughputMeter.Start(pTarget->GetThroughputInBytesPerMillisecond(), pTarget->GetBlockSizeInBytes(), pTarget->GetThinkTime(), dwBurstSize);

        while(g_bRun && !g_bThreadError)
        {
            if (throughputMeter.IsRunning())
            {
                dwSleepTime = throughputMeter.GetSleepTime();
                if (0 != dwSleepTime)
                {
                    Sleep(dwSleepTime);
                    continue;
                }
            }

            //start read or write operation (depends of the type of test)
            //first access is always performed on base offset (even in case of random access)

            UINT64 ullStartTime = 0;

            if (fMeasureLatency)
            {
                ullStartTime = PerfTimer::GetTime(); // record IO start time 
            }

            IOOperation readOrWrite;
            readOrWrite = DecideIo(pTarget->GetWriteRatio());
            if (readOrWrite == IOOperation::ReadIO) 
            {
                rslt = ReadFile(p->vhTargets[0], p->GetReadBuffer(0, 0), pTarget->GetBlockSizeInBytes(), &dwBytesTransferred, nullptr);
            }
            else
            {
                rslt = WriteFile(p->vhTargets[0], p->GetWriteBuffer(0, 0), pTarget->GetBlockSizeInBytes(), &dwBytesTransferred, nullptr);
            }

            if (!rslt)
            {
                PrintError("t[%u:%u] error during %s error code: %u)\n", p->ulThreadNo, 0, (readOrWrite == IOOperation::ReadIO ? "read" : "write"), GetLastError()); 
                fOk = false;
                goto cleanup;
            }

            //check if I/O operation transferred requested number of bytes
            if (dwBytesTransferred != pTarget->GetBlockSizeInBytes())
            {
                PrintError("Warning: thread %u transfered %u bytes instead of %u bytes\n", p->ulThreadNo, dwBytesTransferred, pTarget->GetBlockSizeInBytes());
            }

            if (throughputMeter.IsRunning())
            {
                throughputMeter.Adjust(pTarget->GetBlockSizeInBytes());
            }

            if (*p->pfAccountingOn)
            {
                p->pResults->vTargetResults[0].Add(dwBytesTransferred,
                    readOrWrite,
                    &ullStartTime,
                    p->pullStartTime,
                    fMeasureLatency,
                    fCalculateIopsStdDev);
            }

            // check if we should print a progress dot
            if (0 != p->pProfile->GetProgress() > 0)
            {
                ++dwIOCnt;
                if (dwIOCnt == p->pProfile->GetProgress())
                {
                    print(".");
                    dwIOCnt = 0;
                }
            }

            li.QuadPart = IORequestGenerator::GetNextFileOffset(*p, 0, li.QuadPart);

            printfv(p->pProfile->GetVerbose(), "t[%u] new I/O op at %I64u (starting in block: %I64u)\n",
                    p->ulThreadNo, 
                    li.QuadPart,
                    li.QuadPart / pTarget->GetBlockSizeInBytes());

            if (!SetFilePointerEx(p->vhTargets[0], li, NULL, FILE_BEGIN))
            {
                PrintError("thread %u: Error setting file pointer\n", p->ulThreadNo);
                fOk = false;
                goto cleanup;
            }

            assert(!g_bError);  // at this point we shouldn't be seeing initialization error
        }
    }//end of synchronous access
    //
    // overlapped IO operations
    //
    else
    {
        //
        // create IO completion port
        //
        for (unsigned int i = 0; i < p->vTargets.size(); i++)
        {
            if (!p->pTimeSpan->GetCompletionRoutines())
            {
                hCompletionPort = CreateIoCompletionPort(p->vhTargets[i], hCompletionPort, 0, 1);
                if (nullptr == hCompletionPort)
                {
                    PrintError("unable to create IO completion port (error code: %u)\n", GetLastError());
                    fOk = false;
                    goto cleanup;
                }
            }
        }

        //
        // fill the OVERLAPPED structures
        //
        
        UINT32 cOverlapped = p->GetTotalRequestCount();
        
        p->vOverlapped.clear();
        p->vOverlapped.resize(cOverlapped);

        p->vdwIoType.clear();
        p->vdwIoType.resize(cOverlapped);
    
        p->vIoStartTimes.clear();
        p->vIoStartTimes.resize(cOverlapped);

        p->vFirstOverlappedIdForTargetId.clear();
        
        UINT32 iOverlapped = 0;
        for (unsigned int iFile = 0; iFile < p->vTargets.size(); iFile++)
        {
            Target *pTarget = &p->vTargets[iFile];
            
            li.QuadPart = IORequestGenerator::GetStartingFileOffset(*p, iFile);
            p->vFirstOverlappedIdForTargetId.push_back(iOverlapped);

            for (DWORD iRequest = 0; iRequest < pTarget->GetRequestCount(); ++iRequest)
            {
                // on increment, get next except in the case of parallel async, which all start at the initial offset.
                // note that we must only do this when needed, since it will advance global state.
                if (iRequest != 0 && !pTarget->GetUseParallelAsyncIO())
                {
                    li.QuadPart = IORequestGenerator::GetNextFileOffset(*p, iFile, li.QuadPart);
                }

                p->vOverlappedIdToTargetId.push_back(iFile);
                if (!p->pTimeSpan->GetCompletionRoutines())
                {
                    p->vOverlapped[iOverlapped].hEvent = nullptr;    //we don't need event, because we use IO completion port
                }
                else
                {
                    //in case of completion routines hEvent field is not used,
                    //so we can use it to pass a pointer to the thread parameters
                    p->vOverlapped[iOverlapped].hEvent = (HANDLE)p;
                }

                printfv(p->pProfile->GetVerbose(), "t[%u:%u] initial I/O op at %I64u (starting in block: %I64u)\n",
                    p->ulThreadNo,
                    iFile,
                    li.QuadPart,
                    li.QuadPart / pTarget->GetBlockSizeInBytes());

                p->vOverlapped[iOverlapped].Offset = li.LowPart;
                p->vOverlapped[iOverlapped].OffsetHigh = li.HighPart;

                ++iOverlapped;
            }
        }

        //
        // wait for a signal to start
        //
        printfv(p->pProfile->GetVerbose(), "thread %u: waiting for a signal to start\n", p->ulThreadNo);
        if( WAIT_FAILED == WaitForSingleObject(p->hStartEvent, INFINITE) )
        {
            PrintError("Waiting for a signal to start failed (error code: %u)\n", GetLastError());
            fOk = false;
            goto cleanup;
        }
        printfv(p->pProfile->GetVerbose(), "thread %u: received signal to start\n", p->ulThreadNo);

        //check if everything is ok
        if (g_bError)
        {
            fOk = false;
            goto cleanup;
        }

        //error handling and memory freeing is done in doWorkUsingIOCompletionPorts and doWorkUsingCompletionRoutines
        if (!p->pTimeSpan->GetCompletionRoutines())
        {
            // use IO Completion Ports (it will also close the I/O completion port)
            if (!doWorkUsingIOCompletionPorts(p, hCompletionPort))
            {
                fOk = false;
                goto cleanup;
            }
        }
        else
        {
            //use completion routines
            if (!doWorkUsingCompletionRoutines(p))
            {
                fOk = false;
                goto cleanup;
            }
        }

        assert(!g_bError);  // at this point we shouldn't be seeing initialization error
    } // end of overlapped IO operations

    // save results

cleanup:
    if (!fOk)
    {
        g_bThreadError = TRUE;
    }

    // free memory allocated with VirtualAlloc
    for (auto i = p->vpDataBuffers.begin(); i != p->vpDataBuffers.end(); i++)
    {
        if (nullptr != *i)
        {
#pragma prefast(suppress:6001, "Prefast does not understand this vector will only contain validly allocated buffer pointers")
            VirtualFree(*i, 0, MEM_RELEASE);
        }
    }

    // close files
    for (auto i = p->vhTargets.begin(); i != p->vhTargets.end(); i++)
    {
        CloseHandle(*i);
    }

    // close completion ports
    if (hCompletionPort != nullptr)
    {
        CloseHandle(hCompletionPort);
    }

    delete p;

    // notify master thread that we've finished
    InterlockedDecrement(&g_lRunningThreadsCount);

    return fOk ? 1 : 0;
}

/*****************************************************************************/
struct ETWSessionInfo IORequestGenerator::_GetResultETWSession(const EVENT_TRACE_PROPERTIES *pTraceProperties) const
{
    struct ETWSessionInfo session = {};
    if (nullptr != pTraceProperties)
    {
        session.lAgeLimit = pTraceProperties->AgeLimit;
        session.ulBufferSize = pTraceProperties->BufferSize;
        session.ulBuffersWritten = pTraceProperties->BuffersWritten;
        session.ulEventsLost = pTraceProperties->EventsLost;
        session.ulFlushTimer = pTraceProperties->FlushTimer;
        session.ulFreeBuffers = pTraceProperties->FreeBuffers;
        session.ulLogBuffersLost = pTraceProperties->LogBuffersLost;
        session.ulMaximumBuffers = pTraceProperties->MaximumBuffers;
        session.ulMinimumBuffers = pTraceProperties->MinimumBuffers;
        session.ulNumberOfBuffers = pTraceProperties->NumberOfBuffers;
        session.ulRealTimeBuffersLost = pTraceProperties->RealTimeBuffersLost;
    }
    return session;
}

DWORD IORequestGenerator::_CreateDirectoryPath(const char *pszPath) const
{
    char *c = nullptr;          //variable used to browse the path
    char dirPath[MAX_PATH];  //copy of the path (it will be altered)

    //only support absolute paths that specify the drive letter
    if (pszPath[0] == '\0' || pszPath[1] != ':')
    {
        return ERROR_NOT_SUPPORTED;
    }
    
    if (strcpy_s(dirPath, _countof(dirPath), pszPath) != 0)
    {
        return ERROR_BUFFER_OVERFLOW;
    }
    
    c = dirPath;
    while('\0' != *c)
    {
        if ('\\' == *c)
        {
            //skip the first one as it will be the drive name
            if (c-dirPath >= 3)
            {
                *c = '\0';
                //create directory if it doesn't exist
                if (GetFileAttributes(dirPath) == INVALID_FILE_ATTRIBUTES)
                {
                    if (CreateDirectory(dirPath, NULL) == FALSE)
                    {
                        return GetLastError();
                    }
                }
                *c = L'\\';
            }
        }
        
        c++;
    }

    return ERROR_SUCCESS;
}

/*****************************************************************************/
// create a file of the given size
//
bool IORequestGenerator::_CreateFile(UINT64 ullFileSize, const char *pszFilename, bool fZeroBuffers, bool fVerbose) const
{
    bool fSlowWrites = false;
    printfv(fVerbose, "Creating file '%s' of size %I64u.\n", pszFilename, ullFileSize);

    //enable SE_MANAGE_VOLUME_NAME privilege, required to set valid size of a file
    if (!SetPrivilege(SE_MANAGE_VOLUME_NAME))
    {
        PrintError("WARNING: Could not set privileges for setting valid file size; will use a slower method of preparing the file\n", GetLastError());
        fSlowWrites = true;
    }

    // there are various forms of paths we do not support creating subdir hierarchies
    // for - relative and unc paths specifically. this is fine, and not neccesary to
    // warn about. we can add support in the future.
    DWORD dwError = _CreateDirectoryPath(pszFilename);
    if (dwError != ERROR_SUCCESS && dwError != ERROR_NOT_SUPPORTED)
    {
        PrintError("WARNING: Could not create intermediate directory (error code: %u)\n", dwError);
    }

    //create handle to the file
    HANDLE hFile = CreateFile(pszFilename,
                              GENERIC_WRITE,
                              FILE_SHARE_WRITE,
                              NULL,                        //security
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,        //flags
                              NULL);                        //template file
    if (INVALID_HANDLE_VALUE == hFile)
    {
        PrintError("Could not create the file (error code: %u)\n", GetLastError());
        return false;
    }

    if (ullFileSize > 0)
    {
        LARGE_INTEGER li;
        li.QuadPart = ullFileSize;

        LARGE_INTEGER liNewFilePointer;

        if (!SetFilePointerEx(hFile, li, &liNewFilePointer, FILE_BEGIN))
        {
            PrintError("Could not set file pointer during file creation when extending file (error code: %u)\n", GetLastError());
            CloseHandle(hFile);
            return false;
        }
        if (liNewFilePointer.QuadPart != li.QuadPart)
        {
            PrintError("File pointer improperly moved during file creation when extending file\n");
            CloseHandle(hFile);
            return false;
        }

        //extends file (warning! this is a kind of "reservation" of space; valid size of the file is still 0!)
        if (!SetEndOfFile(hFile))
        {
            PrintError("Error setting end of file (error code: %u)\n", GetLastError());
            CloseHandle(hFile);
            return false;
        }
        //try setting valid size of the file (privileges for that are enabled before CreateFile)
        if (!fSlowWrites && !SetFileValidData(hFile, ullFileSize))
        {
            PrintError("WARNING: Could not set valid file size (error code: %u); trying a slower method of filling the file"
                       " (this does not affect performance, just makes the test preparation longer)\n",
                       GetLastError());
            fSlowWrites = true;
        }

        //if setting valid size couldn't be performed, fill in the file by simply writing to it (slower)
        if (fSlowWrites)
        {
            li.QuadPart = 0;
            if (!SetFilePointerEx(hFile, li, &liNewFilePointer, FILE_BEGIN))
            {
                PrintError("Could not set file pointer during file creation (error code: %u)\n", GetLastError());
                CloseHandle(hFile);
                return false;
            }
            if (liNewFilePointer.QuadPart != li.QuadPart)
            {
                PrintError("File pointer improperly moved during file creation\n");
                CloseHandle(hFile);
                return false;
            }

            UINT32 ulBufSize;
            UINT64 ullRemainSize;

            ulBufSize = 1024*1024;
            if (ullFileSize < (UINT64)ulBufSize)
            {
                ulBufSize = (UINT32)ullFileSize;
            }

            vector<BYTE> vBuf(ulBufSize);
            for (UINT32 i=0; i<ulBufSize; ++i)
            {
                vBuf[i] = fZeroBuffers ? 0 : (BYTE)(i&0xFF);
            }

            ullRemainSize = ullFileSize;
            while (ullRemainSize > 0)
            {
                DWORD dwBytesWritten;
                if ((UINT64)ulBufSize > ullRemainSize)
                {
                    ulBufSize = (UINT32)ullRemainSize;
                }
                if (!WriteFile(hFile, &vBuf[0], ulBufSize, &dwBytesWritten, NULL))
                {
                    PrintError("Error while writng during file creation (error code: %u)\n", GetLastError());
                    CloseHandle(hFile);
                    return false;
                }
                if (dwBytesWritten != ulBufSize)
                {
                    PrintError("Improperly written data during file creation\n");
                    CloseHandle(hFile);
                    return false;
                }
                ullRemainSize -= ulBufSize;
            }
        }
    }

    //if compiled with debug support, check file size
#ifndef NDEBUG
    LARGE_INTEGER li;
    if( GetFileSizeEx(hFile, &li) )
    {
        assert(li.QuadPart == (LONGLONG)ullFileSize);
    }
#endif

    CloseHandle(hFile);

    return TRUE;
}

/*****************************************************************************/
void IORequestGenerator::_TerminateWorkerThreads(vector<HANDLE>& vhThreads) const
{
    for (UINT32 x = 0; x < vhThreads.size(); ++x)
    {
        assert(NULL != vhThreads[x]);
#pragma warning( push )
#pragma warning( disable : 6258 )
        if (!TerminateThread(vhThreads[x], 0))
        {
            PrintError("Warning: unable to terminate worker thread %u\n", x);
        }
#pragma warning( pop )
    }
}
/*****************************************************************************/
void IORequestGenerator::_AbortWorkerThreads(HANDLE hStartEvent, vector<HANDLE>& vhThreads) const
{
    assert(NULL != hStartEvent);

    if (NULL == hStartEvent)
    {
        return;
    }

    g_bError = TRUE;
    if (!SetEvent(hStartEvent))
    {
        PrintError("Error signaling start event\n");
        _TerminateWorkerThreads(vhThreads);
    }
    else
    {
        //FUTURE EXTENSION: maximal timeout may be added here (and below)
        while (g_lRunningThreadsCount > 0)
        {
            Sleep(100);
        }
    }
}

/*****************************************************************************/
bool IORequestGenerator::_StopETW(bool fUseETW, TRACEHANDLE hTraceSession) const
{
    bool fOk = true;
    if (fUseETW)
    {
        PEVENT_TRACE_PROPERTIES pETWSession = StopETWSession(hTraceSession);
        if (nullptr == pETWSession)
        {
            PrintError("Error stopping ETW session\n");
            fOk = false;
        }
        else
        {
            free(pETWSession);
        }
    }
    return fOk;
}

/*****************************************************************************/
// initializes all global parameters
//
void IORequestGenerator::_InitializeGlobalParameters()
{
//    g_vThreadResults.clear(); // TODO: remove
    g_lRunningThreadsCount = 0;     //number of currently running worker threads
    g_ulProcCount = 0;              //number of CPUs present in the system
    g_bRun = TRUE;                  //used for letting threads know that they should stop working

    g_bThreadError = FALSE;         //true means that an error has occured in one of the threads
    g_bTracing = FALSE;             //true means that ETW is turned on

    _hNTDLL = nullptr;              //handle to ntdll.dll
    g_bError = FALSE;               //true means there was fatal error during intialization and threads shouldn't perform their work

    g_pActiveGroupsAndProcs = NULL; // structure with KGroup info
}

bool IORequestGenerator::_PrecreateFiles(Profile& profile) const
{
    bool fOk = true;
    if (profile.GetPrecreateFiles() != PrecreateFiles::None)
    {
        vector<CreateFileParameters> vFilesToCreate = _GetFilesToPrecreate(profile);
        vector<string> vCreatedFiles;
        for (auto file : vFilesToCreate)
        {
            fOk = _CreateFile(file.ullFileSize, file.sPath.c_str(), file.fZeroWriteBuffers, profile.GetVerbose());
            if (!fOk)
            {
                break;
            }
            vCreatedFiles.push_back(file.sPath);
        }

        if (fOk)
        {
            profile.MarkFilesAsPrecreated(vCreatedFiles);
        }
    }
    return fOk;
}

bool IORequestGenerator::GenerateRequests(Profile& profile, IResultParser& resultParser, PRINTF pPrintOut, PRINTF pPrintError, PRINTF pPrintVerbose, struct Synchronization *pSynch, int *totalScore)
{
    g_pfnPrintOut = pPrintOut;
    g_pfnPrintError = pPrintError;
    g_pfnPrintVerbose = pPrintVerbose;

    bool fOk = _PrecreateFiles(profile);
    if (fOk)
    {
        const vector<TimeSpan>& vTimeSpans = profile.GetTimeSpans();
        vector<Results> vResults(vTimeSpans.size());
        for (size_t i = 0; fOk && (i < vTimeSpans.size()); i++)
        {
            printfv(profile.GetVerbose(), "Generating requests for timespan %u.\n", i + 1);
            fOk = _GenerateRequestsForTimeSpan(profile, vTimeSpans[i], vResults[i], pSynch);
        }

        // TODO: show results only for timespans that succeeded
        SystemInformation system;
        string sResults = resultParser.ParseResults(profile, system, vResults);
        print("%s", sResults.c_str());
		*totalScore = resultParser.GetTotalScore() * 10;
    }

    return fOk;
}

bool IORequestGenerator::_GenerateRequestsForTimeSpan(const Profile& profile, const TimeSpan& timeSpan, Results& results, struct Synchronization *pSynch)
{
    //FUTURE EXTENSION: add new I/O capabilities presented in Longhorn
    //FUTURE EXTENSION: add a check if the folder is compressed (cache is always enabled in case of compressed folders)

    //check if I/O request generator is already running
    LONG lGenState = InterlockedExchange(&g_lGeneratorRunning, 1);
    if (1 == lGenState)
    {
        PrintError("FATAL ERROR: I/O Request Generator already running\n");
        return false;
    }

    //initialize all global parameters (in case of second run, after the first one is finished)
    _InitializeGlobalParameters();

    HANDLE hStartEvent = nullptr;                       // start event (used to inform the worker threads that they should start the work)
    HANDLE hEndEvent = nullptr;                         // end event (used only in case of completin routines (not for IO Completion Ports))

    memset(&g_EtwEventCounters, 0, sizeof(struct ETWEventCounters));  // reset all etw event counters

    //ulProcCount = getProcessorCount();
    g_pActiveGroupsAndProcs = (PACTIVE_GROUPS_AND_PROCS)malloc(sizeof(ACTIVE_GROUPS_AND_PROCS));
    if (g_pActiveGroupsAndProcs == NULL)
    {
        PrintError("ERROR: Memory allocation for groups and procs structure failed!\r\n");
        return false;
    }
    if (_GetActiveGroupsAndProcs() == false)
    {
        PrintError("ERROR: Failed to get groups and processors information!\r\n");
        return false;
    }

    bool fUseETW = false;            //true if user wants ETW

    //
    // load dlls
    //
    assert(nullptr == _hNTDLL);
    if (!_LoadDLLs())
    {
        PrintError("Error loading NtQuerySystemInformation\n");
        return false;
    }

    //FUTURE EXTENSION: check for conflicts in alignment (when cache is turned off only sector aligned I/O are permitted)
    //FUTURE EXTENSION: check if file sizes are enough to have at least first requests not wrapping around
    
    vector<Target> vTargets = timeSpan.GetTargets();
    // allocate memory for random data write buffers
    for (auto i = vTargets.begin(); i != vTargets.end(); i++)
    {
        if ((i->GetRandomDataWriteBufferSize() > 0) && !i->AllocateAndFillRandomDataWriteBuffer())
        {
            return false;
        }
    }

    // check if user wanted to create a file
    for (auto i = vTargets.begin(); i != vTargets.end(); i++)
    {
        if ((i->GetFileSize() > 0) && (i->GetPrecreated() == false))
        {
            string str = i->GetPath();
            const char *filename = str.c_str();
            if (NULL == filename || NULL == *(filename))
            {
                PrintError("You have to provide a filename\n");
                return false;
            }

            //skip physical drives and partitions
            if ('#' == filename[0] || (':' == filename[1] && 0 == filename[2]))
            {
                continue;
            }

            //create only regular files
            if (!_CreateFile(i->GetFileSize(), filename, i->GetZeroWriteBuffers(), profile.GetVerbose()))
            {
                return false;
            }
        }
    }

    // get thread count
    UINT32 cThreads = timeSpan.GetThreadCount();
    if (cThreads < 1)
    {
        for (auto i = vTargets.begin(); i != vTargets.end(); i++)
        {
            cThreads += i->GetThreadsPerFile();
        }
    }

    // allocate memory for thread handles
    vector<HANDLE> vhThreads(cThreads);

    //
    // allocate memory for performance counters
    //
    vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> vPerfInit(g_ulProcCount);
    vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> vPerfDone(g_ulProcCount);
    vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> vPerfDiff(g_ulProcCount);

    //
    //create start event
    //

    hStartEvent = CreateEvent(NULL, TRUE, FALSE, "");
    if (NULL == hStartEvent)
    {
        PrintError("Error creating the start event\n");
        return false;
    }

    //
    // create end event
    //
    if (timeSpan.GetCompletionRoutines())
    {
        hEndEvent = CreateEvent(NULL, TRUE, FALSE, "");
        if (NULL == hEndEvent)
        {
            PrintError("Error creating the end event\n");
            return false;
        }
    }

    //
    // create the threads
    //

    g_bRun = TRUE;
    WORD wGroupCtr = 0;
    DWORD dwProcCtr = 0;

    volatile bool fAccountingOn = false;
    UINT64 ullStartTime;    //start time
    UINT64 ullTimeDiff;  //elapsed test time (in units returned by QueryPerformanceCounter)
    vector<UINT64> vullSharedSequentialOffsets(vTargets.size(), 0);

    results.vThreadResults.clear();
    results.vThreadResults.resize(cThreads);
    for (UINT32 iThread = 0; iThread < cThreads; ++iThread)
    {
        printfv(profile.GetVerbose(), "creating thread %u\n", iThread);
        ThreadParameters *cookie = new ThreadParameters();  // threadFunc is going to free the memory
        if (nullptr == cookie)
        {
            PrintError("FATAL ERROR: could not allocate memory\n");
            _AbortWorkerThreads(hStartEvent, vhThreads);
            return false;
        }

        UINT32 ulRelativeThreadNo = 0;

        if (timeSpan.GetThreadCount() > 0)
        {
            // fixed thread mode: all threads operate on all files
            // and receive the entire seq index array.
            // relative thread number is the same as thread number.
            cookie->vTargets = vTargets;
            cookie->pullSharedSequentialOffsets = &vullSharedSequentialOffsets[0];
            ulRelativeThreadNo = iThread;
        }
        else
        {
            size_t cAssignedThreads = 0;
            size_t cBaseThread = 0;
            auto psi = vullSharedSequentialOffsets.begin();
            for (auto i = vTargets.begin();
                 i != vTargets.end();
                 i++, psi++)
            {
                // per-file thread mode: groups of threads operate on individual files
                // and receive the specific seq index for their file (note: singular).
                // loop up through the targets to assign thread n to the appropriate file.
                // relative thread number is file-relative, so keep track of the base
                // thread number for the file and calculate relative to that.
                //
                // ex: two files, two threads per file
                //  t0: rt0 for f0 (cAssigned = 2, cBase = 0)
                //  t1: rt1 for f0 (cAssigned = 2, cBase = 0)
                //  t2: rt0 for f1 (cAssigned = 4, cBase = 2)
                //  t3: rt1 for f1 (cAssigned = 4, cBase = 2)

                cAssignedThreads += i->GetThreadsPerFile();
                if (iThread < cAssignedThreads)
                {
                    cookie->vTargets.push_back(*i);
                    cookie->pullSharedSequentialOffsets = &(*psi);
                    ulRelativeThreadNo = (iThread - cBaseThread) % i->GetThreadsPerFile();

                    printfv(profile.GetVerbose(), "thread %u is relative thread %u for %s\n", iThread, ulRelativeThreadNo, i->GetPath().c_str());
                    break;
                }
                cBaseThread += i->GetThreadsPerFile();
            }
        }

        cookie->pProfile = &profile;
        cookie->pTimeSpan = &timeSpan;
        cookie->hStartEvent = hStartEvent;
        cookie->hEndEvent = hEndEvent;
        cookie->ulThreadNo = iThread;
        cookie->ulRelativeThreadNo = ulRelativeThreadNo;
        cookie->pfAccountingOn = &fAccountingOn;
        cookie->pullStartTime = &ullStartTime;
        cookie->ulRandSeed = timeSpan.GetRandSeed() + iThread;  // each thread has a different random seed

        //Set thread group and proc affinity
        if (timeSpan.GetGroupAffinity())
        {
            cookie->wGroupNum = wGroupCtr;
            cookie->dwProcNum = dwProcCtr;

            if (dwProcCtr == g_pActiveGroupsAndProcs->dwaActiveProcsCount[wGroupCtr] - 1)
            {
                dwProcCtr = 0;
                if (wGroupCtr == g_pActiveGroupsAndProcs->wActiveGroupCount - 1)
                {
                    wGroupCtr = 0;
                }
                else
                {
                    wGroupCtr++;
                }
            }
            else
            {
                dwProcCtr++;
            }
        }

        //create thread
        cookie->pResults = &results.vThreadResults[iThread];

        InterlockedIncrement(&g_lRunningThreadsCount);
        DWORD dwThreadId;
        HANDLE hThread = CreateThread(NULL, 64 * 1024, threadFunc, cookie, 0, &dwThreadId);
        if (NULL == hThread)
        {
            //in case of error terminate running worker threads
            PrintError("ERROR: unable to create thread (error code: %u)\n", GetLastError());
            InterlockedDecrement(&g_lRunningThreadsCount);
            _AbortWorkerThreads(hStartEvent, vhThreads);
            delete cookie;
            return false;
        }

        //store handle to the thread
        vhThreads[iThread] = hThread;
    }
    //
    // affinitize thread to first cpu
    // (otherwise, because of bios bugs, RDTSC readings may be not accurate)
    //
    SetThreadAffinityMask(GetCurrentThread(), 1);    //FUTURE EXTENSION: check if it is set correctly, on the end/error set the affinity (and priority) to original

    //FUTURE EXTENSION: SetPriorityClass HIGH/ABOVE_NORMAL
    //FUTURE EXTENSION: lower priority so the worker threads will initialize (-2)
    //FUTURE EXTENSION: raise priority so this thread will run after the time end

    if (STRUCT_SYNCHRONIZATION_SUPPORTS(pSynch, hStartEvent) && (NULL != pSynch->hStartEvent))
    {
        if (WAIT_OBJECT_0 != WaitForSingleObject(pSynch->hStartEvent, INFINITE))
        {
            PrintError("Error during WaitForSingleObject\n");
            _AbortWorkerThreads(hStartEvent, vhThreads);
            return false;
        }
    }

    //
    // get cycle count (it will be used to calculate actual work time)
    //
    DWORD dwWaitStatus = 0;

    //bAccountingOn = FALSE; // clear the accouning flag so that threads didn't count what they do while in the warmup phase

    BOOL bSynchStop = STRUCT_SYNCHRONIZATION_SUPPORTS(pSynch, hStopEvent) && (NULL != pSynch->hStopEvent);
    BOOL bBreak = FALSE;
    PEVENT_TRACE_PROPERTIES pETWSession = NULL;

    printfv(profile.GetVerbose(), "starting warm up...\n");
    //
    // send start signal
    //
    if (!SetEvent(hStartEvent))
    {
        PrintError("Error signaling start event\n");
        //        stopETW(bUseETW, hTraceSession);
        _TerminateWorkerThreads(vhThreads);    //FUTURE EXTENSION: timeout for worker threads
        return false;
    }

    //
    // wait specified amount of time in each phase (warm up, test, cool down)
    //
    if (timeSpan.GetWarmup() > 0)
    {
        if (bSynchStop)
        {
            assert(NULL != pSynch->hStopEvent);
            dwWaitStatus = WaitForSingleObject(pSynch->hStopEvent, 1000 * timeSpan.GetWarmup());
            if (WAIT_OBJECT_0 != dwWaitStatus && WAIT_TIMEOUT != dwWaitStatus)
            {
                PrintError("Error during WaitForSingleObject\n");
                _TerminateWorkerThreads(vhThreads);
                return false;
            }
            bBreak = (WAIT_TIMEOUT != dwWaitStatus);
        }
        else
        {
            Sleep(1000 * timeSpan.GetWarmup());
        }
    }

    if (!bBreak) // proceed only if user didn't break the test
    {
        //FUTURE EXTENSION: starting ETW session shouldn't be done brutally here, should be done before warmup and here just a fast signal to start logging (see also stopping ETW session)
        //FUTURE EXTENSION: put an ETW mark here, for easier parsing by external tools

        //
        // start etw session
        //
        TRACEHANDLE hTraceSession = NULL;
        if (fUseETW)
        {
            printfv(profile.GetVerbose(), "starting trace session\n");
            hTraceSession = StartETWSession(profile);
            if (NULL == hTraceSession)
            {
                PrintError("Could not start ETW session\n");
                _TerminateWorkerThreads(vhThreads);
                return false;
            }

            if (NULL == CreateThread(NULL, 64 * 1024, etwThreadFunc, NULL, 0, NULL))
            {
                PrintError("Warning: unable to create thread for ETW session\n");
                _TerminateWorkerThreads(vhThreads);
                return false;
            }
            printfv(profile.GetVerbose(), "tracing events\n");
        }

        //
        // read performance counters
        //
        if (_GetSystemPerfInfo(&vPerfInit[0], g_ulProcCount) == FALSE)
        {
            PrintError("Error reading performance counters\n");
            _StopETW(fUseETW, hTraceSession);
            _TerminateWorkerThreads(vhThreads);
            return false;
        }

        printfv(profile.GetVerbose(), "starting measurements...\n");
        //get cycle count (it will be used to calculate actual work time)

        //
        // notify the front-end that the test is about to start;
        // do it before starting timing in order not to perturb measurements
        //
        if (STRUCT_SYNCHRONIZATION_SUPPORTS(pSynch, pfnCallbackTestStarted) && (NULL != pSynch->pfnCallbackTestStarted))
        {
            pSynch->pfnCallbackTestStarted();
        }

        ullStartTime = PerfTimer::GetTime();

#pragma warning( push )
#pragma warning( disable : 28931 )
        fAccountingOn = true;
#pragma warning( pop )

        assert(timeSpan.GetDuration() > 0);
        if (bSynchStop)
        {
            assert(NULL != pSynch->hStopEvent);
            dwWaitStatus = WaitForSingleObject(pSynch->hStopEvent, 1000 * timeSpan.GetDuration());
            if (WAIT_OBJECT_0 != dwWaitStatus && WAIT_TIMEOUT != dwWaitStatus)
            {
                PrintError("Error during WaitForSingleObject\n");
                _StopETW(fUseETW, hTraceSession);
                _TerminateWorkerThreads(vhThreads);    //FUTURE EXTENSION: worker threads should have a chance to free allocated memory (see also other places calling terminateWorkerThreads())
                return FALSE;
            }
            bBreak = (WAIT_TIMEOUT != dwWaitStatus);
        }
        else
        {
            Sleep(1000 * timeSpan.GetDuration());
        }

        fAccountingOn = false;

        //get cycle count and perf counters
        ullTimeDiff = PerfTimer::GetTime() - ullStartTime;

        //
        // notify the front-end that the test has just finished;
        // do it after stopping timing in order not to perturb measurements
        //
        if (STRUCT_SYNCHRONIZATION_SUPPORTS(pSynch, pfnCallbackTestFinished) && (NULL != pSynch->pfnCallbackTestFinished))
        {
            pSynch->pfnCallbackTestFinished();
        }

        if (_GetSystemPerfInfo(&vPerfDone[0], g_ulProcCount) == FALSE)
        {
            PrintError("Error getting performance counters\n");
            _StopETW(fUseETW, hTraceSession);
            _TerminateWorkerThreads(vhThreads);
            return false;
        }

        //
        // stop etw session
        //
        if (fUseETW)
        {
            printfv(profile.GetVerbose(), "stopping ETW session\n");
            pETWSession = StopETWSession(hTraceSession);
            if (NULL == pETWSession)
            {
                PrintError("Error stopping ETW session\n");
                return false;
            }
            else
            {
                free(pETWSession);
            }
        }
    }
    else
    {
        ullTimeDiff = 0; // mark that no test was run
    }

    printfv(profile.GetVerbose(), "starting cool down...\n");
    if ((timeSpan.GetCooldown() > 0) && !bBreak)
    {
        if (bSynchStop)
        {
            assert(NULL != pSynch->hStopEvent);
            dwWaitStatus = WaitForSingleObject(pSynch->hStopEvent, 1000 * timeSpan.GetCooldown());
            if (WAIT_OBJECT_0 != dwWaitStatus && WAIT_TIMEOUT != dwWaitStatus)
            {
                PrintError("Error during WaitForSingleObject\n");
                //                stopETW(bUseETW, hTraceSession);
                _TerminateWorkerThreads(vhThreads);
                return false;
            }
        }
        else
        {
            Sleep(1000 * timeSpan.GetCooldown());
        }
    }
    printfv(profile.GetVerbose(), "finished test...\n");

    //
    // signal the threads to finish
    //
    g_bRun = FALSE;
    if (timeSpan.GetCompletionRoutines())
    {
        if (!SetEvent(hEndEvent))
        {
            PrintError("Error signaling end event\n");
            //            stopETW(bUseETW, hTraceSession);
            return false;
        }
    }

    //
    // wait till all of the threads finish
    //
#pragma warning( push )
#pragma warning( disable : 28112 )
    while (g_lRunningThreadsCount > 0)
    {
        Sleep(10);    //FUTURE EXTENSION: a timeout should be implemented
    }
#pragma warning( pop )


    //check if there has been an error during threads execution
    if (g_bThreadError)
    {
        PrintError("There has been an error during threads execution\n");
        return false;
    }

    //
    // close events' handles
    //
    CloseHandle(hStartEvent);
    hStartEvent = NULL;

    if (NULL != hEndEvent)
    {
        CloseHandle(hEndEvent);
        hEndEvent = NULL;
    }
    //FUTURE EXTENSION: hStartEvent and hEndEvent should be closed in case of error too

    //
    // compute time spent by each cpu
    //
    for (unsigned int p = 0; p<g_ulProcCount; ++p)
    {
        assert(vPerfDone[p].IdleTime.QuadPart >= vPerfInit[p].IdleTime.QuadPart);
        assert(vPerfDone[p].KernelTime.QuadPart >= vPerfInit[p].KernelTime.QuadPart);
        assert(vPerfDone[p].UserTime.QuadPart >= vPerfInit[p].UserTime.QuadPart);
        assert(vPerfDone[p].Reserved1[0].QuadPart >= vPerfInit[p].Reserved1[0].QuadPart);
        assert(vPerfDone[p].Reserved1[1].QuadPart >= vPerfInit[p].Reserved1[1].QuadPart);
        assert(vPerfDone[p].Reserved2 >= vPerfInit[p].Reserved2);

        vPerfDiff[p].IdleTime.QuadPart = vPerfDone[p].IdleTime.QuadPart - vPerfInit[p].IdleTime.QuadPart;
        vPerfDiff[p].KernelTime.QuadPart = vPerfDone[p].KernelTime.QuadPart - vPerfInit[p].KernelTime.QuadPart;
        vPerfDiff[p].UserTime.QuadPart = vPerfDone[p].UserTime.QuadPart - vPerfInit[p].UserTime.QuadPart;
        vPerfDiff[p].Reserved1[0].QuadPart = vPerfDone[p].Reserved1[0].QuadPart - vPerfInit[p].Reserved1[0].QuadPart;
        vPerfDiff[p].Reserved1[1].QuadPart = vPerfDone[p].Reserved1[1].QuadPart - vPerfInit[p].Reserved1[1].QuadPart;
        vPerfDiff[p].Reserved2 = vPerfDone[p].Reserved2 - vPerfInit[p].Reserved2;
    }

    //
    // process results and pass them to the result parser
    //

    // get processors perf. info
    results.vSystemProcessorPerfInfo = vPerfDiff;
    results.ullTimeCount = ullTimeDiff;

    //
    // create structure containing etw results and properties
    //
    results.fUseETW = fUseETW;
    if (fUseETW)
    {
        results.EtwEventCounters = g_EtwEventCounters;
        results.EtwSessionInfo = _GetResultETWSession(pETWSession);

        // TODO: refactor to a separate function
        results.EtwMask.bProcess = profile.GetEtwProcess();
        results.EtwMask.bThread = profile.GetEtwThread();
        results.EtwMask.bImageLoad = profile.GetEtwImageLoad();
        results.EtwMask.bDiskIO = profile.GetEtwDiskIO();
        results.EtwMask.bMemoryPageFaults = profile.GetEtwMemoryPageFaults();
        results.EtwMask.bMemoryHardFaults = profile.GetEtwMemoryHardFaults();
        results.EtwMask.bNetwork = profile.GetEtwNetwork();
        results.EtwMask.bRegistry = profile.GetEtwRegistry();
        results.EtwMask.bUsePagedMemory = profile.GetEtwUsePagedMemory();
        results.EtwMask.bUsePerfTimer = profile.GetEtwUsePerfTimer();
        results.EtwMask.bUseSystemTimer = profile.GetEtwUseSystemTimer();
        results.EtwMask.bUseCyclesCounter = profile.GetEtwUseCyclesCounter();
    }

    if (g_pActiveGroupsAndProcs != nullptr)
    {
        free(g_pActiveGroupsAndProcs);
    }

    // free memory used by random data write buffers
    for (auto i = vTargets.begin(); i != vTargets.end(); i++)
    {
        i->FreeRandomDataWriteBuffer();
    }

    // TODO: this won't catch error cases, which exit early
    InterlockedExchange(&g_lGeneratorRunning, 0);
    return true;
}

vector<struct IORequestGenerator::CreateFileParameters> IORequestGenerator::_GetFilesToPrecreate(const Profile& profile) const
{
    vector<struct CreateFileParameters> vFilesToCreate;
    const vector<TimeSpan>& vTimeSpans = profile.GetTimeSpans();
    map<string, vector<struct CreateFileParameters>> filesMap;
    for (const auto& timeSpan : vTimeSpans)
    {
        vector<Target> vTargets(timeSpan.GetTargets());
        for (const auto& target : vTargets)
        {
            struct CreateFileParameters createFileParameters;
            createFileParameters.sPath = target.GetPath();
            createFileParameters.ullFileSize = target.GetFileSize();
            createFileParameters.fZeroWriteBuffers = target.GetZeroWriteBuffers();

            filesMap[createFileParameters.sPath].push_back(createFileParameters);
        }
    }

    PrecreateFiles filter = profile.GetPrecreateFiles();
    for (auto fileMapEntry : filesMap)
    {
        if (fileMapEntry.second.size() > 0)
        {
            UINT64 ullLastNonZeroSize = fileMapEntry.second[0].ullFileSize;
            UINT64 ullMaxSize = fileMapEntry.second[0].ullFileSize;
            bool fLastZeroWriteBuffers = fileMapEntry.second[0].fZeroWriteBuffers;
            bool fHasZeroSizes = false;
            bool fConstantSize = true;
            bool fConstantZeroWriteBuffers = true;
            for (auto file : fileMapEntry.second)
            {
                ullMaxSize = max(ullMaxSize, file.ullFileSize);
                if (ullLastNonZeroSize == 0)
                {
                    ullLastNonZeroSize = file.ullFileSize;
                }
                if (file.ullFileSize == 0)
                {
                    fHasZeroSizes = true;
                }
                if ((file.ullFileSize != 0) && (file.ullFileSize != ullLastNonZeroSize))
                {
                    fConstantSize = false;
                }
                if (file.fZeroWriteBuffers != fLastZeroWriteBuffers)
                {
                    fConstantZeroWriteBuffers = false;
                }
                if (file.ullFileSize != 0)
                {
                    ullLastNonZeroSize = file.ullFileSize;
                }
                fLastZeroWriteBuffers = file.fZeroWriteBuffers;
            }

            if (fConstantZeroWriteBuffers && ullMaxSize > 0)
            {
                struct CreateFileParameters file = fileMapEntry.second[0];
                file.ullFileSize = ullMaxSize;
                if (filter == PrecreateFiles::UseMaxSize)
                {
                    vFilesToCreate.push_back(file);
                }
                else if ((filter == PrecreateFiles::OnlyFilesWithConstantSizes) && fConstantSize && !fHasZeroSizes)
                {
                    vFilesToCreate.push_back(file);
                }
                else if ((filter == PrecreateFiles::OnlyFilesWithConstantOrZeroSizes) && fConstantSize)
                {
                    vFilesToCreate.push_back(file);
                }
            }
        }
    }

    return vFilesToCreate;
}
