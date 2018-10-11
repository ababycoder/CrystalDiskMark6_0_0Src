/*---------------------------------------------------------------------------*/
//       Author : hiyohiyo
//         Mail : hiyohiyo@crystalmark.info
//          Web : http://crystalmark.info/
//      License : The MIT License
//
//                                             Copyright (c) 2007-2015 hiyohiyo
/*---------------------------------------------------------------------------*/

#include "stdafx.h"
#include "DiskMark.h"
#include "DiskMarkDlg.h"

#include "GetFileVersion.h"
#include "GetOsInfo.h"

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <afxole.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CDiskMarkApp, CWinApp)
	ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()

CDiskMarkApp::CDiskMarkApp()
{
}

CDiskMarkApp theApp;

//-----------------------------------------------------------------------------
// Prototypes
//-----------------------------------------------------------------------------
static BOOL IsFileExistEx(const TCHAR* path, const TCHAR* fileName);

BOOL CDiskMarkApp::InitInstance()
{
	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls);

	CWinApp::InitInstance();

	// Init m_Ini
	TCHAR *ptrEnd;
	TCHAR ini[MAX_PATH];
	TCHAR exeName[MAX_PATH];

	::GetModuleFileName(NULL, ini, MAX_PATH);
	if((ptrEnd = _tcsrchr(ini, '.')) != NULL)
	{
		*ptrEnd = '\0';
		_tcscat_s(ini, MAX_PATH, _T(".ini"));
		_wsplitpath_s(ini, NULL, 0, NULL, 0, exeName, MAX_PATH, NULL, 0);
		m_Ini = ini;
	}

	OSVERSIONINFOEX osvi;
	BOOL bosVersionInfoEx;

	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	if(!(bosVersionInfoEx = GetVersionEx((OSVERSIONINFO *)&osvi)))
	{
		osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
		GetVersionEx((OSVERSIONINFO *)&osvi);
	}

	// UAC support for Vista/7 or later
	if(osvi.dwMajorVersion >= 6 && WritePrivateProfileString(_T("Settings"), _T("WriteCheck"), _T("0"), m_Ini) == 0)
	{
		CString path;
		SHGetSpecialFolderPath(NULL, ini, CSIDL_LOCAL_APPDATA, TRUE);
		path.Format(_T("%s\\%s"), ini, PRODUCT_ROMING_NAME);
		CreateDirectory(path, NULL);
		m_Ini.Format(_T("%s\\%s.%s"), path, exeName, _T("ini"));
		if(WritePrivateProfileString(_T("Settings"), _T("WriteCheck"), _T("0"), m_Ini) == 0)
		{
			m_Ini.Format(_T("%s\\%s.%s"), ini, exeName, _T("ini"));
		}
	}

	CString DefaultTheme;
	CString DefaultLanguage;
	TCHAR tmp[MAX_PATH];

	GetModuleFileName(NULL, tmp, MAX_PATH);
	if((ptrEnd = _tcsrchr(tmp, '\\')) != NULL){*ptrEnd = '\0';}
	m_ExeDir.Format(_T("%s\\"), tmp);
	m_ThemeDir.Format(_T("%s\\%s"), tmp, THEME_DIR);
	m_LangDir.Format(_T("%s\\%s"), tmp, LANGUAGE_DIR);

	m_ThemeIndex = MENU_THEME_INDEX;
	m_LangIndex = MENU_LANG_INDEX;

	m_MainDlgPath.Format(_T("%s\\") DIALOG_DIR MAIN_DIALOG, tmp);
	m_MainDlgPathIE8.Format(_T("%s\\") DIALOG_DIR MAIN_DIALOG_IE8, tmp);
	m_AboutDlgPath.Format(_T("%s\\") DIALOG_DIR ABOUT_DIALOG, tmp);
	m_SettingsDlgPath.Format(_T("%s\\") DIALOG_DIR SETTINGS_DIALOG, tmp);

	DefaultTheme.Format(_T("%s\\%s"), tmp, DEFAULT_THEME);
	DefaultLanguage.Format(_T("%s\\%s"), tmp, DEFAULT_LANGUAGE);
	
	if (!IsFileExistEx(m_MainDlgPath, MAIN_DIALOG))
	{
		AfxMessageBox(_T("Not Found Main Dialog."));
		return FALSE;
	}

	if (!IsFileExistEx(m_MainDlgPathIE8, MAIN_DIALOG_IE8))
	{
		AfxMessageBox(_T("Not Found Main Dialog for IE8 Mode."));
		return FALSE;
	}


	if(! IsFileExistEx(DefaultTheme, DEFAULT_THEME))
	{
		AfxMessageBox(_T("Not Found Default Theme."));
		return FALSE;
	}
	if(! IsFileExistEx(DefaultLanguage, DEFAULT_LANGUAGE))
	{
		AfxMessageBox(_T("Not Found Default Language."));
		return FALSE;
	}

	// No Server Busy Dialog!!
	if(!AfxOleInit())
	{
		AfxMessageBox(_T("Unsupported OLE version."));
		return FALSE;
	}

	// No Server Busy Dialog!!
	AfxOleGetMessageFilter()->SetMessagePendingDelay(60 * 1000);
	AfxOleGetMessageFilter()->EnableNotRespondingDialog(FALSE);
	AfxOleGetMessageFilter()->EnableBusyDialog(FALSE);

	CString cstr;
	DWORD debugMode = GetPrivateProfileInt(_T("Settings"), _T("DebugMode"), 0, m_Ini);
	SetDebugMode(debugMode);
	cstr.Format(_T("%d"), debugMode);
	WritePrivateProfileString(_T("Settings"), _T("DebugMode"), cstr, m_Ini);

	// Multimedia Timer Setting
	TIMECAPS tc;
	timeGetDevCaps(&tc,sizeof(TIMECAPS));
	timeBeginPeriod(tc.wPeriodMin);

	BOOL flagReExec = FALSE;

	CDiskMarkDlg dlg;
	m_pMainWnd = &dlg;

	if (dlg.DoModal() == RE_EXEC)
	{
		flagReExec = TRUE;
	}

	timeEndPeriod(tc.wPeriodMin);

	if(flagReExec)
	{
		TCHAR str[MAX_PATH];
		::GetModuleFileName(NULL, str, MAX_PATH);
		ShellExecute(NULL, NULL, str, NULL, NULL, SW_SHOWNORMAL);
	}

	return FALSE;
}

BOOL IsFileExistEx(const TCHAR* path, const TCHAR* fileName)
{
	if(! IsFileExist(path))
	{
		CString cstr;
		cstr.Format(_T("Not Found \"%s\"."), fileName); 
		AfxMessageBox(cstr);
		return FALSE;
	}
	return TRUE;
}