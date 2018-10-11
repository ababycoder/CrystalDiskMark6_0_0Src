// InputDlg.cpp : implementation file
//

#include "stdafx.h"
#include "DiskMark.h"
#include "InputDlg.h"
#include "afxdialogex.h"


// CInputDlg dialog

IMPLEMENT_DYNAMIC(CInputDlg, CDialogEx)

CInputDlg::CInputDlg(CWnd* pParent /*=NULL*/):CDialog(IDD_INPUT_DIALOG, pParent)
{	
}

CInputDlg::~CInputDlg()
{
}

void CInputDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
}


BEGIN_MESSAGE_MAP(CInputDlg, CDialog)
	ON_BN_CLICKED(IDOK, &CInputDlg::OnBnClickedOk)
END_MESSAGE_MAP()


// CInputDlg message handlers

void CInputDlg::OnBnClickedOk()
{
	// TODO: Add your control notification handler code here
	m_inputNum = GetDlgItemInt(IDC_EDIT);
	CDialog::OnOK();
}

BOOL CInputDlg::OnInitDialog()
{
	SetDlgItemInt(IDC_EDIT, m_inputNum);
	return CDialog::OnInitDialog();
}