#include <filezilla.h>

#include "directorycache.h"
#include "resolve.h"

enum resolveStates
{
	resolve_init = 0,
	resolve_waitlistbuckets,
	resolve_id,
	resolve_waitlist
};

int CStorjResolveOpData::Send()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjResolveOpData::Send() in state %d", opState);

	switch (opState) {
	case resolve_init:
		bucket_.clear();
		if (fileId_) {
			fileId_->clear();
		}

		if (path_.empty()) {
			return FZ_REPLY_INTERNALERROR;
		}
		else if (!path_.SegmentCount()) {
			// It's the root, nothing to resolve here.

			if (fileId_ || !file_.empty()) {
				return FZ_REPLY_INTERNALERROR;
			}

			return FZ_REPLY_OK;
		}
		else if (path_.SegmentCount() != 1) {
			return FZ_REPLY_INTERNALERROR;
		}
		else {
			CDirectoryListing buckets;

			bool outdated{};
			bool found = engine_.GetDirectoryCache().Lookup(buckets, currentServer_, CServerPath(L"/"), false, outdated);
			if (found && !outdated) {
				int pos = buckets.FindFile_CmpCase(path_.GetLastSegment());
				if (pos != -1) {
					bucket_ = *buckets[pos].ownerGroup;
					LogMessage(MessageType::Debug_Info, L"Directory is in bucket %s", bucket_);
					opState = resolve_id;
					return FZ_REPLY_CONTINUE;
				}
				else {
					LogMessage(MessageType::Error, _("Bucket not found"));
					return FZ_REPLY_ERROR;
				}
			}

			opState = resolve_waitlistbuckets;
			controlSocket_.List(CServerPath(L"/"), std::wstring(), 0);

			return FZ_REPLY_CONTINUE;
		}
		break;
	case resolve_id:
		if (!fileId_) {
			return FZ_REPLY_OK;
		}
		else {
			if (file_.empty()) {
				return FZ_REPLY_INTERNALERROR;
			}
			CDirectoryListing listing;

			bool outdated{};
			bool found = engine_.GetDirectoryCache().Lookup(listing, currentServer_, path_, false, outdated);
			if (found && !outdated) {
				int pos = listing.FindFile_CmpCase(file_);
				if (pos != -1) {
					*fileId_ = *listing[pos].ownerGroup;
					LogMessage(MessageType::Debug_Info, L"File %s has id %s", path_.FormatFilename(file_), *fileId_);
					return FZ_REPLY_OK;
				}

				if (ignore_missing_file_) {
					return FZ_REPLY_OK;
				}

				LogMessage(MessageType::Error, _("File not found"));
				return FZ_REPLY_ERROR;
			}

			opState = resolve_waitlist;
			controlSocket_.List(path_, std::wstring(), 0);
			return FZ_REPLY_CONTINUE;
		}
		break;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjResolveOpData::ResolveSend()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjResolveOpData::ParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjResolveOpData::ParseResponse() in state %d", opState);
	return FZ_REPLY_INTERNALERROR;
}

int CStorjResolveOpData::SubcommandResult(int prevResult, COpData const&)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjResolveOpData::SubcommandResult() in state %d", opState);

	if (prevResult != FZ_REPLY_OK) {
		return prevResult;
	}

	switch (opState) {
	case resolve_waitlistbuckets:
		{
			CDirectoryListing buckets;

			bool outdated{};
			bool found = engine_.GetDirectoryCache().Lookup(buckets, currentServer_, CServerPath(L"/"), false, outdated);
			if (found && !outdated) {
				int pos = buckets.FindFile_CmpCase(path_.GetLastSegment());
				if (pos != -1) {
					bucket_ = *buckets[pos].ownerGroup;
					LogMessage(MessageType::Debug_Info, L"Directory is in bucket %s", bucket_);
					opState = resolve_id;
					return FZ_REPLY_CONTINUE;
				}
			}
		}
		LogMessage(MessageType::Error, _("Bucket not found"));
		return FZ_REPLY_ERROR;
	case resolve_waitlist:
		{
			CDirectoryListing listing;

			bool outdated{};
			bool found = engine_.GetDirectoryCache().Lookup(listing, currentServer_, path_, false, outdated);
			if (found && !outdated) {
				int pos = listing.FindFile_CmpCase(file_);
				if (pos != -1) {
					*fileId_ = *listing[pos].ownerGroup;
					LogMessage(MessageType::Debug_Info, L"File %s has id %s", path_.FormatFilename(file_), *fileId_);
					return FZ_REPLY_OK;
				}
				if (ignore_missing_file_) {
					return FZ_REPLY_OK;
				}
			}
		}
		LogMessage(MessageType::Error, _("File not found"));
		return FZ_REPLY_ERROR;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjResolveOpData::SubcommandResult()");
	return FZ_REPLY_INTERNALERROR;
}
