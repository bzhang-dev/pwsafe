/*
* Copyright (c) 2003-2014 Rony Shapiro <ronys@users.sourceforge.net>.
* All rights reserved. Use of the code is allowed under the
* Artistic License 2.0 terms, as specified in the LICENSE file
* distributed with this code, or available from
* http://www.opensource.org/licenses/artistic-license-2.0.php
*/
/// \file PasskeyEntry.cpp
//-----------------------------------------------------------------------------

/*
Passkey?  That's Russian for 'pass'.  You know, passkey
down the streetsky.  [Groucho Marx]
*/

#include "PasswordSafe.h"
#include "PWFileDialog.h"
#include "ThisMfcApp.h"
#include "PasskeyEntry.h"
#include "PasskeySetup.h"
#include "Fonts.h"
#include "DboxMain.h" // for CheckPasskey()
#include "PWSversion.h"
#include "GeneralMsgBox.h"
#include "SDThread.h"

#include "core/PwsPlatform.h"
#include "core/Pwsdirs.h"
#include "core/pwsprefs.h"
#include "core/PWScore.h"
#include "core/PWSfile.h"
#include "core/Util.h"
#include "core/core.h"

#include "os/file.h"
#include "os/dir.h"

#include "VirtualKeyboard/VKeyBoardDlg.h"

#include "resource.h"
#include "resource3.h"  // String resources

#include "SecString.h"

#include "SysColStatic.h"

#include <iomanip>  // For setbase and setw

// See DboxMain.h for the relevant enum
int CPasskeyEntry::dialog_lookup[10] = {
  // Normal dialogs
  IDD_PASSKEYENTRY_FIRST,          // GCP_FIRST
  IDD_PASSKEYENTRY,                // GCP_NORMAL
  IDD_PASSKEYENTRY,                // GCP_RESTORE
  IDD_PASSKEYENTRY_WITHEXIT,       // GCP_WITHEXIT
  IDD_PASSKEYENTRY,                // GCP_CHANGEMODE
  // Secure Desktop dialogs
  IDD_PASSKEYENTRY_FIRST_SD,       // GCP_FIRST_SD
  IDD_PASSKEYENTRY_SD,             // GCP_NORMAL_SD
  IDD_PASSKEYENTRY_SD,             // GCP_RESTORE_SD
  IDD_PASSKEYENTRY_WITHEXIT_SD,    // GCP_WITHEXIT_SD
  IDD_PASSKEYENTRY_SD              // GCP_CHANGEMODE_SD
};

//-----------------------------------------------------------------------------
CPasskeyEntry::CPasskeyEntry(CWnd* pParent, const CString& a_filespec, int index,
                             bool bReadOnly, bool bForceReadOnly, bool bHideReadOnly,
                             bool bUseSecureDesktop)
  : CPKBaseDlg(dialog_lookup[index + (bUseSecureDesktop ? NUM_PER_ENVIRONMENT : 0)], pParent, bUseSecureDesktop),
  m_filespec(a_filespec), m_orig_filespec(a_filespec),
  m_tries(0),
  m_status(TAR_INVALID),
  m_PKE_ReadOnly(bReadOnly ? TRUE : FALSE),
  m_bForceReadOnly(bForceReadOnly), m_bHideReadOnly(bHideReadOnly),
  m_yubi_sk(NULL)
{
  m_index = index;

  DBGMSG("CPasskeyEntry()\n");
  if (m_index == GCP_FIRST) {
    DBGMSG("** FIRST **\n");
  }

  m_hIcon = app.LoadIcon(IDI_CORNERICON);
  m_SelectedDatabase = a_filespec;

  PWSversion *pPWSver = PWSversion::GetInstance();
  int nMajor = pPWSver->GetMajor();
  int nMinor = pPWSver->GetMinor();
  int nBuild = pPWSver->GetBuild();
  CString csSpecialBuild = pPWSver->GetSpecialBuild();

  if (nBuild == 0)
    m_appversion.Format(L"V%d.%02d%s", nMajor, nMinor, csSpecialBuild);
  else
    m_appversion.Format(L"V%d.%02d.%02d%s", nMajor, nMinor, nBuild, csSpecialBuild);
}

CPasskeyEntry::~CPasskeyEntry()
{
  ::DestroyIcon(m_hIcon);

  if (m_yubi_sk != NULL) {
    trashMemory(m_yubi_sk, PWSfile::HeaderRecord::YUBI_SK_LEN);
    delete[] m_yubi_sk;
  }
}

void CPasskeyEntry::DoDataExchange(CDataExchange* pDX)
{
  CPKBaseDlg::DoDataExchange(pDX);

  //{{AFX_DATA_MAP(CPasskeyEntry)
  if (m_index == GCP_FIRST) {
    DDX_Control(pDX, IDC_STATIC_LOGOTEXT, m_ctlLogoText);
    DDX_Control(pDX, IDC_DATABASECOMBO, m_MRU_combo);
  }

  DDX_Control(pDX, IDC_STATIC_LOGO, m_ctlLogo);
  DDX_Text(pDX, IDC_SELECTED_DATABASE, m_SelectedDatabase);
  DDX_Check(pDX, IDC_READONLY, m_PKE_ReadOnly);

  if (!m_bUseSecureDesktop)
    DDX_Control(pDX, IDOK, m_ctlOK);
  else
    DDX_Control(pDX, IDC_ENTERCOMBINATION, m_ctlEnterCombination);
  //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CPasskeyEntry, CPKBaseDlg)
  //{{AFX_MSG_MAP(CPasskeyEntry)
  ON_WM_DESTROY()
  ON_WM_TIMER()
  ON_BN_CLICKED(ID_HELP, OnHelp)
  ON_BN_CLICKED(IDC_CREATE_DB, OnCreateDb)
  ON_BN_CLICKED(IDC_EXIT, OnExit)
  ON_CBN_EDITCHANGE(IDC_DATABASECOMBO, OnComboEditChange)
  ON_CBN_SELCHANGE(IDC_DATABASECOMBO, OnComboSelChange)
  ON_BN_CLICKED(IDC_BTN_BROWSE, OnOpenFileBrowser)
  ON_MESSAGE(PWS_MSG_INSERTBUFFER, OnInsertBuffer)
  ON_STN_CLICKED(IDC_VKB, OnVirtualKeyboard)
  ON_BN_CLICKED(IDC_YUBIKEY_BTN, OnYubikeyBtn)
  ON_BN_CLICKED(IDC_ENTERCOMBINATION, OnEnterCombination)
  //}}AFX_MSG_MAP
END_MESSAGE_MAP()

static CString NarrowPathText(const CString &text)
{
  const int Width = 48;
  CString retval;
  if (text.GetLength() > Width) {
    retval =  text.Left(Width / 2 - 5) +
                   L" ... " + text.Right(Width / 2);
  } else {
    retval = text;
  }
  return retval;
}

BOOL CPasskeyEntry::OnInitDialog(void)
{
  CPKBaseDlg::OnInitDialog();

  if (!m_bUseSecureDesktop) {
    Fonts::GetInstance()->ApplyPasswordFont(GetDlgItem(IDC_PASSKEY));

    m_pctlPasskey->SetPasswordChar(PSSWDCHAR);
  }

  switch (m_index) {
  case GCP_FIRST:
    // At start up - give the user the option unless file is R-O
    GetDlgItem(IDC_READONLY)->EnableWindow(m_bForceReadOnly ? FALSE : TRUE);
    GetDlgItem(IDC_READONLY)->ShowWindow(SW_SHOW);
    GetDlgItem(IDC_VERSION)->SetWindowText(m_appversion);
#ifdef DEMO
    GetDlgItem(IDC_SPCL_TXT)->
      SetWindowText(CString(MAKEINTRESOURCE(IDS_DEMO)));
#endif
    break;
  case GCP_NORMAL:
    // otherwise during open - user can - again unless file is R-O
    if (m_bHideReadOnly) {
      GetDlgItem(IDC_READONLY)->EnableWindow(FALSE);
      GetDlgItem(IDC_READONLY)->ShowWindow(SW_HIDE);
    }
    else {
      GetDlgItem(IDC_READONLY)->EnableWindow(m_bForceReadOnly ? FALSE : TRUE);
      GetDlgItem(IDC_READONLY)->ShowWindow(SW_SHOW);
    }
    break;
  case GCP_RESTORE:
  case GCP_WITHEXIT:
    GetDlgItem(IDC_READONLY)->EnableWindow(m_bForceReadOnly ? FALSE : TRUE);
    GetDlgItem(IDC_READONLY)->ShowWindow(SW_SHOW);
    break;
  case GCP_CHANGEMODE:
    GetDlgItem(IDC_READONLY)->EnableWindow(FALSE);
    GetDlgItem(IDC_READONLY)->ShowWindow(SW_HIDE);
    break;
  default:
    ASSERT(FALSE);
  }

  // Only show virtual Keyboard menu if we can load DLL
  if (!m_bUseSecureDesktop && !m_bVKAvailable) {
    GetDlgItem(IDC_VKB)->ShowWindow(SW_HIDE);
    GetDlgItem(IDC_VKB)->EnableWindow(FALSE);
  }

  if (m_SelectedDatabase.IsEmpty()) {
    if (m_index == GCP_FIRST) {
      SetOKButton(true, false);
      if (m_bUseSecureDesktop) {
        m_SelectedDatabase.LoadString(IDS_NOCURRENTSAFE);
      }
    }
  }

  if (m_index == GCP_FIRST) {
    GetDlgItem(IDC_SELECTED_DATABASE)->ShowWindow(SW_HIDE);

    CRecentFileList *mru = app.GetMRU();
    ASSERT(mru != NULL);

    int N = mru->GetSize();

    std::vector<CSecString> cs_tooltips;

    if (!m_filespec.IsEmpty()) {
      cs_tooltips.push_back(m_filespec);
      m_MRU_combo.AddString(NarrowPathText(m_filespec));
      m_MRU_combo.SelectString(-1, NarrowPathText(m_filespec));
      m_MRU_combo.SetItemData(0, DWORD_PTR(-1));
    }

    for (int i = 0; i < N; i++) {
      const CString &str = (*mru)[i];
      if (str != m_filespec && !str.IsEmpty()) {
        cs_tooltips.push_back(str);
        int li = m_MRU_combo.AddString(NarrowPathText(str));
        if (li != CB_ERR && li != CB_ERRSPACE)
          m_MRU_combo.SetItemData(li, i);
      }
    }
    if ((N > 0) || !m_filespec.IsEmpty()) {
      // Add an empty row to allow NODB
      int li = m_MRU_combo.AddString(L"");
      if (li != CB_ERR && li != CB_ERRSPACE) {
        m_MRU_combo.SetItemData(li, DWORD_PTR(-2)); // -1 already taken, but a < 0 value is easier to check than N
        CString cs_empty(MAKEINTRESOURCE(IDS_EMPTY_DB));
        cs_tooltips.push_back(cs_empty);
        N++; // for SetHeight
      }
    }

    if (N > 0) {
      SetHeight(N);
    }

    m_MRU_combo.SetToolTipStrings(cs_tooltips);
  }

  /*
   * this bit makes the background come out right on the bitmaps
   */

  if (m_index == GCP_FIRST) {
    m_ctlLogoText.ReloadBitmap(IDB_PSLOGO);
    m_ctlLogo.ReloadBitmap(m_bUseSecureDesktop ? IDB_CLOGO_SMALL : IDB_CLOGO);
  } else {
    m_ctlLogo.ReloadBitmap(IDB_CLOGO_SMALL);
  }

  // Set the icon for this dialog.  The framework does this automatically
  //  when the application's main window is not a dialog

  SetIcon(m_hIcon, TRUE);  // Set big icon
  SetIcon(m_hIcon, FALSE); // Set small icon

  if (app.WasHotKeyPressed()) {
    // Reset it
    app.SetHotKeyPressed(false);
    // Following (1) brings to top when hotkey pressed,
    // (2) ensures focus is on password entry field, where it belongs.
    // This is "stronger" than BringWindowToTop().
    SetWindowPos(&CWnd::wndTopMost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetActiveWindow();
    SetForegroundWindow();
    if (!m_bUseSecureDesktop)
      m_pctlPasskey->SetFocus();
    return FALSE;
  }

  // Following works fine for other (non-hotkey) cases:
  SetForegroundWindow();

  // If the dbase field's !empty, the user most likely will want to enter
  // a password:
  if (m_index == GCP_FIRST && !m_filespec.IsEmpty()) {
    m_MRU_combo.SetEditSel(-1, -1);
    if (!m_bUseSecureDesktop)
      m_pctlPasskey->SetFocus();
    return FALSE;
  }

  return TRUE;
}

void CPasskeyEntry::OnCreateDb()
{
  // 1. Get a filename from a file dialog box
  // 2. Get a password
  // 3. Set m_filespec && m_passkey to returned value!
  INT_PTR rc;
  CString newfile;
  CString cs_msg, cs_title, cs_temp;
  CString cs_text(MAKEINTRESOURCE(IDS_CREATENAME));

  CString cf(MAKEINTRESOURCE(IDS_DEFDBNAME)); // reasonable default for first time user
  std::wstring v3FileName = PWSUtil::GetNewFileName(LPCWSTR(cf), DEFAULT_SUFFIX);

  std::wstring dir;
  DboxMain *pDbx = (DboxMain*)GetParent();
  if (pDbx->GetCurFile().empty())
    dir = PWSdirs::GetSafeDir();
  else {
    std::wstring cdrive, cdir, dontCare;
    pws_os::splitpath(pDbx->GetCurFile().c_str(), cdrive, cdir, dontCare, dontCare);
    dir = cdrive + cdir;
  }

  while (1) {
    CPWFileDialog fd(FALSE,
                     DEFAULT_SUFFIX,
                     v3FileName.c_str(),
                     OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                        OFN_LONGNAMES | OFN_OVERWRITEPROMPT,
                     CString(MAKEINTRESOURCE(IDS_FDF_V3_ALL)),
                     this);

    fd.m_ofn.lpstrTitle = cs_text;

    if (!dir.empty())
      fd.m_ofn.lpstrInitialDir = dir.c_str();

    rc = fd.DoModal();

    if (((DboxMain*)GetParent())->ExitRequested()) {
      // If U3ExitNow called while in CPWFileDialog,
      // PostQuitMessage makes us return here instead
      // of exiting the app. Try resignalling
      PostQuitMessage(0);
      return;
    }
    if (rc == IDOK) {
      newfile = fd.GetPathName();
      break;
    } else
      return;
  }

  // 2. Get a password
  bool bUseSecureDesktop = PWSprefs::GetInstance()->GetPref(PWSprefs::UseSecureDesktop);

  do
  {
    CPasskeySetup pksetup(this, *app.GetCore(), bUseSecureDesktop);
    rc = pksetup.DoModal();

    if (rc == IDOK)
      m_passkey = pksetup.GetPassKey();

    // In case user wanted to toggle Secure Desktop
    bUseSecureDesktop = !bUseSecureDesktop;
  } while (rc == INT_MAX);

  if (rc != IDOK)
    return;  //User cancelled password entry

  // 3. Set m_filespec && m_passkey to returned value!
  m_filespec = newfile;
  if (!m_bUseSecureDesktop)
    ((CEdit*)GetDlgItem(IDC_PASSKEY))->SetWindowText(m_passkey);

  m_status = TAR_NEW;
  CPWDialog::OnOK();
}

void CPasskeyEntry::OnCancel()
{
  m_status = TAR_CANCEL;
  CPWDialog::OnCancel();
}

void CPasskeyEntry::OnExit()
{
  m_status = TAR_EXIT;
  CPWDialog::OnCancel();
}

void CPasskeyEntry::OnOK()
{
  UpdateData(TRUE);

  if (m_filespec.IsEmpty()) {
    m_status = TAR_OPEN_NODB;
    CPWDialog::OnCancel();
    return;
  }

  CGeneralMsgBox gmb;
  if (m_passkey.IsEmpty()) {
    gmb.AfxMessageBox(IDS_CANNOTBEBLANK);
    m_pctlPasskey->SetFocus();
    return;
  }

  if (!pws_os::FileExists(m_filespec.GetString())) {
    gmb.AfxMessageBox(IDS_FILEPATHNOTFOUND);
    if (m_MRU_combo.IsWindowVisible())
      m_MRU_combo.SetFocus();
    return;
  }

  ProcessPhrase();
}

void CPasskeyEntry::ProcessPhrase()
{
  CGeneralMsgBox gmb;

  switch (GetMainDlg()->CheckPasskey(LPCWSTR(m_filespec), LPCWSTR(m_passkey))) {
  case PWScore::SUCCESS: {
    // OnOK clears the passkey, so we save it
    const CSecString save_passkey = m_passkey;
    // Try to change read-only state if user changed checkbox:
    // r/w -> r-o always succeeds
    // r-o -> r/w may fail
    // Note that if file is read-only, m_bForceReadOnly is true -> checkbox
    // is disabled -> don't need to worry about that.
    if ((m_index == GCP_RESTORE || m_index == GCP_WITHEXIT) && 
        (m_PKE_ReadOnly == TRUE) == pws_os::IsLockedFile(LPCWSTR(m_filespec))) {
      GetMainDlg()->ChangeMode(false); // false means
      //                           "don't prompt use for password", as we just got it.
    }
    CPWDialog::OnOK();
    m_passkey = save_passkey;
  }
    break;
  case PWScore::WRONG_PASSWORD:
    if (m_tries++ >= 2) { // too many tries
      CString cs_toomany;
      cs_toomany.Format(IDS_TOOMANYTRIES, m_tries);
      gmb.AfxMessageBox(cs_toomany);
    }
    else { // try again
      gmb.AfxMessageBox(m_index == GCP_CHANGEMODE ? IDS_BADPASSKEY : IDS_INCORRECTKEY);
    }
    if (!m_bUseSecureDesktop) {
      m_pctlPasskey->SetSel(MAKEWORD(-1, 0));
      m_pctlPasskey->SetFocus();
    }
    break;
  case PWScore::READ_FAIL:
    gmb.AfxMessageBox(IDSC_FILE_UNREADABLE);
    CPWDialog::OnCancel();
    break;
  case PWScore::TRUNCATED_FILE:
    gmb.AfxMessageBox(IDSC_FILE_TRUNCATED);
    CPWDialog::OnCancel();
    break;
  default:
    ASSERT(0);
    gmb.AfxMessageBox(IDSC_UNKNOWN_ERROR);
    CPWDialog::OnCancel();
    break;
  }
}

void CPasskeyEntry::OnHelp()
{
  ShowHelp(L"::/html/create_new_db.html");
}

//-----------------------------------------------------------------------------

void CPasskeyEntry::UpdateRO()
{
  if (!m_bForceReadOnly) { // if allowed, changed r-o state to reflect file's permission
    bool fro;
    if (pws_os::FileExists(LPCWSTR(m_filespec), fro) && fro) {
      m_PKE_ReadOnly = TRUE;
      GetDlgItem(IDC_READONLY)->EnableWindow(FALSE);
    } else { // no file or write-enabled
      if (m_index == GCP_FIRST)
        m_PKE_ReadOnly = PWSprefs::GetInstance()->GetPref(PWSprefs::DefaultOpenRO) ? TRUE : FALSE;
      else
        m_PKE_ReadOnly = FALSE;
      GetDlgItem(IDC_READONLY)->EnableWindow(TRUE);
    }
    ((CButton *)GetDlgItem(IDC_READONLY))->SetCheck(m_PKE_ReadOnly == TRUE ? 
                                                    BST_CHECKED : BST_UNCHECKED);
    UpdateData(FALSE);
  } // !m_bForceReadOnly
}

void CPasskeyEntry::OnComboEditChange()
{
  m_MRU_combo.m_edit.GetWindowText(m_filespec);
  SetOKButton(m_filespec.IsEmpty(), false);
  UpdateRO();
}

void CPasskeyEntry::OnComboSelChange()
{
  CRecentFileList *mru = app.GetMRU();
  int curSel = m_MRU_combo.GetCurSel();
  const int N = mru->GetSize();

  if (curSel == CB_ERR) {
    ASSERT(0);
  } else if (curSel >= N) {
    m_filespec = L"";
  } else {
    int i = int(m_MRU_combo.GetItemData(curSel));
    if (i >= 0) // -1 means original m_filespec
      m_filespec = (*mru)[i];
    else if (i == -2)
      m_filespec = L"";
    else
      m_filespec = m_orig_filespec;
  }

  SetOKButton(m_filespec.IsEmpty(), true);

  UpdateRO();
}

void CPasskeyEntry::OnOpenFileBrowser()
{
  CString cs_text(MAKEINTRESOURCE(IDS_CHOOSEDATABASE));

  //Open-type dialog box
  CPWFileDialog fd(TRUE,
                   DEFAULT_SUFFIX,
                   NULL,
                   OFN_FILEMUSTEXIST | OFN_LONGNAMES,
                   CString(MAKEINTRESOURCE(IDS_FDF_DB_BU_ALL)),
                   this);

  fd.m_ofn.lpstrTitle = cs_text;

  if (PWSprefs::GetInstance()->GetPref(PWSprefs::DefaultOpenRO))
      fd.m_ofn.Flags |= OFN_READONLY;
    else
      fd.m_ofn.Flags &= ~OFN_READONLY;

  std::wstring dir;
  if (GetMainDlg()->GetCurFile().empty())
    dir = PWSdirs::GetSafeDir();
  else {
    std::wstring cdrive, cdir, dontCare;
    pws_os::splitpath(GetMainDlg()->GetCurFile().c_str(), cdrive, cdir, dontCare, dontCare);
    dir = cdrive + cdir;
  }

  if (!dir.empty())
    fd.m_ofn.lpstrInitialDir = dir.c_str();

  INT_PTR rc = fd.DoModal();

  if (((DboxMain*)GetParent())->ExitRequested()) {
    // If U3ExitNow called while in CPWFileDialog,
    // PostQuitMessage makes us return here instead
    // of exiting the app. Try resignalling
    PostQuitMessage(0);
    return;
  }
  if (rc == IDOK) {
    m_PKE_ReadOnly = fd.GetReadOnlyPref();
    m_filespec = fd.GetPathName();
    m_MRU_combo.m_edit.SetWindowText(m_filespec);
    SetOKButton(m_filespec.IsEmpty(), true);
    UpdateRO();
  } // rc == IDOK
}

void CPasskeyEntry::SetHeight(const int num)
{
  // Find the longest string in the list box.
  CString str;
  CRect rect;
  CSize sz;

  // Try to ensure that dropdown list is big enough for all entries and
  // therefore no scrolling
  int ht = m_MRU_combo.GetItemHeight(0);

  m_MRU_combo.GetWindowRect(&rect);

  sz.cx = rect.Width();
  sz.cy = ht * (num + 2);

  if ((rect.top - sz.cy) < 0 || 
      (rect.bottom + sz.cy > ::GetSystemMetrics(SM_CYSCREEN))) {
    int ifit = max((rect.top / ht), (::GetSystemMetrics(SM_CYSCREEN) - rect.bottom) / ht);
    int ht2 = ht * ifit;
    sz.cy = min(ht2, sz.cy);
  }

  m_MRU_combo.SetWindowPos(NULL, 0, 0, sz.cx, sz.cy, SWP_NOMOVE | SWP_NOZORDER);
}

void CPasskeyEntry::OnVirtualKeyboard()
{
  // This is used if Secure Desktop isn't!
  DWORD dwError; //  Define it here to stop warning that local variable is initialized but not referenced later on

  // Shouldn't be here if couldn't load DLL. Static control disabled/hidden
  if (!m_bVKAvailable)
    return;

  if (m_hwndVKeyBoard != NULL && ::IsWindowVisible(m_hwndVKeyBoard)) {
    // Already there - move to top
    ::SetWindowPos(m_hwndVKeyBoard, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    return;
  }

  // If not already created - do it, otherwise just reset it
  if (m_pVKeyBoardDlg == NULL) {
    HINSTANCE hInstResDLL = app.GetResourceDLL();
    StringX cs_LUKBD = PWSprefs::GetInstance()->GetPref(PWSprefs::LastUsedKeyboard);
    m_pVKeyBoardDlg = new CVKeyBoardDlg(hInstResDLL, this->GetSafeHwnd(), this->GetSafeHwnd(), cs_LUKBD.c_str());
    m_hwndVKeyBoard = CreateDialogParam(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_SDVKEYBOARD), this->GetSafeHwnd(),
      (DLGPROC)(m_pVKeyBoardDlg->VKDialogProc), (LPARAM)(m_pVKeyBoardDlg));

    if (m_hwndVKeyBoard == NULL) {
      dwError = pws_os::IssueError(_T("CreateDialogParam - IDD_SDVKEYBOARD"), false);
      ASSERT(m_hwndVKeyBoard);
    }
  } else {
    m_pVKeyBoardDlg->ResetKeyboard();
  }

  // Now show it and make it top & enable it
  ::SetWindowPos(m_hwndVKeyBoard, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE);
  ::EnableWindow(m_hwndVKeyBoard, TRUE);
}

LRESULT CPasskeyEntry::OnInsertBuffer(WPARAM, LPARAM)
{
  // This is used if Secure Desktop isn't!

  // Update the variables
  UpdateData(TRUE);

  // Get the buffer
  CSecString vkbuffer = m_pVKeyBoardDlg->GetPassphrase();

  // Find the selected characters - if any
  int nStartChar, nEndChar;
  m_pctlPasskey->GetSel(nStartChar, nEndChar);

  // If any characters selected - delete them
  if (nStartChar != nEndChar)
    m_passkey.Delete(nStartChar, nEndChar - nStartChar);

  // Insert the buffer
  m_passkey.Insert(nStartChar, vkbuffer);

  // Update the dialog
  UpdateData(FALSE);

  // Put cursor at end of inserted text
  m_pctlPasskey->SetSel(nStartChar + vkbuffer.GetLength(), 
                        nStartChar + vkbuffer.GetLength());

  return 0L;
}

void CPasskeyEntry::OnYubikeyBtn()
{
  UpdateData(TRUE);
  if (!pws_os::FileExists(m_filespec.GetString())) {
    CGeneralMsgBox gmb;
    gmb.AfxMessageBox(IDS_FILEPATHNOTFOUND);
    if (m_MRU_combo.IsWindowVisible())
      m_MRU_combo.SetFocus();
    return;
  }
  yubiRequestHMACSha1(m_passkey);
}

void CPasskeyEntry::OnEnterCombination()
{
  if (m_filespec.IsEmpty()) {
    m_status = TAR_OPEN_NODB;
    CPWDialog::OnCancel();
    return;
  }

  CGeneralMsgBox gmb;
  if (!pws_os::FileExists(m_filespec.GetString())) {
    gmb.AfxMessageBox(IDS_FILEPATHNOTFOUND);
    if (m_MRU_combo.IsWindowVisible())
      m_MRU_combo.SetFocus();
    return;
  }

  // Get passphrase from Secure Desktop
  StartThread(IDD_SDGETPHRASE);

  ShowWindow(SW_SHOW);

  if (m_GMP.bPhraseEntered) {
    ShowWindow(SW_SHOW);
    m_passkey = m_GMP.sPhrase.c_str();
    ProcessPhrase();
  }

  // Just do nothing if no passphrase entered i.e. user pressed cancel
  // Try to get this seen
  SetForegroundWindow();
}

void CPasskeyEntry::SetOKButton(bool bEmptyDB, bool bSetFocus) {
  if (!m_bUseSecureDesktop) {
    m_pctlPasskey->EnableWindow(TRUE);
    if (bSetFocus)
      m_pctlPasskey->SetFocus();

    m_ctlEnterCombination.EnableWindow(FALSE);
    m_ctlOK.EnableWindow(TRUE);
  } else {
    CString csText(MAKEINTRESOURCE(bEmptyDB ? IDS_OK : IDS_ENTERCOMBINATION));

    m_ctlEnterCombination.SetWindowText(csText);
    m_ctlOK.EnableWindow(FALSE);
    m_ctlEnterCombination.EnableWindow(TRUE);
  }
}