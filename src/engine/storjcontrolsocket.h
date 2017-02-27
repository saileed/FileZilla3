#ifndef FILEZILLA_ENGINE_STORJCONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_STORJCONTROLSOCKET_HEADER

#include "ControlSocket.h"

#include "../storj/events.hpp"

namespace fz {
class process;
}

class CStorjInputThread;

struct storj_message;
class CStorjControlSocket final : public CControlSocket, public CRateLimiterObject
{
public:
	CStorjControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CStorjControlSocket();

	virtual void Connect(const CServer &server) override;

	virtual void List(CServerPath const& path = CServerPath(), std::wstring const& subDir = std::wstring(), int flags = 0) override;
	virtual void FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
							 std::wstring const& remoteFile, bool download,
							 CFileTransferCommand::t_transferSettings const& transferSettings) override;
	/*
	virtual void Delete(const CServerPath& path, std::deque<std::wstring>&& files) override;
	virtual void RemoveDir(CServerPath const& path = CServerPath(), std::wstring const& subDir = std::wstring()) override;
	virtual void Mkdir(const CServerPath& path) override;
	virtual void Rename(const CRenameCommand& command) override;*/
	virtual void Cancel() override;

	virtual bool Connected() const override { return m_pInputThread != 0; }

	virtual bool SetAsyncRequestReply(CAsyncRequestNotification *pNotification) override;

protected:
	// Replaces filename"with"quotes with
	// "filename""with""quotes"
	std::wstring QuoteFilename(std::wstring const& filename);

	virtual int DoClose(int nErrorCode = FZ_REPLY_DISCONNECTED);

	virtual int ResetOperation(int nErrorCode);
	virtual int ParseSubcommandResult(int prevResult);

	void ProcessReply(int result, std::wstring const& reply);

	int ConnectParseResponse(bool successful, std::wstring const& reply);
	int ConnectSend();

	int FileTransferSubcommandResult(int prevResult);
	int FileTransferSend();
	int FileTransferParseResponse(int result, std::wstring const& reply);

	int ListSend();
	int ListParseResponse(bool successful, std::wstring const& reply);
	int ListParseEntry(std::wstring && name, std::wstring const& size, std::wstring && id);
	int ListSubcommandResult(int prevResult);
/*
	int ChangeDir(CServerPath path = CServerPath(), std::wstring subDir = std::wstring(), bool link_discovery = false);
	int ChangeDirParseResponse(bool successful, std::wstring const& reply);
	int ChangeDirSubcommandResult(int prevResult);
	int ChangeDirSend();

	int MkdirParseResponse(bool successful, std::wstring const& reply);
	int MkdirSend();

	int DeleteParseResponse(bool successful, std::wstring const& reply);
	int DeleteSend();

	int RemoveDirParseResponse(bool successful, std::wstring const& reply);

	int RenameParseResponse(bool successful, std::wstring const& reply);
	int RenameSubcommandResult(int prevResult);
	int RenameSend();*/

	bool SendCommand(std::wstring const& cmd, std::wstring const& show = std::wstring());
	bool AddToStream(std::wstring const& cmd);

	virtual void OnRateAvailable(CRateLimiter::rate_direction direction);
	void OnQuotaRequest(CRateLimiter::rate_direction direction);

	fz::process* m_pProcess{};
	CStorjInputThread* m_pInputThread{};

	virtual void operator()(fz::event_base const& ev);
	void OnStorjEvent(storj_message const& message);
	void OnTerminate(std::wstring const& error);
};

#endif
