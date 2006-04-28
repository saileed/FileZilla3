#include "FileZilla.h"
#include "state.h"
#include "LocalListView.h"
#include "LocalTreeView.h"
#include "RemoteListView.h"
#include "viewheader.h"
#include "commandqueue.h"
#include "FileZillaEngine.h"
#include "Options.h"
#include "MainFrm.h"

CState::CState(CMainFrame* pMainFrame)
{
	m_pMainFrame = pMainFrame;

	m_pLocalListView = 0;
	m_pLocalTreeView = 0;
	m_pRemoteListView = 0;

	m_pDirectoryListing = 0;
	m_pServer = 0;
	m_pLocalViewHeader = 0;
	m_pRemoteViewHeader = 0;

	m_pEngine = 0;
	m_pCommandQueue = 0;
}

CState::~CState()
{
	delete m_pDirectoryListing;
	delete m_pServer;

	delete m_pCommandQueue;
	delete m_pEngine;
}

wxString CState::GetLocalDir() const
{
	return m_localDir;
}

bool CState::SetLocalDir(wxString dir)
{
	dir.Replace(_T("\\"), _T("/"));
	if (dir != _T(".."))
	{
#ifdef __WXMSW__
		if (dir == _T("") || dir == _T("/") || dir.c_str()[1] == ':' || m_localDir == _T("\\"))
			m_localDir = dir;
		else
#else
		if (dir.IsEmpty() || dir.c_str()[0] == '/')
			m_localDir = dir;
		else
#endif
			m_localDir += dir;

		if (m_localDir.Right(1) != _T("/"))
			m_localDir += _T("/");
	}
	else
	{
#ifdef __WXMSW__
		m_localDir.Replace(_T("\\"), _T("/"));
#endif

		if (m_localDir == _T("/"))
			m_localDir.Clear();
		else if (!m_localDir.IsEmpty())
		{
			m_localDir.Truncate(m_localDir.Length() - 1);
			int pos = m_localDir.Find('/', true);
			if (pos == -1)
				m_localDir = _T("/");
			else
				m_localDir = m_localDir.Left(pos + 1);
		}
	}

#ifdef __WXMSW__

	//Todo: Desktop and homedir
	if (m_localDir == _T(""))
		m_localDir = _T("\\");

	m_localDir.Replace(_T("/"), _T("\\"));
#else
	if (m_localDir == _T(""))
		m_localDir = _T("/");
#endif

	if (m_pLocalListView)
		m_pLocalListView->DisplayDir(m_localDir);

	if (m_pLocalTreeView)
		m_pLocalTreeView->SetDir(m_localDir);

	if (m_pLocalViewHeader)
		m_pLocalViewHeader->SetDir(m_localDir);

	return true;
}

void CState::SetLocalListView(CLocalListView *pLocalListView)
{
	m_pLocalListView = pLocalListView;
}

void CState::SetLocalTreeView(CLocalTreeView *pLocalTreeView)
{
	m_pLocalTreeView = pLocalTreeView;
}

void CState::SetRemoteListView(CRemoteListView *pRemoteListView)
{
	m_pRemoteListView = pRemoteListView;
}

bool CState::SetRemoteDir(const CDirectoryListing *pDirectoryListing, bool modified /*=false*/)
{
    if (!pDirectoryListing)
	{
		if (m_pRemoteListView)
			m_pRemoteListView->SetDirectoryListing(0);
		delete m_pDirectoryListing;
		m_pDirectoryListing = 0;
		return true;
	}

	if (m_pDirectoryListing && pDirectoryListing && 
		m_pDirectoryListing->path == pDirectoryListing->path && 
		pDirectoryListing->m_failed)
		return true;

	CDirectoryListing *newListing = new CDirectoryListing;
	*newListing = *pDirectoryListing;
	
	if (m_pRemoteListView)
		m_pRemoteListView->SetDirectoryListing(newListing, modified);

	if (m_pRemoteViewHeader)
		m_pRemoteViewHeader->SetDir(newListing ? newListing->path : CServerPath());

	delete m_pDirectoryListing;
	m_pDirectoryListing = newListing;

	return true;
}

const CDirectoryListing *CState::GetRemoteDir() const
{
	return m_pDirectoryListing;
}

const CServerPath CState::GetRemotePath() const
{
	if (!m_pDirectoryListing)
		return CServerPath();
	
	return m_pDirectoryListing->path;
}

void CState::RefreshLocal()
{
	if (m_pLocalListView)
		m_pLocalListView->DisplayDir(m_localDir);
	if (m_pLocalTreeView)
		m_pLocalTreeView->Refresh();
}

void CState::SetServer(const CServer* server)
{
	delete m_pServer;
	if (server)
		m_pServer = new CServer(*server);
	else
		m_pServer = 0;
}

const CServer* CState::GetServer() const
{
	return m_pServer;
}

void CState::ApplyCurrentFilter()
{
	if (m_pLocalListView)
		m_pLocalListView->ApplyCurrentFilter();
	if (m_pRemoteListView)
		m_pRemoteListView->ApplyCurrentFilter();
}

bool CState::Connect(const CServer& server, bool askBreak, const CServerPath& path /*=CServerPath()*/)
{
	if (!m_pEngine)
		return false;
	if (m_pEngine->IsConnected() || m_pEngine->IsBusy() || !m_pCommandQueue->Idle())
	{
		if (askBreak)
			if (wxMessageBox(_("Break current connection?"), _T("FileZilla"), wxYES_NO | wxICON_QUESTION) != wxYES)
				return false;
		m_pCommandQueue->Cancel();
	}

	SetServer(&server);
	m_pCommandQueue->ProcessCommand(new CConnectCommand(server));
	m_pCommandQueue->ProcessCommand(new CListCommand(path));
	
	COptions::Get()->SetLastServer(server);

	return true;
}

bool CState::CreateEngine()
{
	wxASSERT(!m_pEngine);
	if (m_pEngine)
		return true;

	m_pEngine = new CFileZillaEngine();
	m_pEngine->Init(m_pMainFrame, COptions::Get());

	m_pCommandQueue = new CCommandQueue(m_pEngine, m_pMainFrame);
	
	return true;
}

void CState::DestroyEngine()
{
	delete m_pCommandQueue;
	m_pCommandQueue = 0;
	delete m_pEngine;
	m_pEngine = 0;
}
