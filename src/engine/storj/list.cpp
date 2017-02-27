#include <filezilla.h>

#include "directorycache.h"
#include "list.h"

enum listStates
{
	list_init = 0,
	list_waitcwd,
	list_waitlock,
	list_list
};

int CStorjListOpData::Send()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjListOpData::Send() in state %d", opState);


	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjListOpData::ListSend()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjListOpData::ParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjListOpData::ParseResponse() in state %d", opState);


	LogMessage(MessageType::Debug_Warning, L"ListParseResponse called at inproper time: %d", opState);
	return FZ_REPLY_INTERNALERROR;
}

int CStorjListOpData::SubcommandResult(int prevResult, COpData const& previousOperation)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjListOpData::SubcommandResult() in state %d", opState);


	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjListOpData::SubcommandResult()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjListOpData::ParseEntry(std::wstring && name, std::wstring const& size, std::wstring && id)
{
	if (opState != list_list) {
		LogMessage(MessageType::Debug_Warning, L"ListParseResponse called at inproper time: %d", opState);
		return FZ_REPLY_INTERNALERROR;
	}

	CDirentry entry;
	entry.name = name;
	entry.size = fz::to_integral<int64_t>(size, -1);
	entry.ownerGroup.get() = id;
	if (bucket_.empty()) {
		entry.flags = CDirentry::flag_dir;
		entry.size = -1;
	}
	else {
		entry.flags = 0;
	}

	directoryListing_.Append(std::move(entry));

	return FZ_REPLY_WOULDBLOCK;
}
