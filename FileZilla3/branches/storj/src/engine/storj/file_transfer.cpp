#include <filezilla.h>

#include "directorycache.h"
#include "file_transfer.h"

#include <libfilezilla/local_filesys.hpp>

enum FileTransferStates
{
	filetransfer_init,
	filetransfer_resolve,
	filetransfer_transfer
};

int CStorjFileTransferOpData::Send()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjFileTransferOpData::Send() in state %d", opState);

	switch (opState) {
	case filetransfer_init:
		if (localFile_.empty()) {
			if (!download_) {
				return FZ_REPLY_CRITICALERROR | FZ_REPLY_NOTSUPPORTED;
			}
			else {
				return FZ_REPLY_SYNTAXERROR;
			}
		}

		if (download_) {
			std::wstring filename = remotePath_.FormatFilename(remoteFile_);
			LogMessage(MessageType::Status, _("Starting download of %s"), filename);
		}
		else {
			LogMessage(MessageType::Status, _("Starting upload of %s"), localFile_);
		}

		int64_t size;
		bool isLink;
		if (fz::local_filesys::get_file_info(fz::to_native(localFile_), isLink, &size, 0, 0) == fz::local_filesys::file) {
			localFileSize_ = size;
		}

		opState = filetransfer_resolve;

		if (remotePath_.GetType() == DEFAULT) {
			remotePath_.SetType(currentServer_.GetType());
		}

		controlSocket_.Resolve(remotePath_, remoteFile_, bucket_, &fileId_, !download_);
		return FZ_REPLY_CONTINUE;
	case filetransfer_transfer:

		if (!resume_) {
			controlSocket_.CreateLocalDir(localFile_);
		}

		engine_.transfer_status_.Init(remoteFileSize_, 0, false);

		engine_.transfer_status_.SetStartTime();
		transferInitiated_ = true;
		if (download_) {
			return controlSocket_.SendCommand(L"get " + bucket_ + L" " + fileId_ + L" " + controlSocket_.QuoteFilename(localFile_));
		}
		else {
			return controlSocket_.SendCommand(L"put " + bucket_ + L" " + controlSocket_.QuoteFilename(localFile_) + L" " + controlSocket_.QuoteFilename(remoteFile_));
		}


		return FZ_REPLY_WOULDBLOCK;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjFileTransferOpData::FileTransferSend()");
	return FZ_REPLY_INTERNALERROR;
}

int CStorjFileTransferOpData::ParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjFileTransferOpData::ParseResponse() in state %d", opState);

	if (opState == filetransfer_transfer) {
		return controlSocket_.result_;
	}

	LogMessage(MessageType::Debug_Warning, L"FileTransferParseResponse called at inproper time: %d", opState);
	return FZ_REPLY_INTERNALERROR;
}

int CStorjFileTransferOpData::SubcommandResult(int prevResult, COpData const&)
{
	LogMessage(MessageType::Debug_Verbose, L"CStorjFileTransferOpData::SubcommandResult() in state %d", opState);

	if (prevResult != FZ_REPLY_OK) {
		return prevResult;
	}

	switch (opState) {
	case filetransfer_resolve:
		opState = filetransfer_transfer;

		// Get remote file info
		CDirentry entry;
		bool dirDidExist;
		bool matchedCase;
		bool found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, remotePath_, remoteFile_, dirDidExist, matchedCase);
		if (found) {
			if (matchedCase && !entry.is_unsure()) {
				remoteFileSize_ = entry.size;
				if (entry.has_date()) {
					fileTime_ = entry.time;
				}
			}
		}

		int res = controlSocket_.CheckOverwriteFile();
		if (res != FZ_REPLY_OK) {
			return res;
		}

		return FZ_REPLY_CONTINUE;
	}

	LogMessage(MessageType::Debug_Warning, L"Unknown opState in CStorjFileTransferOpData::SubcommandResult()");
	return FZ_REPLY_INTERNALERROR;
}

