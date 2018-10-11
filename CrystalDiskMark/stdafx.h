/*---------------------------------------------------------------------------*/
//       Author : hiyohiyo
//         Mail : hiyohiyo@crystalmark.info
//          Web : http://crystalmark.info/
//      License : The MIT License
//
//                                             Copyright (c) 2007-2015 hiyohiyo
/*---------------------------------------------------------------------------*/

#pragma once

#ifndef _SECURE_ATL
#define _SECURE_ATL 1
#endif

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#ifndef WINVER
#define WINVER 0x0501
#endif

#ifndef _WIN32_WINNT              
#define _WIN32_WINNT 0x0501
#endif						

#ifndef _WIN32_WINDOWS
#define _WIN32_WINDOWS 0x0410
#endif

#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#define _AFX_ALL_WARNINGS

#include <afxwin.h>         // MFC core and standard component
#include <afxext.h>         // Extended MFC

#ifndef _AFX_NO_OLE_SUPPORT
#include <afxdtctl.h>		// MFC IE4 Common Control support
#endif
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>			// MFC Windows Common Control support
#endif // _AFX_NO_AFXCMN_SUPPORT

#include <afxdhtml.h>        // CDHtmlDialog
#include "DHtmlDialogEx.h"		// CDHtmlDialogEx by hiyohiyo
#include "DHtmlMainDialog.h"	// CDHtmlMainDialog by hiyohiyo

#include "DebugPrint.h"

#ifdef _UNICODE
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// Version Information
#define PRODUCT_NAME			_T("CrystalDiskMark")
#define PRODUCT_VERSION			_T("6.0.0")
#define PRODUCT_ROMING_NAME		_T("CrystalDiskMark")

#ifdef UWP
#ifdef SUISHO_SHIZUKU_SUPPORT
#define PRODUCT_SHORT_NAME		_T("")
#ifdef _M_X64
#define PRODUCT_EDITION			_T("Shizuku Edition x64 (UWP)")
#else
#define PRODUCT_EDITION			_T("Shizuku Edition (UWP)")
#endif
#else
#define PRODUCT_SHORT_NAME		_T("")
#ifdef _M_X64
#define PRODUCT_EDITION			_T("x64 (UWP)")
#else
#define PRODUCT_EDITION			_T("(UWP)")
#endif
#endif
#else
#ifdef SUISHO_SHIZUKU_SUPPORT
#define PRODUCT_SHORT_NAME		_T("")
#ifdef _M_X64
#define PRODUCT_EDITION			_T("Shizuku Edition x64")
#else
#define PRODUCT_EDITION			_T("Shizuku Edition")
#endif
#else
#define PRODUCT_SHORT_NAME		_T("")
#ifdef _M_X64
#define PRODUCT_EDITION			_T("x64")
#else
#define PRODUCT_EDITION			_T("")
#endif
#endif
#endif

#define PRODUCT_RELEASE			_T("2017/11/5")
#define PRODUCT_COPY_YEAR		_T("2007-2017")
#define PRODUCT_COPYRIGHT		_T("&copy; hiyohiyo 2007-2017 ")

#define URL_CRYSTAL_DEW_WORLD_JA	_T("https://crystalmark.info/")
#define URL_CRYSTAL_DEW_WORLD_EN 	_T("https://crystalmark.info/?lang=en")

#define	URL_PROJECT_SHIZUKU_JA		_T("https://suishoshizuku.com/ja/")
#define	URL_PROJECT_SHIZUKU_EN		_T("https://suishoshizuku.com/en/")

#define	URL_CDM_LICENSE_JA			_T("https://crystalmark.info/software/CrystalDiskMark/manual-ja/License.html")
#define	URL_CDM_LICENSE_EN			_T("https://crystalmark.info/software/CrystalDiskMark/manual-en/License.html")

#define URL_DISKSPD					_T("https://github.com/microsoft/diskspd")

#define URL_HTML_HELP_JA			_T("https://crystalmark.info/software/CrystalDiskMark/manual-ja/")
#define URL_HTML_HELP_EN 			_T("https://crystalmark.info/software/CrystalDiskMark/manual-en/")

#define MAX_THREADS 64
#define MAX_QUEUES 512

static const int RE_EXEC = 5963;
