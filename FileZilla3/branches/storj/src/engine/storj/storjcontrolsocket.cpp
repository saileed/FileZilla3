#include <filezilla.h>

#include "connect.h"
#include "event.h"
#include "input_thread.h"
#include "directorycache.h"
#include "directorylistingparser.h"
#include "engineprivate.h"
#include "list.h"
#include "pathcache.h"
#include "proxy.h"
#include "resolve.h"
#include "servercapabilities.h"
#include "storjcontrolsocket.h"

#include <libfilezilla/event_loop.hpp>
#include <libfilezilla/local_filesys.hpp>
#include <libfilezilla/process.hpp>
#include <libfilezilla/thread_pool.hpp>

#include <algorithm>
#include <cwchar>

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

void CStorjControlSocket::Connect(CServer const& server)
{
	LogMessage(MessageType::Status, _("Connecting to %s..."), server.Format(ServerFormat::with_optional_port));
	SetWait(true);

	currentServer_ = server;

	process_ = std::make_unique<fz::process>();

	engine_.GetRateLimiter().AddObject(this);
	Push(std::make_unique<CStorjConnectOpData>(*this));
}
/*
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
*/
void CStorjControlSocket::List(CServerPath const& path, std::wstring const& subDir, int flags)
{
	CServerPath newPath = currentPath_;
	if (!path.empty()) {
		newPath = path;
	}
	if (!newPath.ChangePath(subDir)) {
		newPath.clear();
	}

	if (newPath.empty()) {
		LogMessage(MessageType::Status, _("Retrieving directory listing..."));
	}
	else {
		LogMessage(MessageType::Status, _("Retrieving directory listing of \"%s\"..."), newPath.GetPath());
	}

	Push(std::make_unique<CStorjListOpData>(*this, newPath, std::wstring(), flags, operations_.empty()));
}

/*
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
*/

void CStorjControlSocket::FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
						 std::wstring const& remoteFile, bool download,
						 CFileTransferCommand::t_transferSettings const& transferSettings)
{
}

/*
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
	bool found = engine_.GetDirectoryCache().Lookup(buckets, currentServer_, CServerPath(L"/"), false, outdated);
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
	found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, pData->remotePath, pData->remoteFile, dirDidExist, matchedCase);
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
	LogMessage(MessageType::Debug_Verbose, L"CStorjControlSocket::FileTransferSubcommandResult()");

	CStorjFileTransferOpData *pData = static_cast<CStorjFileTransferOpData *>(m_pCurOpData);

	if (pData->opState == filetransfer_waitlist) {

		if (prevResult != FZ_REPLY_OK) {
			ResetOperation(FZ_REPLY_ERROR);
			return FZ_REPLY_ERROR;
		}

		if (pData->bucket.empty()) {
			// Get bucket
			CDirectoryListing buckets;
			bool outdated{};
			bool found = engine_.GetDirectoryCache().Lookup(buckets, currentServer_, CServerPath(L"/"), false, outdated);
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
			bool found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, pData->remotePath, pData->remoteFile, dirDidExist, matchedCase);
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
		LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjControlSocket::FileTransferSubcommandResult");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CStorjControlSocket::FileTransferSend()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjControlSocket::FileTransferSend()");


	CStorjFileTransferOpData *pData = static_cast<CStorjFileTransferOpData *>(m_pCurOpData);

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

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjControlSocket::ListSend");
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

int CStorjControlSocket::FileTransferParseResponse(int result, std::wstring const& reply)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjControlSocket::FileTransferParseResponse(%d)", result);

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CStorjFileTransferOpData *pData = static_cast<CStorjFileTransferOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, L"  state = %d", pData->opState);

	if (pData->opState == filetransfer_transfer) {
		ResetOperation(result);
		return result;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjControlSocket::ListSend");
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}
*/

void CStorjControlSocket::Resolve(CServerPath const& path, std::wstring const& file, std::wstring & bucket, std::wstring * fileId)
{
	Push(std::make_unique<CStorjResolveOpData>(*this, path, file, bucket, fileId));
}

void CStorjControlSocket::OnStorjEvent(storj_message const& message)
{
	if (!currentServer_) {
		return;
	}

	if (!input_thread_) {
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
		if (operations_.empty() || operations_.back()->opId != Command::list) {
			LogMessage(MessageType::Debug_Warning, L"storjEvent::Listentry outside list operation, ignoring.");
			break;
		}
		else {
			int res = static_cast<CStorjListOpData&>(*operations_.back()).ParseEntry(std::move(message.text[0]), message.text[1], std::move(message.text[2]));
			if (res != FZ_REPLY_WOULDBLOCK) {
				ResetOperation(res);
			}
		}
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
/*
			bool tmp;
			CTransferStatus status = engine_.transfer_status_.Get(tmp);
			if (!status.empty() && !status.madeProgress) {
				if (!operations_.empty() && operations_.back()->opId == Command::transfer) {
					auto & data = static_cast<CStorjFileTransferOpData &>(*operations_.back());
					if (data.download_) {
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
*/
			engine_.transfer_status_.Update(value);
		}
		break;
	default:
		LogMessage(MessageType::Debug_Warning, L"Message type %d not handled", message.type);
		break;
	}
}

void CStorjControlSocket::OnTerminate(std::wstring const& error)
{
	if (!error.empty()) {
		LogMessageRaw(MessageType::Error, error);
	}
	else {
		LogMessageRaw(MessageType::Debug_Info, L"CStorjControlSocket::OnTerminate without error");
	}
	if (process_) {
		DoClose();
	}
}

int CStorjControlSocket::SendCommand(std::wstring const& cmd, std::wstring const& show)
{
	SetWait(true);

	LogMessageRaw(MessageType::Command, show.empty() ? cmd : show);

	// Check for newlines in command
	// a command like "ls\nrm foo/bar" is dangerous
	if (cmd.find('\n') != std::wstring::npos ||
		cmd.find('\r') != std::wstring::npos)
	{
		LogMessage(MessageType::Debug_Warning, L"Command containing newline characters, aborting.");
		return FZ_REPLY_INTERNALERROR;
	}

	return AddToStream(cmd + L"\n");
}

int CStorjControlSocket::AddToStream(std::wstring const& cmd)
{
	if (!process_) {
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	std::string const str = ConvToServer(cmd, true);
	if (str.empty()) {
		LogMessage(MessageType::Error, _("Could not convert command to server encoding"));
		return FZ_REPLY_ERROR;
	}

	if (!process_->write(str)) {
		return FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED;
	}

	return FZ_REPLY_WOULDBLOCK;
}

bool CStorjControlSocket::SetAsyncRequestReply(CAsyncRequestNotification *pNotification)
{
	if (operations_.empty() || !operations_.back()->waitForAsyncRequest) {
		LogMessage(MessageType::Debug_Info, L"Not waiting for request reply, ignoring request reply %d", pNotification->GetRequestID());
		return false;
	}

	operations_.back()->waitForAsyncRequest = false;

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
				LogMessage(MessageType::Debug_Info, L"SetAsyncRequestReply called to wrong time");
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
			currentServer_.SetUser(currentServer_.GetUser(), pass);
			std::wstring show = L"Pass: ";
			show.append(pass.size(), '*');
			SendCommand(pass, show);
		}
		break;*/
	default:
		LogMessage(MessageType::Debug_Warning, L"Unknown async request reply id: %d", requestId);
		return false;
	}

	return true;
}

void CStorjControlSocket::ProcessReply(int result, std::wstring const& reply)
{
	result_ = result;
	response_ = reply;

	if (operations_.empty()) {
		LogMessage(MessageType::Debug_Info, L"Skipping reply without active operation.");
		return;
	}

	auto & data = *operations_.back();
	int res = data.ParseResponse();
	if (res == FZ_REPLY_OK) {
		ResetOperation(FZ_REPLY_OK);
	}
	else if (res == FZ_REPLY_CONTINUE) {
		SendNextCommand();
	}
	else if (res & FZ_REPLY_DISCONNECTED) {
		DoClose(res);
	}
	else if (res & FZ_REPLY_ERROR) {
		if (data.opId == Command::connect) {
			DoClose(res | FZ_REPLY_DISCONNECTED);
		}
		else {
			ResetOperation(res);
		}
	}
}

int CStorjControlSocket::ResetOperation(int nErrorCode)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjControlSocket::ResetOperation(%d)", nErrorCode);

	if (!operations_.empty() && operations_.back()->opId == Command::connect) {
		auto &data = static_cast<CStorjConnectOpData &>(*operations_.back());
		if (data.opState == connect_init && nErrorCode & FZ_REPLY_ERROR && (nErrorCode & FZ_REPLY_CANCELED) != FZ_REPLY_CANCELED) {
			LogMessage(MessageType::Error, _("fzstorj could not be started"));
		}
	}
/*	if (!operations_.empty() && operations_.back()->opId == Command::del && !(nErrorCode & FZ_REPLY_DISCONNECTED)) {
		auto &data = static_cast<CStorjDeleteOpData &>(*operations_.back());
		if (data.needSendListing_) {
			SendDirectoryListingNotification(data.path_, false, false);
		}
	}*/

	return CControlSocket::ResetOperation(nErrorCode);
}

int CStorjControlSocket::DoClose(int nErrorCode)
{
	engine_.GetRateLimiter().RemoveObject(this);

	if (process_) {
		process_->kill();
	}

	if (input_thread_) {
		input_thread_.reset();

		auto threadEventsFilter = [&](fz::event_loop::Events::value_type const& ev) -> bool {
			if (ev.first != this) {
				return false;
			}
			else if (ev.second->derived_type() == CStorjEvent::type() || ev.second->derived_type() == StorjTerminateEvent::type()) {
				return true;
			}
			return false;
		};

		event_loop_.filter_events(threadEventsFilter);
	}
	process_.reset();
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
	if (fz::dispatch<CStorjEvent, StorjTerminateEvent>(ev, this,
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
