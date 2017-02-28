#include <filezilla.h>

#include "directorycache.h"
#include "delete.h"

enum DeleteStates
{
	delete_init,
	delete_resolve,
};

int CStorjDeleteOpData::Send()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjDeleteOpData::Send() in state %d", opState);

	switch (opState) {
	case delete_init:
		if (files_.empty()) {
			return FZ_REPLY_CRITICALERROR;
		}

		fileIds_.resize(1);
		controlSocket_.Resolve(path_, files_.front(), bucket_, &fileIds_[0], true);
		return FZ_REPLY_CONTINUE;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjDeleteOpData::FileTransferSend()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjDeleteOpData::ParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjDeleteOpData::ParseResponse() in state %d", opState);



	LogMessage(MessageType::Debug_Warning, L"CStorjDeleteOpData called at inproper time: %d", opState);
	return FZ_REPLY_INTERNALERROR;
}

int CStorjDeleteOpData::SubcommandResult(int prevResult, COpData const&)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjDeleteOpData::SubcommandResult() in state %d", opState);

	if (prevResult != FZ_REPLY_OK) {
		return prevResult;
	}

	switch (opState) {

	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjDeleteOpData::SubcommandResult()");
	return FZ_REPLY_INTERNALERROR;
}
