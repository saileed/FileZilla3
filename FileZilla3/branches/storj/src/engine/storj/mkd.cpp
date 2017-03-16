#include <filezilla.h>

#include "directorycache.h"
#include "mkd.h"

enum mkdStates
{
	mkd_init = 0,
	mkd_mkbucket
};


int CStorjMkdirOpData::Send()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjMkdirOpData::Send() in state %d", opState);

	switch (opState) {
	case mkd_init:
		if (path_.SegmentCount() != 1) {
			LogMessage(MessageType::Error, _("Only top-level directories are supported"));
			return FZ_REPLY_NOTSUPPORTED;
		}
		opState = mkd_mkbucket;
		return FZ_REPLY_CONTINUE;
	case mkd_mkbucket:
		return controlSocket_.SendCommand(L"mkbucket " + path_.GetLastSegment());
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjMkdirOpData::Send()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjMkdirOpData::ParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjMkdirOpData::ParseResponse() in state %d", opState);


	switch (opState) {
	case mkd_mkbucket:
		if (controlSocket_.result_ == FZ_REPLY_OK) {
			engine_.GetDirectoryCache().UpdateFile(currentServer_, CServerPath(L"/"), path_.GetLastSegment(), true, CDirectoryCache::dir);
			controlSocket_.SendDirectoryListingNotification(CServerPath(L"/"), false, false);
		}

		return controlSocket_.result_;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjMkdirOpData::ParseResponse()");
	return FZ_REPLY_INTERNALERROR;
}
