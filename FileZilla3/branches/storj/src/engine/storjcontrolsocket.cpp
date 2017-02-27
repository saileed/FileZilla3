#include <filezilla.h>

#include "directorycache.h"
#include "directorylistingparser.h"
#include "engineprivate.h"
#include "pathcache.h"
#include "proxy.h"
#include "servercapabilities.h"
#include "storjcontrolsocket.h"

#include <libfilezilla/event_loop.hpp>
#include <libfilezilla/local_filesys.hpp>
#include <libfilezilla/process.hpp>
#include <libfilezilla/thread_pool.hpp>

#include <algorithm>
#include <cwchar>

struct storj_message
{
	storjEvent type;
	mutable std::wstring text[3];
};

struct storj_event_type;
typedef fz::simple_event<storj_event_type, storj_message> CStorjEvent;

struct terminate_event_type;
typedef fz::simple_event<terminate_event_type, std::wstring> CTerminateEvent;

class CStorjInputThread final
{
public:
	CStorjInputThread(CStorjControlSocket* pOwner, fz::process& proc)
		: process_(proc)
		, m_pOwner(pOwner)
	{
	}

	~CStorjInputThread()
	{
		thread_.join();
	}

	bool spawn(fz::thread_pool & pool)
	{
		if (!thread_) {
			thread_ = pool.spawn([this]() { entry(); });
		}
		return thread_.operator bool();
	}

protected:

	std::wstring ReadLine(std::wstring &error)
	{
		int len = 0;
		const int buffersize = 4096;
		char buffer[buffersize];

		while (true) {
			char c;
			int read = process_.read(&c, 1);
			if (read != 1) {
				if (!read) {
					error = L"Unexpected EOF.";
				}
				else {
					error = L"Unknown error reading from process";
				}
				return std::wstring();
			}

			if (c == '\n') {
				break;
			}

			if (len == buffersize - 1) {
				// Cap string length
				continue;
			}

			buffer[len++] = c;
		}

		while (len && buffer[len - 1] == '\r') {
			--len;
		}

		buffer[len] = 0;

		std::wstring const line = m_pOwner->ConvToLocal(buffer, len + 1);
		if (len && line.empty()) {
			error = L"Failed to convert reply to local character set.";
		}

		return line;
	}

	void entry()
	{
		std::wstring error;
		while (error.empty()) {
			char readType = 0;
			int read = process_.read(&readType, 1);
			if (read != 1) {
				break;
			}

			readType -= '0';

			if (readType < 0 || readType >= static_cast<char>(storjEvent::count) ) {
				error = fz::sprintf(L"Unknown eventType %d", readType);
				break;
			}

			storjEvent eventType = static_cast<storjEvent>(readType);

			int lines{};
			switch (eventType)
			{
			case storjEvent::count:
			case storjEvent::Unknown:
				error = fz::sprintf(L"Unknown eventType %d", readType);
				break;
			case storjEvent::Recv:
			case storjEvent::Send:
			case storjEvent::UsedQuotaRecv:
			case storjEvent::UsedQuotaSend:
				break;
			case storjEvent::Reply:
			case storjEvent::Done:
			case storjEvent::Error:
			case storjEvent::Verbose:
			case storjEvent::Info:
			case storjEvent::Status:
			case storjEvent::Transfer:
				lines = 1;
				break;
			case storjEvent::Listentry:
				lines = 3;
				break;
			};

			auto msg = new CStorjEvent;
			auto & message = std::get<0>(msg->v_);
			message.type = eventType;
			for (int i = 0; i < lines && error.empty(); ++i) {
				message.text[i] = ReadLine(error);
			}

			if (!error.empty()) {
				delete msg;
				break;
			}

			m_pOwner->send_event(msg);
		}

		m_pOwner->send_event<CTerminateEvent>(error);
	}

	fz::process& process_;
	CStorjControlSocket* m_pOwner;

	fz::async_task thread_;
};

CStorjControlSocket::CStorjControlSocket(CFileZillaEnginePrivate & engine)
	: CControlSocket(engine)
{
	m_useUTF8 = true;
}

CStorjControlSocket::~CStorjControlSocket()
{
	remove_handler();
	DoClose();
}

enum connectStates
{
	connect_init,
	connect_host,
	connect_user,
	connect_pass
};

class CStorjConnectOpData final : public COpData
{
public:
	CStorjConnectOpData()
		: COpData(Command::connect)
		, keyfile_(keyfiles_.cend())
	{
	}

	virtual ~CStorjConnectOpData()
	{
	}

	std::wstring lastChallenge;
	CInteractiveLoginNotification::type lastChallengeType{CInteractiveLoginNotification::interactive};
	bool criticalFailure{};

	std::vector<std::wstring> keyfiles_;
	std::vector<std::wstring>::const_iterator keyfile_;
};

void CStorjControlSocket::Connect(const CServer &server)
{
	LogMessage(MessageType::Status, _("Connecting to %s..."), server.Format(ServerFormat::with_optional_port));
	SetWait(true);

	currentServer_ = server;

	CStorjConnectOpData* pData = new CStorjConnectOpData;
	m_pCurOpData = pData;

	pData->opState = connect_init;

	m_pProcess = new fz::process();

	engine_.GetRateLimiter().AddObject(this);

	auto executable = fz::to_native(engine_.GetOptions().GetOption(OPTION_FZSTORJ_EXECUTABLE));
	if (executable.empty()) {
		executable = fzT("fzstorj");
	}
	LogMessage(MessageType::Debug_Verbose, L"Going to execute %s", executable);

	std::vector<fz::native_string> args;
	if (!m_pProcess->spawn(executable, args)) {
		LogMessage(MessageType::Debug_Warning, L"Could not create process");
		DoClose();
		//return FZ_REPLY_ERROR;
	}

	m_pInputThread = new CStorjInputThread(this, *m_pProcess);
	if (!m_pInputThread->spawn(engine_.GetThreadPool())) {
		LogMessage(MessageType::Debug_Warning, L"Thread creation failed");
		delete m_pInputThread;
		m_pInputThread = 0;
		DoClose();
		//return FZ_REPLY_ERROR;
	}

	//return FZ_REPLY_WOULDBLOCK;
}

int CStorjControlSocket::ConnectParseResponse(bool successful, std::wstring const& reply)
{
	LogMessage(MessageType::Debug_Verbose, _T("CStorjControlSocket::ConnectParseResponse(%s)"), reply);

	if (!successful) {
		DoClose(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjConnectOpData *pData = static_cast<CStorjConnectOpData *>(m_pCurOpData);
	if (!pData) {
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	switch (pData->opState)
	{
	case connect_init:
		if (reply != fz::sprintf(L"fzStorj started, protocol_version=%d", FZSTORJ_PROTOCOL_VERSION)) {
			LogMessage(MessageType::Error, _("fzstorj belongs to a different version of FileZilla"));
			DoClose(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
		pData->opState = connect_host;
		break;
	case connect_host:
		pData->opState = connect_user;
		break;
	case connect_user:
		pData->opState = connect_pass;
		break;
	case connect_pass:
		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown op state: %d"), pData->opState);
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CStorjControlSocket::ConnectSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CStorjControlSocket::ConnectSend()"));
	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjConnectOpData *pData = static_cast<CStorjConnectOpData *>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData of wrong type"));
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res{};
	switch (pData->opState)
	{
	case connect_host:
		res = SendCommand(fz::sprintf(L"host %s", m_pCurrentServer->Format(ServerFormat::with_optional_port)));
		break;
	case connect_user:
		res = SendCommand(fz::sprintf(L"user %s", m_pCurrentServer->GetUser()));
		break;
	case connect_pass:
		// FIXME: interactive login
		res = SendCommand(fz::sprintf(L"pass %s", m_pCurrentServer->GetPass()), fz::sprintf(L"pass %s", std::wstring(m_pCurrentServer->GetPass().size(), '*')));
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown op state: %d"), pData->opState);
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (res) {
		return FZ_REPLY_WOULDBLOCK;
	}
	else {
		return FZ_REPLY_ERROR;
	}
}

class CStorjListOpData final : public COpData
{
public:
	CStorjListOpData()
		: COpData(Command::list)
	{
	}

	CDirectoryListing directoryListing;
	std::wstring bucket;

	fz::monotonic_clock m_time_before_locking;
};

enum listStates
{
	list_init = 0,
	list_waitlist,
	list_waitlock,
	list_list
};

void CStorjControlSocket::List(CServerPath const& path, std::wstring const& subDir, int flags)
{
	if (!path.empty()) {
		m_CurrentPath = path;
	}
	if (!m_CurrentPath.ChangePath(subDir)) {
		m_CurrentPath.clear();
	}
	if (m_CurrentPath.empty()) {
		m_CurrentPath.SetPath(L"/");
	}
	LogMessage(MessageType::Status, _("Retrieving directory listing of \"%s\"..."), m_CurrentPath.GetPath());

	CStorjListOpData *pData = new CStorjListOpData;
	pData->pNextOpData = m_pCurOpData;
	m_pCurOpData = pData;
	pData->directoryListing.path = m_CurrentPath;

	if (!m_pCurrentServer) {
		LogMessage(MessageType::Debug_Warning, _T("CStorjControlSocket::List called with m_pCurrenServer == 0"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		//return FZ_REPLY_ERROR;
	}

	if (m_CurrentPath.GetType() != ServerType::UNIX) {
		LogMessage(MessageType::Debug_Warning, L"CStorControlSocket::List called with incompatible server type %d in path", m_CurrentPath.GetType());
		ResetOperation(FZ_REPLY_INTERNALERROR);
		//return FZ_REPLY_ERROR;
	}

	if (pData->directoryListing.path.SegmentCount() > 1) {
		LogMessage(MessageType::Error, L"Invalid path");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		//return FZ_REPLY_ERROR;
	}

	if (pData->directoryListing.path.SegmentCount() == 1) {
		CDirectoryListing buckets;

		bool outdated{};
		bool found = engine_.GetDirectoryCache().Lookup(buckets, *m_pCurrentServer, CServerPath(L"/"), false, outdated);
		if (found && !outdated) {
			int pos = buckets.FindFile_CmpCase(pData->directoryListing.path.GetLastSegment());
			if (pos != -1) {
				pData->bucket = *buckets[pos].ownerGroup;
				LogMessage(MessageType::Debug_Info, L"Directory is in bucket %s", pData->bucket);
			}
			else {
				LogMessage(MessageType::Error, _("Bucket not found"));
				ResetOperation(FZ_REPLY_ERROR);
				//return FZ_REPLY_ERROR;
			}
		}
		else {
			pData->opState = list_waitlist;
			int res = List(CServerPath(L"/"), std::wstring(), 0);
			if (res != FZ_REPLY_OK) {
				return res;
			}
			LogMessage(MessageType::Debug_Warning, L"Subcommand unexpectedly completed early");
			ResetOperation(FZ_REPLY_INTERNALERROR);
			//return FZ_REPLY_ERROR;
		}
	}

	pData->opState = list_waitlock;
	if (!TryLockCache(lock_list, m_CurrentPath)) {
		pData->m_time_before_locking = fz::monotonic_clock::now();
		//return FZ_REPLY_WOULDBLOCK;
	}

	++pData->opState;

	//return SendNextCommand();
}

int CStorjControlSocket::ListSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CStorjControlSocket::ListSend()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjListOpData *pData = static_cast<CStorjListOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (pData->opState == list_waitlock) {
		if (!pData->holdsLock) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Not holding the lock as expected"));
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}

		// Check if we can use already existing listing
		CDirectoryListing listing;
		bool is_outdated = false;
		bool found = engine_.GetDirectoryCache().Lookup(listing, *m_pCurrentServer, m_CurrentPath, false, is_outdated);
		if (found && !is_outdated && !listing.get_unsure_flags() &&
			listing.m_firstListTime >= pData->m_time_before_locking)
		{
			engine_.SendDirectoryListingNotification(listing.path, !pData->pNextOpData, false, false);
			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}

		++pData->opState;

		return SendNextCommand();
	}
	else if (pData->opState == list_list) {
		if (pData->bucket.empty()) {
			if (!SendCommand(L"list-buckets")) {
				return FZ_REPLY_ERROR;
			}
		}
		else {
			if (!SendCommand(L"list " + pData->bucket)) {
				return FZ_REPLY_ERROR;
			}
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	LogMessage(MessageType::Debug_Warning, _T("Unknown opState in CStorjControlSocket::ListSend"));
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

int CStorjControlSocket::ListParseResponse(bool successful, std::wstring const& reply)
{
	LogMessage(MessageType::Debug_Verbose, _T("CStorjControlSocket::ListParseResponse(%s)"), reply);

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (m_pCurOpData->opId != Command::list) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData of wrong type"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjListOpData *pData = static_cast<CStorjListOpData *>(m_pCurOpData);

	if (pData->opState == list_list) {
		if (!successful) {
			ResetOperation(FZ_REPLY_ERROR);
			return FZ_REPLY_ERROR;
		}

		pData->directoryListing.m_firstListTime = fz::monotonic_clock::now();

		engine_.GetDirectoryCache().Store(pData->directoryListing, *m_pCurrentServer);
		engine_.SendDirectoryListingNotification(pData->directoryListing.path, !pData->pNextOpData, true, false);

		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	}

	LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("ListParseResponse called at inproper time: %d"), pData->opState);
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

int CStorjControlSocket::ListParseEntry(std::wstring && name, std::wstring const& size, std::wstring && id)
{
	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (m_pCurOpData->opId != Command::list) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Listentry received, but current operation is not Command::list"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjListOpData *pData = static_cast<CStorjListOpData *>(m_pCurOpData);
	if (pData->opState != list_list) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("ListParseResponse called at inproper time: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CDirentry entry;
	entry.name = name;
	entry.size = fz::to_integral<int64_t>(size, -1);
	entry.ownerGroup.get() = id;
	if (pData->bucket.empty()) {
		entry.flags = CDirentry::flag_dir;
		entry.size = -1;
	}
	else {
		entry.flags = 0;
	}

	pData->directoryListing.Append(std::move(entry));

	return FZ_REPLY_WOULDBLOCK;
}

int CStorjControlSocket::ListSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjControlSocket::ListSubcommandResult()");

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjListOpData *pData = static_cast<CStorjListOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, L"  state = %d", pData->opState);

	if (pData->opState != list_waitlist || pData->directoryListing.path.SegmentCount() != 1) {
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK) {
		ResetOperation(prevResult);
		return FZ_REPLY_ERROR;
	}

	m_CurrentPath = pData->directoryListing.path;

	CDirectoryListing buckets;

	bool outdated{};
	bool found = engine_.GetDirectoryCache().Lookup(buckets, *m_pCurrentServer, CServerPath(L"/"), false, outdated);
	if (found && !outdated) {
		int pos = buckets.FindFile_CmpCase(pData->directoryListing.path.GetLastSegment());
		if (pos != -1) {
			pData->bucket = *buckets[pos].ownerGroup;
			LogMessage(MessageType::Debug_Info, L"Directory is in bucket %s", pData->bucket);
		}
	}

	if (pData->bucket.empty()) {
		LogMessage(MessageType::Error, _("Bucket ID not found"));
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	pData->opState = list_waitlock;
	if (!TryLockCache(lock_list, m_CurrentPath)) {
		pData->m_time_before_locking = fz::monotonic_clock::now();
		return FZ_REPLY_WOULDBLOCK;
	}

	++pData->opState;

	return SendNextCommand();
}


class CStorjFileTransferOpData : public CFileTransferOpData
{
public:
	CStorjFileTransferOpData(bool is_download, std::wstring const& local_file, std::wstring const& remote_file, CServerPath const& remote_path)
		: CFileTransferOpData(is_download, local_file, remote_file, remote_path)
	{
	}

	std::wstring bucket;
	std::wstring fileId;
};

enum filetransferStates
{
	filetransfer_init = 0,
	filetransfer_waitlist,
	filetransfer_transfer
};


void CStorjControlSocket::FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
						 std::wstring const& remoteFile, bool download,
						 CFileTransferCommand::t_transferSettings const& transferSettings)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjControlSocket::FileTransfer(...)");

	if (localFile.empty()) {
		if (!download) {
			ResetOperation(FZ_REPLY_CRITICALERROR | FZ_REPLY_NOTSUPPORTED);
		}
		else {
			ResetOperation(FZ_REPLY_SYNTAXERROR);
		}
		//return FZ_REPLY_ERROR;
	}

	if (remotePath.SegmentCount() < 1) {
		ResetOperation(FZ_REPLY_CRITICALERROR | FZ_REPLY_NOTSUPPORTED);
		//return FZ_REPLY_ERROR;
	}

	CStorjFileTransferOpData *pData = new CStorjFileTransferOpData(download, localFile, remoteFile, remotePath);
	m_pCurOpData = pData;

	pData->transferSettings = transferSettings;

	// Get local file info
	int64_t size;
	bool isLink;
	if (fz::local_filesys::get_file_info(fz::to_native(pData->localFile), isLink, &size, 0, 0) == fz::local_filesys::file) {
		pData->localFileSize = size;
	}

	CServerPath dirToList;

	// Get bucket
	CDirectoryListing buckets;
	bool outdated{};
	bool found = engine_.GetDirectoryCache().Lookup(buckets, *m_pCurrentServer, CServerPath(L"/"), false, outdated);
	if (found && !outdated) {
		int pos = buckets.FindFile_CmpCase(pData->remotePath.GetLastSegment());
		if (pos != -1) {
			pData->bucket = *buckets[pos].ownerGroup;
			LogMessage(MessageType::Debug_Info, L"File %s is in bucket %s", pData->remotePath.FormatFilename(pData->remoteFile), pData->bucket);
		}
		else {
			LogMessage(MessageType::Error, _("Bucket not found"));
			ResetOperation(FZ_REPLY_ERROR);
			return FZ_REPLY_ERROR;
		}//
	}
	else {
		dirToList = CServerPath(L"/");
	}

	// Get remote file info
	CDirentry entry;
	bool dirDidExist;
	bool matchedCase;
	found = engine_.GetDirectoryCache().LookupFile(entry, *m_pCurrentServer, pData->remotePath, pData->remoteFile, dirDidExist, matchedCase);
	if (!found) {
		if (!dirDidExist) {
			dirToList = pData->remotePath;
		}
	}
	else {
		if (entry.is_unsure()) {
			dirToList = pData->remotePath;
		}
		else {
			if (matchedCase) {
				pData->remoteFileSize = entry.size;
				if (entry.has_date()) {
					pData->fileTime = entry.time;
				}
				pData->fileId = *entry.ownerGroup;
				LogMessage(MessageType::Debug_Info, L"File %s has id %s", pData->remotePath.FormatFilename(pData->remoteFile), pData->fileId);
			}
		}
	}

	if (pData->download && pData->fileId.empty() && dirToList != pData->remotePath) {
		LogMessage(MessageType::Error, _("File not found"));
		ResetOperation(FZ_REPLY_ERROR);
		//return FZ_REPLY_ERROR;
	}

	if (dirToList.empty()) {
		pData->opState = filetransfer_transfer;
		int res = CheckOverwriteFile();
		if (res != FZ_REPLY_OK) {
			//return res;
		}
	}
	else {
		pData->opState = filetransfer_waitlist;
		int res = List(dirToList, std::wstring(), 0);
		if (res != FZ_REPLY_OK) {
			//return res;
		}
		LogMessage(MessageType::Debug_Warning, L"Subcommand unexpectedly completed early");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		//return FZ_REPLY_ERROR;
	}

	//return SendNextCommand();
}

int CStorjControlSocket::FileTransferSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CStorjControlSocket::FileTransferSubcommandResult()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjFileTransferOpData *pData = static_cast<CStorjFileTransferOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (pData->opState == filetransfer_waitlist) {

		if (prevResult != FZ_REPLY_OK) {
			ResetOperation(FZ_REPLY_ERROR);
			return FZ_REPLY_ERROR;
		}

		if (pData->bucket.empty()) {
			// Get bucket
			CDirectoryListing buckets;
			bool outdated{};
			bool found = engine_.GetDirectoryCache().Lookup(buckets, *m_pCurrentServer, CServerPath(L"/"), false, outdated);
			if (found && !outdated) {
				int pos = buckets.FindFile_CmpCase(pData->remotePath.GetLastSegment());
				if (pos != -1) {
					pData->bucket = *buckets[pos].ownerGroup;
					LogMessage(MessageType::Debug_Info, L"File %s is in bucket %s", pData->remotePath.FormatFilename(pData->remoteFile), pData->bucket);
				}
			}

			if (pData->bucket.empty()) {
				LogMessage(MessageType::Error, _("Bucket not found for file %s"), pData->remotePath.FormatFilename(pData->remoteFile));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}

		if (pData->fileId.empty()) {
			// Get remote file info
			CDirentry entry;
			bool dirDidExist;
			bool matchedCase;
			bool found = engine_.GetDirectoryCache().LookupFile(entry, *m_pCurrentServer, pData->remotePath, pData->remoteFile, dirDidExist, matchedCase);
			if (found && !entry.is_unsure() && matchedCase) {
				pData->remoteFileSize = entry.size;
				if (entry.has_date()) {
					pData->fileTime = entry.time;
				}
				pData->fileId = *entry.ownerGroup;
				LogMessage(MessageType::Debug_Info, L"File %s has id %s", pData->remotePath.FormatFilename(pData->remoteFile), pData->fileId);
			}

			if (pData->download && pData->fileId.empty()) {
				LogMessage(MessageType::Error, _("File id not found for file %s"), pData->remotePath.FormatFilename(pData->remoteFile));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}

		pData->opState = filetransfer_transfer;

		int res = CheckOverwriteFile();
		if (res != FZ_REPLY_OK) {
			return res;
		}
	}
	else {
		LogMessage(MessageType::Debug_Warning, _T("Unknown opState in CStorjControlSocket::FileTransferSubcommandResult"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CStorjControlSocket::FileTransferSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CStorjControlSocket::FileTransferSend()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjFileTransferOpData *pData = static_cast<CStorjFileTransferOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (pData->opState == filetransfer_transfer) {
		if (!pData->resume) {
			CreateLocalDir(pData->localFile);
		}

		engine_.transfer_status_.Init(pData->remoteFileSize, 0, false);
		if (pData->download) {
			if (!SendCommand(L"get " + pData->bucket + L" " + pData->fileId + L" " + QuoteFilename(pData->localFile))) {
				return FZ_REPLY_ERROR;
			}
		}
		else {
			if (!SendCommand(L"put " + pData->bucket + L" " + QuoteFilename(pData->localFile) + L" " + QuoteFilename(pData->remoteFile))) {
				return FZ_REPLY_ERROR;
			}
		}

		engine_.transfer_status_.SetStartTime();
		pData->transferInitiated = true;

		return FZ_REPLY_WOULDBLOCK;
	}

	LogMessage(MessageType::Debug_Warning, _T("Unknown opState in CStorjControlSocket::ListSend"));
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

int CStorjControlSocket::FileTransferParseResponse(int result, std::wstring const& reply)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjControlSocket::FileTransferParseResponse(%d)", result);

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjFileTransferOpData *pData = static_cast<CStorjFileTransferOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (pData->opState == filetransfer_transfer) {
		ResetOperation(result);
		return result;
	}

	LogMessage(MessageType::Debug_Warning, _T("Unknown opState in CStorjControlSocket::ListSend"));
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}


void CStorjControlSocket::OnStorjEvent(storj_message const& message)
{
	if (!m_pCurrentServer) {
		return;
	}

	if (!m_pInputThread) {
		return;
	}

	switch (message.type)
	{
	case storjEvent::Reply:
		LogMessageRaw(MessageType::Response, message.text[0]);
		ProcessReply(FZ_REPLY_OK, message.text[0]);
		break;
	case storjEvent::Done:
		{
			int result;
			if (message.text[0] == L"1") {
				result = FZ_REPLY_OK;
			}
			else if (message.text[0] == L"2") {
				result = FZ_REPLY_CRITICALERROR;
			}
			else {
				result = FZ_REPLY_ERROR;
			}
			ProcessReply(result, std::wstring());
		}
		break;
	case storjEvent::Error:
		LogMessageRaw(MessageType::Error, message.text[0]);
		break;
	case storjEvent::Verbose:
		LogMessageRaw(MessageType::Debug_Info, message.text[0]);
		break;
	case storjEvent::Info:
		LogMessageRaw(MessageType::Command, message.text[0]); // Not exactly the right message type, but it's a silent one.
		break;
	case storjEvent::Status:
		LogMessageRaw(MessageType::Status, message.text[0]);
		break;
	case storjEvent::Recv:
		SetActive(CFileZillaEngine::recv);
		break;
	case storjEvent::Send:
		SetActive(CFileZillaEngine::send);
		break;
	case storjEvent::Listentry:
		ListParseEntry(std::move(message.text[0]), message.text[1], std::move(message.text[2]));
		break;
	case storjEvent::UsedQuotaRecv:
		OnQuotaRequest(CRateLimiter::inbound);
		break;
	case storjEvent::UsedQuotaSend:
		OnQuotaRequest(CRateLimiter::outbound);
		break;
	case storjEvent::Transfer:
		{
			auto value = fz::to_integral<int64_t>(message.text[0]);

			bool tmp;
			CTransferStatus status = engine_.transfer_status_.Get(tmp);
			if (!status.empty() && !status.madeProgress) {
				if (m_pCurOpData && m_pCurOpData->opId == Command::transfer) {
					CStorjFileTransferOpData *pData = static_cast<CStorjFileTransferOpData *>(m_pCurOpData);
					if (pData->download) {
						if (value > 0) {
							engine_.transfer_status_.SetMadeProgress();
						}
					}
					else {
						if (status.currentOffset > status.startOffset + 65565) {
							engine_.transfer_status_.SetMadeProgress();
						}
					}
				}
			}

			engine_.transfer_status_.Update(value);
		}
		break;
	default:
		wxFAIL_MSG(_T("given notification codes not handled"));
		break;
	}
}

void CStorjControlSocket::OnTerminate(std::wstring const& error)
{
	if (!error.empty()) {
		LogMessageRaw(MessageType::Error, error);
	}
	else {
		LogMessageRaw(MessageType::Debug_Info, _T("CStorjControlSocket::OnTerminate without error"));
	}
	if (m_pProcess) {
		DoClose();
	}
}

bool CStorjControlSocket::SendCommand(std::wstring const& cmd, std::wstring const& show)
{
	SetWait(true);

	LogMessageRaw(MessageType::Command, show.empty() ? cmd : show);

	// Check for newlines in command
	// a command like "ls\nrm foo/bar" is dangerous
	if (cmd.find('\n') != std::wstring::npos ||
		cmd.find('\r') != std::wstring::npos)
	{
		LogMessage(MessageType::Debug_Warning, _T("Command containing newline characters, aborting."));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	return AddToStream(cmd + _T("\n"));
}

bool CStorjControlSocket::AddToStream(std::wstring const& cmd)
{
	if (!m_pProcess) {
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	std::string const str = ConvToServer(cmd, true);
	if (str.empty()) {
		LogMessage(MessageType::Error, _("Could not convert command to server encoding"));
		ResetOperation(FZ_REPLY_OK);
		return false;
	}

	return m_pProcess->write(str);
}

bool CStorjControlSocket::SetAsyncRequestReply(CAsyncRequestNotification *pNotification)
{
	if (m_pCurOpData) {
		if (!m_pCurOpData->waitForAsyncRequest) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Not waiting for request reply, ignoring request reply %d"), pNotification->GetRequestID());
			return false;
		}
		m_pCurOpData->waitForAsyncRequest = false;
	}

	RequestId const requestId = pNotification->GetRequestID();
	switch(requestId)
	{
/*	case reqId_fileexists:
		{
			CFileExistsNotification *pFileExistsNotification = static_cast<CFileExistsNotification *>(pNotification);
			return SetFileExistsAction(pFileExistsNotification);
		}
	case reqId_hostkey:
	case reqId_hostkeyChanged:
		{
			if (GetCurrentCommandId() != Command::connect ||
				!m_pCurrentServer)
			{
				LogMessage(MessageType::Debug_Info, _T("SetAsyncRequestReply called to wrong time"));
				return false;
			}

			CHostKeyNotification *pHostKeyNotification = static_cast<CHostKeyNotification *>(pNotification);
			std::wstring show;
			if (requestId == reqId_hostkey) {
				show = _("Trust new Hostkey:");
			}
			else {
				show = _("Trust changed Hostkey:");
			}
			show += ' ';
			if (!pHostKeyNotification->m_trust) {
				SendCommand(std::wstring(), show + _("No"));
				if (m_pCurOpData && m_pCurOpData->opId == Command::connect) {
					CStorjConnectOpData *pData = static_cast<CStorjConnectOpData *>(m_pCurOpData);
					pData->criticalFailure = true;
				}
			}
			else if (pHostKeyNotification->m_alwaysTrust) {
				SendCommand(L"y", show + _("Yes"));
			}
			else {
				SendCommand(L"n", show + _("Once"));
			}
		}
		break;
	case reqId_interactiveLogin:
		{
			CInteractiveLoginNotification *pInteractiveLoginNotification = static_cast<CInteractiveLoginNotification *>(pNotification);

			if (!pInteractiveLoginNotification->passwordSet) {
				DoClose(FZ_REPLY_CANCELED);
				return false;
			}
			std::wstring const pass = pInteractiveLoginNotification->server.GetPass();
			m_pCurrentServer->SetUser(m_pCurrentServer->GetUser(), pass);
			std::wstring show = L"Pass: ";
			show.append(pass.size(), '*');
			SendCommand(pass, show);
		}
		break;*/
	default:
		LogMessage(MessageType::Debug_Warning, _T("Unknown async request reply id: %d"), requestId);
		return false;
	}

	return true;
}

void CStorjControlSocket::ProcessReply(int result, std::wstring const& reply)
{
	Command commandId = GetCurrentCommandId();
	switch (commandId)
	{
	case Command::connect:
		ConnectParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::list:
		ListParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::transfer:
		FileTransferParseResponse(result, reply);
		break;
/*	case Command::cwd:
		ChangeDirParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::mkdir:
		MkdirParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::del:
		DeleteParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::removedir:
		RemoveDirParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::rename:
		RenameParseResponse(result == FZ_REPLY_OK, reply);
		break;*/
	default:
		LogMessage(MessageType::Debug_Warning, _T("No action for parsing replies to command %d"), commandId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}
}

int CStorjControlSocket::ResetOperation(int nErrorCode)
{
	LogMessage(MessageType::Debug_Verbose, _T("CStorjControlSocket::ResetOperation(%d)"), nErrorCode);

	if (m_pCurOpData && m_pCurOpData->opId == Command::connect) {
		CStorjConnectOpData *pData = static_cast<CStorjConnectOpData *>(m_pCurOpData);
		if (pData->opState == connect_init && nErrorCode & FZ_REPLY_ERROR && (nErrorCode & FZ_REPLY_CANCELED) != FZ_REPLY_CANCELED) {
			LogMessage(MessageType::Error, _("fzstorj could not be started"));
		}
		if (pData->criticalFailure) {
			nErrorCode |= FZ_REPLY_CRITICALERROR;
		}
	}
/*	if (m_pCurOpData && m_pCurOpData->opId == Command::del && !(nErrorCode & FZ_REPLY_DISCONNECTED)) {
		CStorjDeleteOpData *pData = static_cast<CStorjDeleteOpData *>(m_pCurOpData);
		if (pData->m_needSendListing) {
			engine_.SendDirectoryListingNotification(pData->path, false, true, false);
		}
	}*/

	return CControlSocket::ResetOperation(nErrorCode);
}

int CStorjControlSocket::DoClose(int nErrorCode)
{
	engine_.GetRateLimiter().RemoveObject(this);

	if (m_pProcess) {
		m_pProcess->kill();
	}

	if (m_pInputThread) {
		delete m_pInputThread;
		m_pInputThread = 0;

		auto threadEventsFilter = [&](fz::event_loop::Events::value_type const& ev) -> bool {
			if (ev.first != this) {
				return false;
			}
			else if (ev.second->derived_type() == CStorjEvent::type() || ev.second->derived_type() == CTerminateEvent::type()) {
				return true;
			}
			return false;
		};

		event_loop_.filter_events(threadEventsFilter);
	}
	if (m_pProcess) {
		delete m_pProcess;
		m_pProcess = 0;
	}
	return CControlSocket::DoClose(nErrorCode);
}

void CStorjControlSocket::Cancel()
{
	if (GetCurrentCommandId() != Command::none) {
		DoClose(FZ_REPLY_CANCELED);
	}
}

void CStorjControlSocket::OnRateAvailable(CRateLimiter::rate_direction direction)
{
	//OnQuotaRequest(direction);
}

void CStorjControlSocket::OnQuotaRequest(CRateLimiter::rate_direction direction)
{
	/*int64_t bytes = GetAvailableBytes(direction);
	if (bytes > 0) {
		int b;
		if (bytes > INT_MAX) {
			b = INT_MAX;
		}
		else {
			b = bytes;
		}
		AddToStream(fz::sprintf(L"-%d%d,%d\n", direction, b, engine_.GetOptions().GetOptionVal(OPTION_SPEEDLIMIT_INBOUND + static_cast<int>(direction))));
		UpdateUsage(direction, b);
	}
	else if (bytes == 0) {
		Wait(direction);
	}
	else if (bytes < 0) {
		AddToStream(fz::sprintf(L"-%d-\n", direction));
	}*/
}

void CStorjControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<CStorjEvent, CTerminateEvent>(ev, this,
		&CStorjControlSocket::OnStorjEvent,
		&CStorjControlSocket::OnTerminate)) {
		return;
	}

	CControlSocket::operator()(ev);
}

std::wstring CStorjControlSocket::QuoteFilename(std::wstring const& filename)
{
	return L"\"" + fz::replaced_substrings(filename, L"\"", L"\"\"") + L"\"";
}

