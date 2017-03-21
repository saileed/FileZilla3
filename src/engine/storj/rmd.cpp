#include <filezilla.h>

#include "directorycache.h"
#include "rmd.h"

enum mkdStates
{
	rmd_init = 0,
	rmd_resolve,
	rmd_rmbucket
};


int CStorjRemoveDirOpData::Send()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjRemoveDirOpData::Send() in state %d", opState);

	switch (opState) {
	case rmd_init:
		if (path_.SegmentCount() != 1) {
			LogMessage(MessageType::Error, _("Only top-level directories are supported"));
			return FZ_REPLY_NOTSUPPORTED;
		}
		controlSocket_.Resolve(path_, std::wstring(), bucket_);
		opState = rmd_resolve;
		return FZ_REPLY_CONTINUE;
	case rmd_rmbucket:
		engine_.GetDirectoryCache().InvalidateFile(currentServer_, CServerPath(L"/"), path_.GetLastSegment());

		engine_.InvalidateCurrentWorkingDirs(path_);

		return controlSocket_.SendCommand(L"rmbucket " + bucket_);
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjRemoveDirOpData::Send()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjRemoveDirOpData::ParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjRemoveDirOpData::ParseResponse() in state %d", opState);


	switch (opState) {
	case rmd_rmbucket:
		if (controlSocket_.result_ == FZ_REPLY_OK) {
			engine_.GetDirectoryCache().RemoveDir(currentServer_, CServerPath(L"/"), path_.GetLastSegment(), CServerPath());
			controlSocket_.SendDirectoryListingNotification(CServerPath(L"/"), false, false);
		}

		return controlSocket_.result_;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjRemoveDirOpData::ParseResponse()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjRemoveDirOpData::SubcommandResult(int prevResult, COpData const&)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjRemoveDirOpData::SubcommandResult() in state %d", opState);

	switch (opState) {
	case rmd_resolve:
		if (prevResult != FZ_REPLY_OK) {
			return prevResult;
		}

		opState = rmd_rmbucket;
		return FZ_REPLY_CONTINUE;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjRemoveDirOpData::SubcommandResult()");
	return FZ_REPLY_INTERNALERROR;
}
