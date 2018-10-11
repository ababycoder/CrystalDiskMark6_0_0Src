/*---------------------------------------------------------------------------*/
//       Author : hiyohiyo
//         Mail : hiyohiyo@crystalmark.info
//          Web : http://crystalmark.info/
//      License : The MIT License
//
//                                             Copyright (c) 2007-2015 hiyohiyo
/*---------------------------------------------------------------------------*/

#include "stdafx.h"
#include <afxmt.h>

#include "DiskMark.h"
#include "DiskMarkDlg.h"
#include "DiskBench.h"
#include "GetFileVersion.h"

#include <winioctl.h>
#include <mmsystem.h>
#pragma comment(lib,"winmm.lib")

#pragma warning(disable : 4996)

static CString TestFilePath;
static CString TestFileDir;
static CString DiskSpdExe;

static HANDLE hFile;
static int DiskTestCount;
static UINT64 DiskTestSize;
static void ShowErrorMessage(CString message);
static void Interval(UINT time, LPVOID dlg);

static BOOL Init(LPVOID dlg);
static void DiskSpd(LPVOID dlg, DISK_SPD_CMD cmd);

static UINT Exit(LPVOID dlg);
static void CALLBACK TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
static volatile BOOL WaitFlag;

#define DISK_SPD_EXE_32 L"CdmResource\\diskspd\\diskspd32.exe"
#define DISK_SPD_EXE_64 L"CdmResource\\diskspd\\diskspd64.exe"

int ExecAndWait(TCHAR *pszCmd, BOOL bNoWindow)
{
	DWORD Code;
	BOOL bSuccess;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	memset(&si, 0, sizeof(STARTUPINFO));
	si.cb = sizeof(STARTUPINFO);

	if (bNoWindow) {
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
	}

	bSuccess = CreateProcess(
		NULL,	// lpApplicationName
		pszCmd,	// lpCommandLine
		NULL,	// lpProcessAttributes
		NULL,	// lpThreadAttributes
		FALSE,	// bInheritHandle
		0,		// dwCreationFlag
		NULL,	// lpEnvironment
		NULL,	// lpCurrentDirectory
		&si,	// lpStartupInfo
		&pi		// lpProcessInformation
		);
	if (bSuccess != TRUE) return 0;

	WaitForInputIdle(pi.hProcess, INFINITE);
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &Code);

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	return Code;
}


void ShowErrorMessage(CString message)
{
	DWORD lastErrorCode = GetLastError();
	CString errorMessage;
	LPVOID lpMessageBuffer;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, lastErrorCode, 
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &lpMessageBuffer, 0, NULL);
	errorMessage.Format(_T("0x%08X:%s"), lastErrorCode, lpMessageBuffer);

	AfxMessageBox(message + _T("\r\n") + errorMessage);
	LocalFree( lpMessageBuffer );
}

VOID Interval(LPVOID dlg)
{
	int intervalTime = ((CDiskMarkDlg*) dlg)->m_IntervalTime;
	CString title;

	for (int i = 0; i < intervalTime; i++)
	{
		title.Format(L"Interval Time %d/%d sec", i, intervalTime);
		::PostMessage(((CDiskMarkDlg*) dlg)->GetSafeHwnd(), WM_USER_UPDATE_MESSAGE, (WPARAM) &title, 0);
		Sleep(1000);
		if (!((CDiskMarkDlg*) dlg)->m_DiskBenchStatus)
		{
			return;
		}
	}
}

UINT ExecDiskBenchAll(LPVOID dlg)
{
	if(Init(dlg))
	{
		DiskSpd(dlg, TEST_SEQUENTIAL_READ1);
		Interval(dlg);
#ifdef SEQUENTIAL2
		DiskSpd(dlg, TEST_SEQUENTIAL_READ2);
		Interval(dlg);
#endif
		DiskSpd(dlg, TEST_RANDOM_READ_4KB1);
		Interval(dlg);
		DiskSpd(dlg, TEST_RANDOM_READ_4KB2);
		Interval(dlg);
		DiskSpd(dlg, TEST_RANDOM_READ_4KB3);
		Interval(dlg);
		DiskSpd(dlg, TEST_SEQUENTIAL_WRITE1);
		Interval(dlg);
#ifdef SEQUENTIAL2
		DiskSpd(dlg, TEST_SEQUENTIAL_WRITE2);
		Interval(dlg);
#endif
		DiskSpd(dlg, TEST_RANDOM_WRITE_4KB1);
		Interval(dlg);
		DiskSpd(dlg, TEST_RANDOM_WRITE_4KB2);
		Interval(dlg);
		DiskSpd(dlg, TEST_RANDOM_WRITE_4KB3);
	}

	return Exit(dlg);
}

UINT ExecDiskBenchSequential1(LPVOID dlg)
{
	if(Init(dlg))
	{
		DiskSpd(dlg, TEST_SEQUENTIAL_READ1);
		Interval(dlg);
		DiskSpd(dlg, TEST_SEQUENTIAL_WRITE1);
	}
	return Exit(dlg);
}

#ifdef SEQUENTIAL2
UINT ExecDiskBenchSequential2(LPVOID dlg)
{
	if (Init(dlg))
	{
		DiskSpd(dlg, TEST_SEQUENTIAL_READ2);
		Interval(dlg);
		DiskSpd(dlg, TEST_SEQUENTIAL_WRITE2);
	}
	return Exit(dlg);
}
#endif

UINT ExecDiskBenchRandom4KB1(LPVOID dlg)
{
	if(Init(dlg))
	{
		DiskSpd(dlg, TEST_RANDOM_READ_4KB1);
		Interval(dlg);
		DiskSpd(dlg, TEST_RANDOM_WRITE_4KB1);
	}
	return Exit(dlg);
}
UINT ExecDiskBenchRandom4KB2(LPVOID dlg)
{
	if(Init(dlg))
	{
		DiskSpd(dlg, TEST_RANDOM_READ_4KB2);
		Interval(dlg);
		DiskSpd(dlg, TEST_RANDOM_WRITE_4KB2);
	}
	return Exit(dlg);
}

UINT ExecDiskBenchRandom4KB3(LPVOID dlg)
{
	if(Init(dlg))
	{
		DiskSpd(dlg, TEST_RANDOM_READ_4KB3);
		Interval(dlg);
		DiskSpd(dlg, TEST_RANDOM_WRITE_4KB3);
	}
	return Exit(dlg);
}

BOOL Init(void* dlg)
{
	BOOL FlagArc;
	BOOL result;
	static CString cstr;
	TCHAR drive;

	ULARGE_INTEGER freeBytesAvailableToCaller;
	ULARGE_INTEGER totalNumberOfBytes;
	ULARGE_INTEGER totalNumberOfFreeBytes;

	// Init m_Ini
	TCHAR *ptrEnd;
	TCHAR temp[MAX_PATH];
	::GetModuleFileName(NULL, temp, MAX_PATH);
	if ((ptrEnd = _tcsrchr(temp, '\\')) != NULL)
	{
		*ptrEnd = '\0';
	}

#ifdef _M_X64
	DiskSpdExe.Format(L"%s\\%s", temp, DISK_SPD_EXE_64);
#else
	DiskSpdExe.Format(L"%s\\%s", temp, DISK_SPD_EXE_32);
#endif

	if (! IsFileExist(DiskSpdExe))
	{
		AfxMessageBox(((CDiskMarkDlg*) dlg)->m_MesDiskSpdNotFound);
		((CDiskMarkDlg*) dlg)->m_DiskBenchStatus = FALSE;
		return FALSE;
	}
	DiskTestCount = ((CDiskMarkDlg*) dlg)->m_IndexTestCount + 1;
	DiskTestSize   = (UINT64)_tstoi(((CDiskMarkDlg*)dlg)->m_ValueTestSize);

	CString RootPath;
	if(((CDiskMarkDlg*)dlg)->m_MaxIndexTestDrive != ((CDiskMarkDlg*)dlg)->m_IndexTestDrive)
	{

		drive = ((CDiskMarkDlg*)dlg)->m_ValueTestDrive.GetAt(0);
		cstr.Format(_T("%C:"), drive);
		GetDiskFreeSpaceEx(cstr, &freeBytesAvailableToCaller, &totalNumberOfBytes, &totalNumberOfFreeBytes);
		if (totalNumberOfBytes.QuadPart < ((ULONGLONG)8 * 1024 * 1024 * 1024)) // < 8 GB
		{
			((CDiskMarkDlg*)dlg)->m_TestDriveInfo.Format(_T("%C: %.1f%% (%.1f/%.1f MiB)"), drive,
				(double)(totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart) / (double)totalNumberOfBytes.QuadPart * 100,
				(totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart) / 1024 / 1024.0,
				totalNumberOfBytes.QuadPart / 1024 / 1024.0);
		}
		else
		{
			((CDiskMarkDlg*)dlg)->m_TestDriveInfo.Format(_T("%C: %.1f%% (%.1f/%.1f GiB)"), drive,
				(double)(totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart) / (double)totalNumberOfBytes.QuadPart * 100,
				(totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart) / 1024 / 1024 / 1024.0,
				totalNumberOfBytes.QuadPart / 1024 / 1024 / 1024.0);
		}
		RootPath.Format(_T("%c:\\"), drive);
	}
	else
	{
		RootPath = ((CDiskMarkDlg*)dlg)->m_TestTargetPath;
		RootPath += L"\\";
	}

	TestFileDir.Format(_T("%sCrystalDiskMark%08X"), RootPath, timeGetTime());
	CreateDirectory(TestFileDir, NULL);
	TestFilePath.Format(_T("%s\\CrystalDiskMark%08X.tmp"), TestFileDir, timeGetTime());

	DWORD FileSystemFlags;
	GetVolumeInformation(RootPath, NULL, NULL, NULL, NULL, &FileSystemFlags, NULL, NULL);
	if(FileSystemFlags & FS_VOL_IS_COMPRESSED)
	{
		FlagArc = TRUE;
	}
	else
	{
		FlagArc = FALSE;
	}

// Check Disk Capacity //
	OSVERSIONINFO osVersionInfo;
	osVersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osVersionInfo);

	ULARGE_INTEGER FreeBytesAvailableToCaller, TotalNumberOfBytes, TotalNumberOfFreeBytes;
	GetDiskFreeSpaceEx(RootPath, &FreeBytesAvailableToCaller, &TotalNumberOfBytes, &TotalNumberOfFreeBytes);
	if(DiskTestSize > TotalNumberOfFreeBytes.QuadPart / 1024 / 1024 )
	{
		AfxMessageBox(((CDiskMarkDlg*)dlg)->m_MesDiskCapacityError);
		((CDiskMarkDlg*)dlg)->m_DiskBenchStatus = FALSE;
		return FALSE;
	}

// Preapare Test File
	hFile = ::CreateFile(TestFilePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL|FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN, NULL);

	if(hFile == INVALID_HANDLE_VALUE)
	{
		AfxMessageBox(((CDiskMarkDlg*)dlg)->m_MesDiskCreateFileError);
		((CDiskMarkDlg*)dlg)->m_DiskBenchStatus = FALSE;
		return FALSE;
	}

// COMPRESSION_FORMAT_NONE
	USHORT lpInBuffer = COMPRESSION_FORMAT_NONE;
	DWORD lpBytesReturned = 0;
	DeviceIoControl(hFile, FSCTL_SET_COMPRESSION, (LPVOID) &lpInBuffer,
				sizeof(USHORT), NULL, 0, (LPDWORD)&lpBytesReturned, NULL);


	// Fill Test Data
	char* buf = NULL;
	int BufSize;
	int Loop;
	int i;
	DWORD writesize;
	BufSize = 1024 * 1024;
	Loop = (int)DiskTestSize;

	buf = (char*) VirtualAlloc(NULL, BufSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (buf == NULL)
	{
		AfxMessageBox(_T("Failed VirtualAlloc()."));
		((CDiskMarkDlg*) dlg)->m_DiskBenchStatus = FALSE;
		return FALSE;
	}

	if (((CDiskMarkDlg*) dlg)->m_TestData == TEST_DATA_ALL0X00)
	{
		for (i = 0; i < BufSize; i++)
		{
			buf[i] = 0;
		}
	}
	else
	{
		// Compatible with DiskSpd
		for (i = 0; i < BufSize; i++)
		{
			buf[i] = (char) (rand() % 256);
		}
	}

	for (i = 0; i < Loop; i++)
	{
		if (((CDiskMarkDlg*) dlg)->m_DiskBenchStatus)
		{
			result = WriteFile(hFile, buf, BufSize, &writesize, NULL);
		}
		else
		{
			CloseHandle(hFile);
			VirtualFree(buf, 0, MEM_RELEASE);
			((CDiskMarkDlg*) dlg)->m_DiskBenchStatus = FALSE;
			return FALSE;
		}
	}
	VirtualFree(buf, 0, MEM_RELEASE);
	CloseHandle(hFile);

	return TRUE;
}

void CALLBACK TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	if(idEvent == TIMER_ID)
	{
		WaitFlag = FALSE;
		KillTimer(hwnd, idEvent);
	}
}

UINT Exit(void* dlg)
{
	DeleteFile(TestFilePath);
	RemoveDirectory(TestFileDir);

	static CString cstr;
	cstr = _T("");

	if(((CDiskMarkDlg*)dlg)->m_TestData == TEST_DATA_ALL0X00)
	{
		cstr = ALL_0X00_0FILL;
	}
	else
	{
		cstr = _T("");
	}

	::PostMessage(((CDiskMarkDlg*)dlg)->GetSafeHwnd(), WM_USER_UPDATE_MESSAGE, NULL, (LPARAM)&cstr);
	::PostMessage(((CDiskMarkDlg*)dlg)->GetSafeHwnd(), WM_USER_EXIT_BENCHMARK, 0, 0);

	((CDiskMarkDlg*)dlg)->m_DiskBenchStatus = FALSE;
	((CDiskMarkDlg*)dlg)->m_WinThread = NULL;

	return 0;
}

void DiskSpd(void* dlg, DISK_SPD_CMD cmd)
{
	static CString cstr;
	double score;
	double *maxScore;
	CString command;
	CString title;
	CString qt;
	CString option;
	CString bufOption;

	int j;

	if (!((CDiskMarkDlg*) dlg)->m_DiskBenchStatus)
	{
		return;
	}


	if (((CDiskMarkDlg*) dlg)->m_TestData == TEST_DATA_ALL0X00)
	{
		bufOption += L" -Z";
	}
	else
	{
		switch (cmd)
		{
		case TEST_SEQUENTIAL_WRITE1:
#ifdef SEQUENTIAL2
		case TEST_SEQUENTIAL_WRITE2:
#endif
			bufOption += L" -Z128K";
			break;
		case TEST_RANDOM_WRITE_4KB1:
		case TEST_RANDOM_WRITE_4KB2:
		case TEST_RANDOM_WRITE_4KB3:
			bufOption += L" -Z4K";
			break;
		}
	}

	switch (cmd)
	{
	case TEST_CREATE_FILE:
		title = L"Preparing...";
		::PostMessage(((CDiskMarkDlg*) dlg)->GetSafeHwnd(), WM_USER_UPDATE_MESSAGE, (WPARAM) &title, 0);

		option.Format(L"-c%dM", (int)DiskTestSize);
		option += bufOption;
		command.Format(_T("\"%s\" %s \"%s\""), DiskSpdExe, option, TestFilePath);
		ExecAndWait((TCHAR*) (command.GetString()), TRUE);

		return;
		break;
	case TEST_SEQUENTIAL_READ1:
		title.Format(L"Sequential Read");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*) dlg)->m_SequentialMultiQueues1, ((CDiskMarkDlg*) dlg)->m_SequentialMultiThreads1);
		option.Format(L"-b128k -d5 -o%d -t%d -W0 -S -w0", ((CDiskMarkDlg*) dlg)->m_SequentialMultiQueues1, ((CDiskMarkDlg*) dlg)->m_SequentialMultiThreads1);
		maxScore = &(((CDiskMarkDlg*) dlg)->m_SequentialReadScore1);
		break;
	case TEST_SEQUENTIAL_WRITE1:
		title.Format(L"Sequential Write");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*) dlg)->m_SequentialMultiQueues1, ((CDiskMarkDlg*) dlg)->m_SequentialMultiThreads1);
		option.Format(L"-b128k -d5 -o%d -t%d -W0 -S -w100", ((CDiskMarkDlg*) dlg)->m_SequentialMultiQueues1, ((CDiskMarkDlg*) dlg)->m_SequentialMultiThreads1);
		option += bufOption;
		maxScore = &(((CDiskMarkDlg*) dlg)->m_SequentialWriteScore1);
		break;
#ifdef SEQUENTIAL2
	case TEST_SEQUENTIAL_READ2:
		title.Format(L"Sequential Read");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*)dlg)->m_SequentialMultiQueues2, ((CDiskMarkDlg*)dlg)->m_SequentialMultiThreads2);
		option.Format(L"-b128k -d5 -o%d -t%d -W0 -S -w0", ((CDiskMarkDlg*)dlg)->m_SequentialMultiQueues2, ((CDiskMarkDlg*)dlg)->m_SequentialMultiThreads2);
		maxScore = &(((CDiskMarkDlg*)dlg)->m_SequentialReadScore2);
		break;
	case TEST_SEQUENTIAL_WRITE2:
		title.Format(L"Sequential Write");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*)dlg)->m_SequentialMultiQueues2, ((CDiskMarkDlg*)dlg)->m_SequentialMultiThreads2);
		option.Format(L"-b128k -d5 -o%d -t%d -W0 -S -w100", ((CDiskMarkDlg*)dlg)->m_SequentialMultiQueues2, ((CDiskMarkDlg*)dlg)->m_SequentialMultiThreads2);
		option += bufOption;
		maxScore = &(((CDiskMarkDlg*)dlg)->m_SequentialWriteScore2);
		break;
#endif
	case TEST_RANDOM_READ_4KB1:
		title.Format(L"Random Read 4KiB");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*) dlg)->m_RandomMultiQueues1, ((CDiskMarkDlg*) dlg)->m_RandomMultiThreads1);
		option.Format(L"-b4K -d5 -o%d -t%d -W0 -r -S -w0", ((CDiskMarkDlg*) dlg)->m_RandomMultiQueues1, ((CDiskMarkDlg*) dlg)->m_RandomMultiThreads1);
		maxScore = &(((CDiskMarkDlg*) dlg)->m_RandomRead4KBScore1);
		break;
	case TEST_RANDOM_WRITE_4KB1:
		title.Format(L"Random Write 4KiB");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*) dlg)->m_RandomMultiQueues1, ((CDiskMarkDlg*) dlg)->m_RandomMultiThreads1);
		option.Format(L"-b4K -d5 -o%d -t%d -W0 -r -S -w100", ((CDiskMarkDlg*) dlg)->m_RandomMultiQueues1, ((CDiskMarkDlg*) dlg)->m_RandomMultiThreads1);
		option += bufOption;
		maxScore = &(((CDiskMarkDlg*) dlg)->m_RandomWrite4KBScore1);
		break;
	case TEST_RANDOM_READ_4KB2:
		title.Format(L"Random Read 4KiB");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues2, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads2);
		option.Format(L"-b4K -d5 -o%d -t%d -W0 -r -S -w0", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues2, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads2);
		maxScore = &(((CDiskMarkDlg*)dlg)->m_RandomRead4KBScore2);
		break;
	case TEST_RANDOM_WRITE_4KB2:
		title.Format(L"Random Write 4KiB");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues2, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads2);
		option.Format(L"-b4K -d5 -o%d -t%d -W0 -r -S -w100", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues2, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads2);
		option += bufOption;
		maxScore = &(((CDiskMarkDlg*)dlg)->m_RandomWrite4KBScore2);
		break;
	case TEST_RANDOM_READ_4KB3:
		title.Format(L"Random Read 4KiB");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues3, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads3);
		option.Format(L"-b4K -d5 -o%d -t%d -W0 -r -S -w0", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues3, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads3);
		maxScore = &(((CDiskMarkDlg*)dlg)->m_RandomRead4KBScore3);
		break;
	case TEST_RANDOM_WRITE_4KB3:
		title.Format(L"Random Write 4KiB");
		qt.Format(L"[Q=%d/T=%d]", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues3, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads3);
		option.Format(L"-b4K -d5 -o%d -t%d -W0 -r -S -w100", ((CDiskMarkDlg*)dlg)->m_RandomMultiQueues3, ((CDiskMarkDlg*)dlg)->m_RandomMultiThreads3);
		option += bufOption;
		maxScore = &(((CDiskMarkDlg*)dlg)->m_RandomWrite4KBScore3);
		break;
	}

	score = 0.0;
	*maxScore = 0.0;
	for (j = 0; j <= DiskTestCount; j++)
	{
		if (j == 0)
		{
			cstr.Format(L"Preparing... %s", title);
		}
		else
		{
			cstr.Format(L"%s [%d/%d]", title, j, DiskTestCount);
		}
		::PostMessage(((CDiskMarkDlg*) dlg)->GetSafeHwnd(), WM_USER_UPDATE_MESSAGE, (WPARAM) &cstr, 0);
		
		command.Format(_T("\"%s\" %s \"%s\""), DiskSpdExe, option, TestFilePath);

		score = ExecAndWait((TCHAR*) (command.GetString()), TRUE) / 10 / 1000.0;

		if (j > 0 && score > *maxScore)
		{
			*maxScore = score;
			::PostMessage(((CDiskMarkDlg*) dlg)->GetSafeHwnd(), WM_USER_UPDATE_SCORE, 0, 0);
		}

		if (!((CDiskMarkDlg*) dlg)->m_DiskBenchStatus)
		{
			return;
		}
	}
	::PostMessage(((CDiskMarkDlg*) dlg)->GetSafeHwnd(), WM_USER_UPDATE_SCORE, 0, 0);
}
