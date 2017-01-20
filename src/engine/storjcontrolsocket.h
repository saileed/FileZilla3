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

	virtual int Connect(const CServer &server);

	virtual int List(CServerPath path = CServerPath(), std::wstring const& subDir = std::wstring(), int flags = 0);
/*	virtual int Delete(const CServerPath& path, std::deque<std::wstring>&& files);
	virtual int RemoveDir(CServerPath const& path = CServerPath(), std::wstring const& subDir = std::wstring());
	virtual int Mkdir(const CServerPath& path);
	virtual int Rename(const CRenameCommand& command);
	virtual int Chmod(const CChmodCommand& command);*/
	virtual void Cancel();

	virtual bool Connected() { return m_pInputThread != 0; }

	virtual bool SetAsyncRequestReply(CAsyncRequestNotification *pNotification);

protected:

	virtual int DoClose(int nErrorCode = FZ_REPLY_DISCONNECTED);

	virtual int ResetOperation(int nErrorCode);
	virtual int SendNextCommand();
	virtual int ParseSubcommandResult(int prevResult);

	void ProcessReply(int result, std::wstring const& reply);

	int ConnectParseResponse(bool successful, std::wstring const& reply);
	int ConnectSend();

	/*
	virtual int FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
							 std::wstring const& remoteFile, bool download,
							 CFileTransferCommand::t_transferSettings const& transferSettings);
	int FileTransferSubcommandResult(int prevResult);
	int FileTransferSend();
	int FileTransferParseResponse(int result, std::wstring const& reply);
*/
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

	int ChmodParseResponse(bool successful, std::wstring const& reply);
	int ChmodSubcommandResult(int prevResult);
	int ChmodSend();

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
