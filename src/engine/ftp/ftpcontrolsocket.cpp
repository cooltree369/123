#include <filezilla.h>

#include "cwd.h"
#include "directorycache.h"
#include "directorylistingparser.h"
#include "engineprivate.h"
#include "externalipresolver.h"
#include "ftpcontrolsocket.h"
#include "iothread.h"
#include "list.h"
#include "logon.h"
#include "mkd.h"
#include "pathcache.h"
#include "proxy.h"
#include "rawtransfer.h"
#include "servercapabilities.h"
#include "tlssocket.h"
#include "transfersocket.h"

#include <libfilezilla/file.hpp>
#include <libfilezilla/iputils.hpp>
#include <libfilezilla/local_filesys.hpp>
#include <libfilezilla/util.hpp>

#include <algorithm>

CFtpFileTransferOpData::CFtpFileTransferOpData(bool is_download, std::wstring const& local_file, std::wstring const& remote_file, CServerPath const& remote_path)
	: CFileTransferOpData(is_download, local_file, remote_file, remote_path)
{
}

CFtpFileTransferOpData::~CFtpFileTransferOpData()
{
	if (pIOThread) {
		CIOThread *pThread = pIOThread;
		pIOThread = 0;
		pThread->Destroy();
		delete pThread;
	}
}

enum filetransferStates
{
	filetransfer_init = 0,
	filetransfer_waitcwd,
	filetransfer_waitlist,
	filetransfer_size,
	filetransfer_mdtm,
	filetransfer_resumetest,
	filetransfer_transfer,
	filetransfer_waittransfer,
	filetransfer_waitresumetest,
	filetransfer_mfmt
};

class CFtpDeleteOpData final : public COpData
{
public:
	CFtpDeleteOpData()
		: COpData(Command::del)
	{
	}

	CServerPath path;
	std::deque<std::wstring> files;
	bool omitPath{};

	// Set to fz::datetime::Now initially and after
	// sending an updated listing to the UI.
	fz::datetime m_time;

	bool m_needSendListing{};

	// Set to true if deletion of at least one file failed
	bool m_deleteFailed{};
};

CFtpControlSocket::CFtpControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	// Enable TCP_NODELAY, speeds things up a bit.
	m_pSocket->SetFlags(CSocket::flag_nodelay | CSocket::flag_keepalive);

	// Enable SO_KEEPALIVE, lots of clueless users have broken routers and
	// firewalls which terminate the control connection on long transfers.
	int v = engine_.GetOptions().GetOptionVal(OPTION_TCP_KEEPALIVE_INTERVAL);
	if (v >= 1 && v < 10000) {
		m_pSocket->SetKeepaliveInterval(fz::duration::from_minutes(v));
	}
}

CFtpControlSocket::~CFtpControlSocket()
{
	remove_handler();

	DoClose();
}

void CFtpControlSocket::OnReceive()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::OnReceive()");

	for (;;) {
		int error;
		int read = m_pBackend->Read(m_receiveBuffer + m_bufferLen, RECVBUFFERSIZE - m_bufferLen, error);

		if (read < 0) {
			if (error != EAGAIN) {
				LogMessage(MessageType::Error, _("Could not read from socket: %s"), CSocket::GetErrorDescription(error));
				if (GetCurrentCommandId() != Command::connect) {
					LogMessage(MessageType::Error, _("Disconnected from server"));
				}
				DoClose();
			}
			return;
		}

		if (!read) {
			auto messageType = (GetCurrentCommandId() == Command::none) ? MessageType::Status : MessageType::Error;
			LogMessage(messageType, _("Connection closed by server"));
			DoClose();
			return;
		}

		SetActive(CFileZillaEngine::recv);

		char* start = m_receiveBuffer;
		m_bufferLen += read;

		for (int i = start - m_receiveBuffer; i < m_bufferLen; ++i) {
			char& p = m_receiveBuffer[i];
			if (p == '\r' ||
				p == '\n' ||
				p == 0)
			{
				int len = i - (start - m_receiveBuffer);
				if (!len) {
					++start;
					continue;
				}

				p = 0;
				std::wstring line = ConvToLocal(start, i + 1 - (start - m_receiveBuffer));
				start = m_receiveBuffer + i + 1;

				ParseLine(line);

				// Abort if connection got closed
				if (!currentServer_) {
					return;
				}
			}
		}
		memmove(m_receiveBuffer, start, m_bufferLen - (start - m_receiveBuffer));
		m_bufferLen -= (start -m_receiveBuffer);
		if (m_bufferLen > MAXLINELEN) {
			m_bufferLen = MAXLINELEN;
		}
	}
}

namespace {
bool HasFeature(std::wstring const& line, std::wstring const& feature)
{
	if (line == feature) {
		return true;
	}
	return line.size() > feature.size() && line.substr(0, feature.size()) == feature && line[feature.size()] == ' ';
}
}

void CFtpControlSocket::ParseFeat(std::wstring line)
{
	fz::trim(line);
	std::wstring up = fz::str_toupper_ascii(line);

	if (HasFeature(up, L"UTF8")) {
		CServerCapabilities::SetCapability(currentServer_, utf8_command, yes);
	}
	else if (HasFeature(up, L"CLNT")) {
		CServerCapabilities::SetCapability(currentServer_, clnt_command, yes);
	}
	else if (HasFeature(up, L"MLSD")) {
		std::wstring facts;
		// FEAT output for MLST overrides MLSD
		if (CServerCapabilities::GetCapability(currentServer_, mlsd_command, &facts) != yes || facts.empty()) {
			if (line.size() > 5) {
				facts = line.substr(5);
			}
			else {
				facts.clear();
			}
		}
		CServerCapabilities::SetCapability(currentServer_, mlsd_command, yes, facts);

		// MLST/MLSD specs require use of UTC
		CServerCapabilities::SetCapability(currentServer_, timezone_offset, no);
	}
	else if (HasFeature(up, L"MLST")) {
		std::wstring facts;
		if (line.size() > 5) {
			facts = line.substr(5);
		}
		// FEAT output for MLST overrides MLSD
		if (facts.empty()) {
			if (CServerCapabilities::GetCapability(currentServer_, mlsd_command, &facts) != yes) {
				facts.clear();
			}
		}
		CServerCapabilities::SetCapability(currentServer_, mlsd_command, yes, facts);

		// MLST/MLSD specs require use of UTC
		CServerCapabilities::SetCapability(currentServer_, timezone_offset, no);
	}
	else if (HasFeature(up, L"MODE Z")) {
		CServerCapabilities::SetCapability(currentServer_, mode_z_support, yes);
	}
	else if (HasFeature(up, L"MFMT")) {
		CServerCapabilities::SetCapability(currentServer_, mfmt_command, yes);
	}
	else if (HasFeature(up, L"MDTM")) {
		CServerCapabilities::SetCapability(currentServer_, mdtm_command, yes);
	}
	else if (HasFeature(up, L"SIZE")) {
		CServerCapabilities::SetCapability(currentServer_, size_command, yes);
	}
	else if (HasFeature(up, L"TVFS")) {
		CServerCapabilities::SetCapability(currentServer_, tvfs_support, yes);
	}
	else if (HasFeature(up, L"REST STREAM")) {
		CServerCapabilities::SetCapability(currentServer_, rest_stream, yes);
	}
	else if (HasFeature(up, L"EPSV")) {
		CServerCapabilities::SetCapability(currentServer_, epsv_command, yes);
	}
}

void CFtpControlSocket::ParseLine(std::wstring line)
{
	m_rtt.Stop();
	LogMessageRaw(MessageType::Response, line);
	SetAlive();

	if (m_pCurOpData && m_pCurOpData->opId == Command::connect) {
		CFtpLogonOpData* pData = static_cast<CFtpLogonOpData *>(m_pCurOpData);
		if (pData->waitChallenge) {
			std::wstring& challenge = pData->challenge;
			if (!challenge.empty())
#ifdef FZ_WINDOWS
				challenge += L"\r\n";
#else
				challenge += L"\n";
#endif
			challenge += line;
		}
		else if (pData->opState == LOGON_FEAT) {
			ParseFeat(line);
		}
		else if (pData->opState == LOGON_WELCOME) {
			if (!pData->gotFirstWelcomeLine) {
				if (fz::str_tolower_ascii(line).substr(0, 3) == L"ssh") {
					LogMessage(MessageType::Error, _("Cannot establish FTP connection to an SFTP server. Please select proper protocol."));
					DoClose(FZ_REPLY_CRITICALERROR);
					return;
				}
				pData->gotFirstWelcomeLine = true;
			}
		}
	}
	//Check for multi-line responses
	if (line.size() > 3) {
		if (!m_MultilineResponseCode.empty()) {
			if (line.substr(0, 4) == m_MultilineResponseCode) {
				// end of multi-line found
				m_MultilineResponseCode.clear();
				m_Response = line;
				ParseResponse();
				m_Response.clear();
				m_MultilineResponseLines.clear();
			}
			else {
				m_MultilineResponseLines.push_back(line);
			}
		}
		// start of new multi-line
		else if (line[3] == '-') {
			// DDD<SP> is the end of a multi-line response
			m_MultilineResponseCode = line.substr(0, 3) + L" ";
			m_MultilineResponseLines.push_back(line);
		}
		else {
			m_Response = line;
			ParseResponse();
			m_Response.clear();
		}
	}
}

void CFtpControlSocket::OnConnect()
{
	m_lastTypeBinary = -1;

	SetAlive();

	if (currentServer_.GetProtocol() == FTPS) {
		if (!m_pTlsSocket) {
			LogMessage(MessageType::Status, _("Connection established, initializing TLS..."));

			assert(!m_pTlsSocket);
			delete m_pBackend;
			m_pTlsSocket = new CTlsSocket(this, *m_pSocket, this);
			m_pBackend = m_pTlsSocket;

			if (!m_pTlsSocket->Init()) {
				LogMessage(MessageType::Error, _("Failed to initialize TLS."));
				DoClose();
				return;
			}

			int res = m_pTlsSocket->Handshake();
			if (res == FZ_REPLY_ERROR) {
				DoClose();
			}

			return;
		}
		else {
			LogMessage(MessageType::Status, _("TLS connection established, waiting for welcome message..."));
		}
	}
	else if ((currentServer_.GetProtocol() == FTPES || currentServer_.GetProtocol() == FTP) && m_pTlsSocket) {
		LogMessage(MessageType::Status, _("TLS connection established."));
		SendNextCommand();
		return;
	}
	else {
		LogMessage(MessageType::Status, _("Connection established, waiting for welcome message..."));
	}
	m_pendingReplies = 1;
	m_repliesToSkip = 0;
}

void CFtpControlSocket::ParseResponse()
{
	if (m_Response.empty()) {
		LogMessage(MessageType::Debug_Warning, L"No reply in ParseResponse");
		return;
	}

	if (m_Response[0] != '1') {
		if (m_pendingReplies > 0) {
			m_pendingReplies--;
		}
		else {
			LogMessage(MessageType::Debug_Warning, L"Unexpected reply, no reply was pending.");
			return;
		}
	}

	if (m_repliesToSkip) {
		LogMessage(MessageType::Debug_Info, L"Skipping reply after cancelled operation or keepalive command.");
		if (m_Response[0] != '1') {
			--m_repliesToSkip;
		}

		if (!m_repliesToSkip) {
			SetWait(false);
			if (!m_pCurOpData) {
				StartKeepaliveTimer();
			}
			else if (!m_pendingReplies) {
				SendNextCommand();
			}
		}

		return;
	}

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"Skipping reply without active operation.");
		return;
	}

	int res = m_pCurOpData->ParseResponse();
	if (res == FZ_REPLY_OK) {
		ResetOperation(FZ_REPLY_OK);
	}
	else if (res == FZ_REPLY_CONTINUE) {
		SendNextCommand();
	}
	else if (res & FZ_REPLY_ERROR) {
		if (m_pCurOpData->opId == Command::connect) {
			DoClose(res | FZ_REPLY_DISCONNECTED);
		}
		else if ((res & FZ_REPLY_DISCONNECTED) == FZ_REPLY_DISCONNECTED) {
			DoClose(res);
		}
		else {
			ResetOperation(res);
		}
	}
}

int CFtpControlSocket::GetReplyCode() const
{
	if (m_Response.empty()) {
		return 0;
	}
	else if (m_Response[0] < '0' || m_Response[0] > '9') {
		return 0;
	}
	else {
		return m_Response[0] - '0';
	}
}

bool CFtpControlSocket::SendCommand(std::wstring const& str, bool maskArgs, bool measureRTT)
{
	size_t pos;
	if (maskArgs && (pos = str.find(' ')) != std::wstring::npos) {
		std::wstring stars(str.size() - pos - 1, '*');
		LogMessageRaw(MessageType::Command, str.substr(0, pos + 1) + stars);
	}
	else {
		LogMessageRaw(MessageType::Command, str);
	}

	std::string buffer = ConvToServer(str);
	if (buffer.empty()) {
		LogMessage(MessageType::Error, _("Failed to convert command to 8 bit charset"));
		ResetOperation(FZ_REPLY_ERROR);
		return false;
	}
	buffer += "\r\n";
	bool res = CRealControlSocket::Send(buffer.c_str(), buffer.size());
	if (res) {
		++m_pendingReplies;
	}

	if (measureRTT) {
		m_rtt.Start();
	}

	return res;
}

void CFtpControlSocket::List(CServerPath const& path, std::wstring const& subDir, int flags)
{
	if (m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"List called from other command");
	}

	CServerPath newPath = m_CurrentPath;
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

	CFtpListOpData *pData = new CFtpListOpData(*this, path, subDir, flags);
	Push(pData);
}

int CFtpControlSocket::ResetOperation(int nErrorCode)
{
 	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::ResetOperation(%d)", nErrorCode);

	m_pTransferSocket.reset();
	m_pIPResolver.reset();

	m_repliesToSkip = m_pendingReplies;

	if (m_pCurOpData && m_pCurOpData->opId == Command::transfer) {
		CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);
		if (pData->tranferCommandSent) {
			if (pData->transferEndReason == TransferEndReason::transfer_failure_critical) {
				nErrorCode |= FZ_REPLY_CRITICALERROR | FZ_REPLY_WRITEFAILED;
			}
			if (pData->transferEndReason != TransferEndReason::transfer_command_failure_immediate || GetReplyCode() != 5) {
				pData->transferInitiated = true;
			}
			else {
				if (nErrorCode == FZ_REPLY_ERROR) {
					nErrorCode |= FZ_REPLY_CRITICALERROR;
				}
			}
		}
		if (nErrorCode != FZ_REPLY_OK && pData->download && !pData->fileDidExist) {
			delete pData->pIOThread;
			pData->pIOThread = 0;
			int64_t size;
			bool isLink;
			if (fz::local_filesys::get_file_info(fz::to_native(pData->localFile), isLink, &size, 0, 0) == fz::local_filesys::file && size == 0) {
				// Download failed and a new local file was created before, but
				// nothing has been written to it. Remove it again, so we don't
				// leave a bunch of empty files all over the place.
				LogMessage(MessageType::Debug_Verbose, L"Deleting empty file");
				fz::remove_file(fz::to_native(pData->localFile));
			}
		}
	}
	if (m_pCurOpData && m_pCurOpData->opId == Command::del && !(nErrorCode & FZ_REPLY_DISCONNECTED)) {
		CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);
		if (pData->m_needSendListing) {
			SendDirectoryListingNotification(pData->path, false, false);
		}
	}

	if (m_pCurOpData && m_pCurOpData->opId == Command::rawtransfer &&
		nErrorCode != FZ_REPLY_OK)
	{
		CFtpRawTransferOpData *pData = static_cast<CFtpRawTransferOpData *>(m_pCurOpData);
		if (pData->pOldData->transferEndReason == TransferEndReason::successful) {
			if ((nErrorCode & FZ_REPLY_TIMEOUT) == FZ_REPLY_TIMEOUT) {
				pData->pOldData->transferEndReason = TransferEndReason::timeout;
			}
			else if (!pData->pOldData->tranferCommandSent) {
				pData->pOldData->transferEndReason = TransferEndReason::pre_transfer_command_failure;
			}
			else {
				pData->pOldData->transferEndReason = TransferEndReason::failure;
			}
		}
	}

	m_lastCommandCompletionTime = fz::monotonic_clock::now();
	if (m_pCurOpData && !(nErrorCode & FZ_REPLY_DISCONNECTED)) {
		StartKeepaliveTimer();
	}
	else {
		stop_timer(m_idleTimer);
		m_idleTimer = 0;
	}

	return CControlSocket::ResetOperation(nErrorCode);
}

int CFtpControlSocket::SendNextCommand()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::SendNextCommand()");
	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Warning, L"SendNextCommand called without active operation");
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	while (m_pCurOpData) {
		if (m_pCurOpData->waitForAsyncRequest) {
			LogMessage(MessageType::Debug_Info, L"Waiting for async request, ignoring SendNextCommand...");
			return FZ_REPLY_WOULDBLOCK;
		}

		if (m_repliesToSkip) {
			LogMessage(MessageType::Status, L"Waiting for replies to skip before sending next command...");
			SetWait(true);
			return FZ_REPLY_WOULDBLOCK;
		}

		int res = m_pCurOpData->Send();
		if (res != FZ_REPLY_CONTINUE) {
			if (res == FZ_REPLY_OK) {
				return ResetOperation(res);
			}
			else if ((res & FZ_REPLY_DISCONNECTED) == FZ_REPLY_DISCONNECTED) {
				return DoClose(res);
			}
			else if (res & FZ_REPLY_ERROR) {
				ResetOperation(res);
			}
			else if (res == FZ_REPLY_WOULDBLOCK) {
				return FZ_REPLY_WOULDBLOCK;
			}
			else if (res != FZ_REPLY_CONTINUE) {
				LogMessage(MessageType::Debug_Warning, L"Unknown result %d returned by m_pCurOpData->Send()");
				return ResetOperation(FZ_REPLY_INTERNALERROR);
			}
		}
	}

	return FZ_REPLY_OK;
}

void CFtpControlSocket::ChangeDir(CServerPath const& path, std::wstring const& subDir, bool link_discovery)
{
	cwdStates state = cwd_init;

	CFtpChangeDirOpData *pData = new CFtpChangeDirOpData(*this);
	pData->opState = state;
	pData->path = path;
	pData->subDir = subDir;
	pData->link_discovery = link_discovery;

	if (pData->pNextOpData && pData->pNextOpData->opId == Command::transfer &&
		!static_cast<CFtpFileTransferOpData *>(pData->pNextOpData)->download)
	{
		pData->tryMkdOnFail = true;
		assert(subDir.empty());
	}

	Push(pData);
}

int CFtpControlSocket::FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
									std::wstring const& remoteFile, bool download,
									CFileTransferCommand::t_transferSettings const& transferSettings)
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::FileTransfer()");

	if (localFile.empty()) {
		if (!download) {
			ResetOperation(FZ_REPLY_CRITICALERROR | FZ_REPLY_NOTSUPPORTED);
		}
		else {
			ResetOperation(FZ_REPLY_SYNTAXERROR);
		}
		return FZ_REPLY_ERROR;
	}

	if (download) {
		std::wstring filename = remotePath.FormatFilename(remoteFile);
		LogMessage(MessageType::Status, _("Starting download of %s"), filename);
	}
	else {
		LogMessage(MessageType::Status, _("Starting upload of %s"), localFile);
	}
	if (m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"deleting nonzero pData");
		delete m_pCurOpData;
	}

	CFtpFileTransferOpData *pData = new CFtpFileTransferOpData(download, localFile, remoteFile, remotePath);
	Push(pData);

	pData->transferSettings = transferSettings;
	pData->binary = transferSettings.binary;

	int64_t size;
	bool isLink;
	if (fz::local_filesys::get_file_info(fz::to_native(pData->localFile), isLink, &size, 0, 0) == fz::local_filesys::file) {
		pData->localFileSize = size;
	}

	pData->opState = filetransfer_waitcwd;

	if (pData->remotePath.GetType() == DEFAULT) {
		pData->remotePath.SetType(currentServer_.GetType());
	}

	ChangeDir(pData->remotePath);
	return FZ_REPLY_CONTINUE;
}

int CFtpControlSocket::FileTransferParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"FileTransferParseResponse()");

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);
	if (pData->opState == filetransfer_init) {
		return FZ_REPLY_ERROR;
	}

	int code = GetReplyCode();
	bool error = false;
	switch (pData->opState)
	{
	case filetransfer_size:
		if (code != 2 && code != 3) {
			if (CServerCapabilities::GetCapability(currentServer_, size_command) == yes ||
				fz::str_tolower_ascii(m_Response.substr(4)) == L"file not found" ||
				(fz::str_tolower_ascii(pData->remotePath.FormatFilename(pData->remoteFile)).find(L"file not found") == std::wstring::npos &&
				 fz::str_tolower_ascii(m_Response).find(L"file not found") != std::wstring::npos))
			{
				// Server supports SIZE command but command failed. Most likely MDTM will fail as well, so
				// skip it.
				pData->opState = filetransfer_resumetest;

				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK) {
					return res;
				}
			}
			else {
				pData->opState = filetransfer_mdtm;
			}
		}
		else {
			pData->opState = filetransfer_mdtm;
			if (m_Response.substr(0, 4) == L"213 " && m_Response.size() > 4) {
				if (CServerCapabilities::GetCapability(currentServer_, size_command) == unknown) {
					CServerCapabilities::SetCapability(currentServer_, size_command, yes);
				}
				std::wstring str = m_Response.substr(4);
				int64_t size = 0;
				for (auto c : str) {
					if (c < '0' || c > '9') {
						break;
					}

					size *= 10;
					size += c - '0';
				}
				pData->remoteFileSize = size;
			}
			else {
				LogMessage(MessageType::Debug_Info, L"Invalid SIZE reply");
			}
		}
		break;
	case filetransfer_mdtm:
		pData->opState = filetransfer_resumetest;
		if (m_Response.substr(0, 4) == L"213 " && m_Response.size() > 16) {
			pData->fileTime = fz::datetime(m_Response.substr(4), fz::datetime::utc);
			if (!pData->fileTime.empty()) {
				pData->fileTime += fz::duration::from_minutes(currentServer_.GetTimezoneOffset());
			}
		}

		{
			int res = CheckOverwriteFile();
			if (res != FZ_REPLY_OK) {
				return res;
			}
		}

		break;
	case filetransfer_mfmt:
		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	default:
		LogMessage(MessageType::Debug_Warning, L"Unknown op state");
		error = true;
		break;
	}

	if (error) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CFtpControlSocket::FileTransferSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, L"FileTransferSubcommandResult()");

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"  empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);

	if (pData->opState == filetransfer_waitcwd) {
		if (prevResult == FZ_REPLY_OK) {
			CDirentry entry;
			bool dirDidExist;
			bool matchedCase;
			bool found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, pData->tryAbsolutePath ? pData->remotePath : m_CurrentPath, pData->remoteFile, dirDidExist, matchedCase);
			if (!found) {
				if (!dirDidExist)
					pData->opState = filetransfer_waitlist;
				else if (pData->download &&
					engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
					CServerCapabilities::GetCapability(currentServer_, mdtm_command) == yes)
				{
					pData->opState = filetransfer_mdtm;
				}
				else {
					pData->opState = filetransfer_resumetest;
				}
			}
			else {
				if (entry.is_unsure()) {
					pData->opState = filetransfer_waitlist;
				}
				else {
					if (matchedCase) {
						pData->remoteFileSize = entry.size;
						if (entry.has_date()) {
							pData->fileTime = entry.time;
						}

						if (pData->download &&
							!entry.has_time() &&
							engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
							CServerCapabilities::GetCapability(currentServer_, mdtm_command) == yes)
						{
							pData->opState = filetransfer_mdtm;
						}
						else {
							pData->opState = filetransfer_resumetest;
						}
					}
					else {
						pData->opState = filetransfer_size;
					}
				}
			}
			if (pData->opState == filetransfer_waitlist) {
				List(CServerPath(), L"", LIST_FLAG_REFRESH);
				return FZ_REPLY_CONTINUE;
			}
			else if (pData->opState == filetransfer_resumetest) {
				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK) {
					return res;
				}
			}
		}
		else {
			pData->tryAbsolutePath = true;
			pData->opState = filetransfer_size;
		}
	}
	else if (pData->opState == filetransfer_waitlist) {
		if (prevResult == FZ_REPLY_OK) {
			CDirentry entry;
			bool dirDidExist;
			bool matchedCase;
			bool found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, pData->tryAbsolutePath ? pData->remotePath : m_CurrentPath, pData->remoteFile, dirDidExist, matchedCase);
			if (!found) {
				if (!dirDidExist) {
					pData->opState = filetransfer_size;
				}
				else if (pData->download &&
					engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
					CServerCapabilities::GetCapability(currentServer_, mdtm_command) == yes)
				{
					pData->opState = filetransfer_mdtm;
				}
				else {
					pData->opState = filetransfer_resumetest;
				}
			}
			else {
				if (matchedCase && !entry.is_unsure()) {
					pData->remoteFileSize = entry.size;
					if (entry.has_date()) {
						pData->fileTime = entry.time;
					}

					if (pData->download &&
						!entry.has_time() &&
						engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
						CServerCapabilities::GetCapability(currentServer_, mdtm_command) == yes)
					{
						pData->opState = filetransfer_mdtm;
					}
					else {
						pData->opState = filetransfer_resumetest;
					}
				}
				else {
					pData->opState = filetransfer_size;
				}
			}

			if (pData->opState == filetransfer_resumetest) {
				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK) {
					return res;
				}
			}
		}
		else {
			pData->opState = filetransfer_size;
		}
	}
	else if (pData->opState == filetransfer_waittransfer) {
		if (prevResult == FZ_REPLY_OK && engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS)) {
			if (!pData->download &&
				CServerCapabilities::GetCapability(currentServer_, mfmt_command) == yes)
			{
				fz::datetime mtime = fz::local_filesys::get_modification_time(fz::to_native(pData->localFile));
				if (!mtime.empty()) {
					pData->fileTime = mtime;
					pData->opState = filetransfer_mfmt;
					return SendNextCommand();
				}
			}
			else if (pData->download && !pData->fileTime.empty()) {
				delete pData->pIOThread;
				pData->pIOThread = 0;
				if (!fz::local_filesys::set_modification_time(fz::to_native(pData->localFile), pData->fileTime)) {
					LogMessage(MessageType::Debug_Warning, L"Could not set modification time");
				}
			}
		}
		ResetOperation(prevResult);
		return prevResult;
	}
	else if (pData->opState == filetransfer_waitresumetest) {
		if (prevResult != FZ_REPLY_OK) {
			if (pData->transferEndReason == TransferEndReason::failed_resumetest) {
				if (pData->localFileSize > (1ll << 32)) {
					CServerCapabilities::SetCapability(currentServer_, resume4GBbug, yes);
					LogMessage(MessageType::Error, _("Server does not support resume of files > 4GB."));
				}
				else {
					CServerCapabilities::SetCapability(currentServer_, resume2GBbug, yes);
					LogMessage(MessageType::Error, _("Server does not support resume of files > 2GB."));
				}

				ResetOperation(prevResult | FZ_REPLY_CRITICALERROR);
				return FZ_REPLY_ERROR;
			}
			else {
				ResetOperation(prevResult);
			}
			return prevResult;
		}
		if (pData->localFileSize > (1ll << 32)) {
			CServerCapabilities::SetCapability(currentServer_, resume4GBbug, no);
		}
		else {
			CServerCapabilities::SetCapability(currentServer_, resume2GBbug, no);
		}

		pData->opState = filetransfer_transfer;
	}

	return SendNextCommand();
}

int CFtpControlSocket::FileTransferSend()
{
	LogMessage(MessageType::Debug_Verbose, L"FileTransferSend()");

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);

	std::wstring cmd;
	switch (pData->opState)
	{
	case filetransfer_size:
		cmd = L"SIZE ";
		cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);
		break;
	case filetransfer_mdtm:
		cmd = L"MDTM ";
		cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);
		break;
	case filetransfer_resumetest:
	case filetransfer_transfer:
		if (m_pTransferSocket) {
			LogMessage(MessageType::Debug_Verbose, L"m_pTransferSocket != 0");
			m_pTransferSocket.reset();
		}

		{
			auto pFile = std::make_unique<fz::file>();
			if (pData->download) {
				int64_t startOffset = 0;

				// Potentially racy
				bool didExist = fz::local_filesys::get_file_type(fz::to_native(pData->localFile)) != fz::local_filesys::unknown;

				if (pData->resume) {
					if (!pFile->open(fz::to_native(pData->localFile), fz::file::writing, fz::file::existing)) {
						LogMessage(MessageType::Error, _("Failed to open \"%s\" for appending/writing"), pData->localFile);
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					pData->fileDidExist = didExist;

					startOffset = pFile->seek(0, fz::file::end);

					if (startOffset == -1) {
						LogMessage(MessageType::Error, _("Could not seek to the end of the file"));
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}
					pData->localFileSize = startOffset;

					// Check resume capabilities
					if (pData->opState == filetransfer_resumetest) {
						int res = FileTransferTestResumeCapability();
						if ((res & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED) {
							// Server does not support resume but remote and local filesizes are equal
							return FZ_REPLY_OK;
						}
						if (res != FZ_REPLY_OK) {
							return res;
						}
					}
				}
				else {
					CreateLocalDir(pData->localFile);

					if (!pFile->open(fz::to_native(pData->localFile), fz::file::writing, fz::file::empty)) {
						LogMessage(MessageType::Error, _("Failed to open \"%s\" for writing"), pData->localFile);
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					pData->fileDidExist = didExist;
					pData->localFileSize = 0;
				}

				pData->resumeOffset = pData->resume ? pData->localFileSize : 0;

				engine_.transfer_status_.Init(pData->remoteFileSize, startOffset, false);

				if (engine_.GetOptions().GetOptionVal(OPTION_PREALLOCATE_SPACE)) {
					// Try to preallocate the file in order to reduce fragmentation
					int64_t sizeToPreallocate = pData->remoteFileSize - startOffset;
					if (sizeToPreallocate > 0) {
						LogMessage(MessageType::Debug_Info, L"Preallocating %d bytes for the file \"%s\"", sizeToPreallocate, pData->localFile);
						auto oldPos = pFile->seek(0, fz::file::current);
						if (oldPos >= 0) {
							if (pFile->seek(sizeToPreallocate, fz::file::end) == pData->remoteFileSize) {
								if (!pFile->truncate()) {
									LogMessage(MessageType::Debug_Warning, L"Could not preallocate the file");
								}
							}
							pFile->seek(oldPos, fz::file::begin);
						}
					}
				}
			}
			else {
				if (!pFile->open(fz::to_native(pData->localFile), fz::file::reading)) {
					LogMessage(MessageType::Error, _("Failed to open \"%s\" for reading"), pData->localFile);
					ResetOperation(FZ_REPLY_ERROR);
					return FZ_REPLY_ERROR;
				}

				int64_t startOffset;
				if (pData->resume) {
					if (pData->remoteFileSize > 0) {
						startOffset = pData->remoteFileSize;

						if (pData->localFileSize < 0) {
							auto s = pFile->size();
							if (s >= 0) {
								pData->localFileSize = s;
							}
						}

						if (startOffset == pData->localFileSize && pData->binary) {
							LogMessage(MessageType::Debug_Info, L"No need to resume, remote file size matches local file size.");

							if (engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
								CServerCapabilities::GetCapability(currentServer_, mfmt_command) == yes)
							{
								fz::datetime mtime = fz::local_filesys::get_modification_time(fz::to_native(pData->localFile));
								if (!mtime.empty()) {
									pData->fileTime = mtime;
									pData->opState = filetransfer_mfmt;
									return SendNextCommand();
								}
							}
							ResetOperation(FZ_REPLY_OK);
							return FZ_REPLY_OK;
						}

						// Assume native 64 bit type exists
						if (pFile->seek(startOffset, fz::file::begin) == -1) {
							std::wstring const s = std::to_wstring(startOffset);
							LogMessage(MessageType::Error, _("Could not seek to offset %s within file"), s);
							ResetOperation(FZ_REPLY_ERROR);
							return FZ_REPLY_ERROR;
						}
					}
					else {
						startOffset = 0;
					}
				}
				else {
					startOffset = 0;
				}

				if (CServerCapabilities::GetCapability(currentServer_, rest_stream) == yes) {
					// Use REST + STOR if resuming
					pData->resumeOffset = startOffset;
				}
				else {
					// Play it safe, use APPE if resuming
					pData->resumeOffset = 0;
				}

				auto len = pFile->size();
				engine_.transfer_status_.Init(len, startOffset, false);
			}
			pData->pIOThread = new CIOThread;
			if (!pData->pIOThread->Create(engine_.GetThreadPool(), std::move(pFile), !pData->download, pData->binary)) {
				// CIOThread will delete pFile
				delete pData->pIOThread;
				pData->pIOThread = 0;
				LogMessage(MessageType::Error, _("Could not spawn IO thread"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}

		m_pTransferSocket = std::make_unique<CTransferSocket>(engine_, *this, pData->download ? TransferMode::download : TransferMode::upload);
		m_pTransferSocket->m_binaryMode = pData->transferSettings.binary;
		m_pTransferSocket->SetIOThread(pData->pIOThread);

		if (pData->download) {
			cmd = L"RETR ";
		}
		else if (pData->resume) {
			if (CServerCapabilities::GetCapability(currentServer_, rest_stream) == yes) {
				cmd = L"STOR "; // In this case REST gets sent since resume offset was set earlier
			}
			else {
				assert(pData->resumeOffset == 0);
				cmd = L"APPE ";
			}
		}
		else {
			cmd = L"STOR ";
		}
		cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);

		pData->opState = filetransfer_waittransfer;
		Transfer(cmd, pData);
		return FZ_REPLY_CONTINUE;
	case filetransfer_mfmt:
		{
			cmd = L"MFMT ";
			fz::datetime t = pData->fileTime;
			t -= fz::duration::from_minutes(currentServer_.GetTimezoneOffset());
			cmd += t.format(L"%Y%m%d%H%M%S ", fz::datetime::utc);
			cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);

			break;
		}
	default:
		LogMessage(MessageType::Debug_Warning, L"Unhandled opState: %d", pData->opState);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (!cmd.empty()) {
		if (!SendCommand(cmd)) {
			return FZ_REPLY_ERROR;
		}
	}

	return FZ_REPLY_WOULDBLOCK;
}

void CFtpControlSocket::TransferEnd()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::TransferEnd()");

	// If m_pTransferSocket is zero, the message was sent by the previous command.
	// We can safely ignore it.
	// It does not cause problems, since before creating the next transfer socket, other
	// messages which were added to the queue later than this one will be processed first.
	if (!m_pCurOpData || !m_pTransferSocket || GetCurrentCommandId() != Command::rawtransfer) {
		LogMessage(MessageType::Debug_Verbose, L"Call to TransferEnd at unusual time, ignoring");
		return;
	}

	TransferEndReason reason = m_pTransferSocket->GetTransferEndreason();
	if (reason == TransferEndReason::none) {
		LogMessage(MessageType::Debug_Info, L"Call to TransferEnd at unusual time");
		return;
	}

	if (reason == TransferEndReason::successful) {
		SetAlive();
	}

	CFtpRawTransferOpData *pData = static_cast<CFtpRawTransferOpData *>(m_pCurOpData);
	if (pData->pOldData->transferEndReason == TransferEndReason::successful) {
		pData->pOldData->transferEndReason = reason;
	}

	switch (m_pCurOpData->opState)
	{
	case rawtransfer_transfer:
		pData->opState = rawtransfer_waittransferpre;
		break;
	case rawtransfer_waitfinish:
		pData->opState = rawtransfer_waittransfer;
		break;
	case rawtransfer_waitsocket:
		ResetOperation((reason == TransferEndReason::successful) ? FZ_REPLY_OK : FZ_REPLY_ERROR);
		break;
	default:
		LogMessage(MessageType::Debug_Info, L"TransferEnd at unusual op state %d, ignoring", m_pCurOpData->opState);
		break;
	}
}

bool CFtpControlSocket::SetAsyncRequestReply(CAsyncRequestNotification *pNotification)
{
	if (m_pCurOpData) {
		if (!m_pCurOpData->waitForAsyncRequest) {
			LogMessage(MessageType::Debug_Info, L"Not waiting for request reply, ignoring request reply %d", pNotification->GetRequestID());
			return false;
		}
		m_pCurOpData->waitForAsyncRequest = false;
	}

	const RequestId requestId = pNotification->GetRequestID();
	switch (requestId)
	{
	case reqId_fileexists:
		{
			if (!m_pCurOpData || m_pCurOpData->opId != Command::transfer) {
				LogMessage(MessageType::Debug_Info, L"No or invalid operation in progress, ignoring request reply %d", pNotification->GetRequestID());
				return false;
			}

			CFileExistsNotification *pFileExistsNotification = static_cast<CFileExistsNotification *>(pNotification);
			return SetFileExistsAction(pFileExistsNotification);
		}
		break;
	case reqId_interactiveLogin:
		{
			if (!m_pCurOpData || m_pCurOpData->opId != Command::connect) {
				LogMessage(MessageType::Debug_Info, L"No or invalid operation in progress, ignoring request reply %d", pNotification->GetRequestID());
				return false;
			}

			CFtpLogonOpData* pData = static_cast<CFtpLogonOpData*>(m_pCurOpData);

			CInteractiveLoginNotification *pInteractiveLoginNotification = static_cast<CInteractiveLoginNotification *>(pNotification);
			if (!pInteractiveLoginNotification->passwordSet) {
				ResetOperation(FZ_REPLY_CANCELED);
				return false;
			}
			currentServer_.SetUser(currentServer_.GetUser(), pInteractiveLoginNotification->server.GetPass());
			pData->gotPassword = true;
			SendNextCommand();
		}
		break;
	case reqId_certificate:
		{
			if (!m_pTlsSocket || m_pTlsSocket->GetState() != CTlsSocket::TlsState::verifycert) {
				LogMessage(MessageType::Debug_Info, L"No or invalid operation in progress, ignoring request reply %d", pNotification->GetRequestID());
				return false;
			}

			CCertificateNotification* pCertificateNotification = static_cast<CCertificateNotification *>(pNotification);
			m_pTlsSocket->TrustCurrentCert(pCertificateNotification->m_trusted);

			if (!pCertificateNotification->m_trusted) {
				DoClose(FZ_REPLY_CRITICALERROR);
				return false;
			}

			if (m_pCurOpData && m_pCurOpData->opId == Command::connect &&
				m_pCurOpData->opState == LOGON_AUTH_WAIT)
			{
				m_pCurOpData->opState = LOGON_LOGON;
			}
		}
		break;
	default:
		LogMessage(MessageType::Debug_Warning, L"Unknown request %d", pNotification->GetRequestID());
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	return true;
}

class CRawCommandOpData final : public COpData
{
public:
	CRawCommandOpData(std::wstring const& command)
		: COpData(Command::raw)
	{
		m_command = command;
	}

	std::wstring m_command;
};

int CFtpControlSocket::RawCommand(std::wstring const& command)
{
	assert(!command.empty());

	Push(new CRawCommandOpData(command));

	return SendNextCommand();
}

int CFtpControlSocket::RawCommandSend()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::RawCommandSend");

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	engine_.GetDirectoryCache().InvalidateServer(currentServer_);
	engine_.GetPathCache().InvalidateServer(currentServer_);
	m_CurrentPath.clear();

	m_lastTypeBinary = -1;

	CRawCommandOpData *pData = static_cast<CRawCommandOpData *>(m_pCurOpData);

	if (!SendCommand(pData->m_command, false, false)) {
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::RawCommandParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::RawCommandParseResponse");

	int code = GetReplyCode();
	if (code == 2 || code == 3) {
		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	}
	else {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
}

int CFtpControlSocket::Delete(const CServerPath& path, std::deque<std::wstring>&& files)
{
	assert(!m_pCurOpData);
	CFtpDeleteOpData *pData = new CFtpDeleteOpData();
	Push(pData);
	pData->path = path;
	pData->files = files;
	pData->omitPath = true;

	ChangeDir(pData->path);
	return FZ_REPLY_CONTINUE;
}

int CFtpControlSocket::DeleteSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::DeleteSubcommandResult()");

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"  empty m_pCurOpData");
		return FZ_REPLY_INTERNALERROR;
	}

	CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);

	if (prevResult != FZ_REPLY_OK) {
		pData->omitPath = false;
	}

	return SendNextCommand();
}

int CFtpControlSocket::DeleteSend()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::DeleteSend");

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"Empty m_pCurOpData");
		return FZ_REPLY_INTERNALERROR;
	}
	CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);

	std::wstring const& file = pData->files.front();
	if (file.empty()) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, L"Empty filename");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	std::wstring filename = pData->path.FormatFilename(file, pData->omitPath);
	if (filename.empty()) {
		LogMessage(MessageType::Error, _("Filename cannot be constructed for directory %s and filename %s"), pData->path.GetPath(), file);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (pData->m_time.empty()) {
		pData->m_time = fz::datetime::now();
	}

	engine_.GetDirectoryCache().InvalidateFile(currentServer_, pData->path, file);

	if (!SendCommand(L"DELE " + filename)) {
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::DeleteParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::DeleteParseResponse()");

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);

	int code = GetReplyCode();
	if (code != 2 && code != 3) {
		pData->m_deleteFailed = true;
	}
	else {
		std::wstring const& file = pData->files.front();

		engine_.GetDirectoryCache().RemoveFile(currentServer_, pData->path, file);

		fz::datetime now = fz::datetime::now();
		if (!pData->m_time.empty() && (now - pData->m_time).get_seconds() >= 1) {
			SendDirectoryListingNotification(pData->path, false, false);
			pData->m_time = now;
			pData->m_needSendListing = false;
		}
		else {
			pData->m_needSendListing = true;
		}
	}

	pData->files.pop_front();

	if (!pData->files.empty()) {
		return SendNextCommand();
	}

	return ResetOperation(pData->m_deleteFailed ? FZ_REPLY_ERROR : FZ_REPLY_OK);
}

class CFtpRemoveDirOpData final : public COpData
{
public:
	CFtpRemoveDirOpData()
		: COpData(Command::removedir)
		, omitPath()
	{
	}

	CServerPath path;
	CServerPath fullPath;
	std::wstring subDir;
	bool omitPath;
};

int CFtpControlSocket::RemoveDir(const CServerPath& path, std::wstring const& subDir)
{
	assert(!m_pCurOpData);
	CFtpRemoveDirOpData *pData = new CFtpRemoveDirOpData();
	Push(pData);
	pData->path = path;
	pData->subDir = subDir;
	pData->omitPath = true;
	pData->fullPath = path;

	if (!pData->fullPath.AddSegment(subDir)) {
		LogMessage(MessageType::Error, _("Path cannot be constructed for directory %s and subdir %s"), path.GetPath(), subDir);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	ChangeDir(pData->path);
	return FZ_REPLY_CONTINUE;
}

int CFtpControlSocket::RemoveDirSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::RemoveDirSubcommandResult()");

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpRemoveDirOpData *pData = static_cast<CFtpRemoveDirOpData *>(m_pCurOpData);

	if (prevResult != FZ_REPLY_OK) {
		pData->omitPath = false;
	}
	else {
		pData->path = m_CurrentPath;
	}

	return SendNextCommand();
}

int CFtpControlSocket::RemoveDirSend()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::RemoveDirSend()");

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpRemoveDirOpData *pData = static_cast<CFtpRemoveDirOpData *>(m_pCurOpData);

	engine_.GetDirectoryCache().InvalidateFile(currentServer_, pData->path, pData->subDir);

	CServerPath path(engine_.GetPathCache().Lookup(currentServer_, pData->path, pData->subDir));
	if (path.empty()) {
		path = pData->path;
		path.AddSegment(pData->subDir);
	}
	engine_.InvalidateCurrentWorkingDirs(path);

	engine_.GetPathCache().InvalidatePath(currentServer_, pData->path, pData->subDir);

	if (pData->omitPath) {
		if (!SendCommand(L"RMD " + pData->subDir)) {
			return FZ_REPLY_ERROR;
		}
	}
	else {
		if (!SendCommand(L"RMD " + pData->fullPath.GetPath())) {
			return FZ_REPLY_ERROR;
		}
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::RemoveDirParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::RemoveDirParseResponse()");

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, L"Empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpRemoveDirOpData *pData = static_cast<CFtpRemoveDirOpData *>(m_pCurOpData);

	int code = GetReplyCode();
	if (code != 2 && code != 3) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	engine_.GetDirectoryCache().RemoveDir(currentServer_, pData->path, pData->subDir, engine_.GetPathCache().Lookup(currentServer_, pData->path, pData->subDir));
	SendDirectoryListingNotification(pData->path, false, false);

	return ResetOperation(FZ_REPLY_OK);
}

int CFtpControlSocket::Mkdir(CServerPath const& path)
{
	/* Directory creation works like this: First find a parent directory into
	 * which we can CWD, then create the subdirs one by one. If either part
	 * fails, try MKD with the full path directly.
	 */

	if (!m_pCurOpData && !path.empty()) {
		LogMessage(MessageType::Status, _("Creating directory '%s'..."), path.GetPath());
	}

	CMkdirOpData *pData = new CMkdirOpData;
	pData->path = path;

	Push(pData);
	return FZ_REPLY_CONTINUE;
}

class CFtpRenameOpData final : public COpData
{
public:
	CFtpRenameOpData(CRenameCommand const& command)
		: COpData(Command::rename), m_cmd(command)
	{
	}

	CRenameCommand m_cmd;
	bool m_useAbsolute{};
};

enum renameStates
{
	rename_init = 0,
	rename_rnfrom,
	rename_rnto
};

int CFtpControlSocket::Rename(CRenameCommand const& command)
{
	if (m_pCurOpData) {
		LogMessage(MessageType::Debug_Warning, L"CFtpControlSocket::Rename(): m_pCurOpData not empty");
		return FZ_REPLY_INTERNALERROR;
	}

	LogMessage(MessageType::Status, _("Renaming '%s' to '%s'"), command.GetFromPath().FormatFilename(command.GetFromFile()), command.GetToPath().FormatFilename(command.GetToFile()));

	CFtpRenameOpData *pData = new CFtpRenameOpData(command);
	pData->opState = rename_rnfrom;
	Push(pData);

	ChangeDir(command.GetFromPath());
	return FZ_REPLY_CONTINUE;
}

int CFtpControlSocket::RenameParseResponse()
{
	CFtpRenameOpData *pData = static_cast<CFtpRenameOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, L"m_pCurOpData empty");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	int code = GetReplyCode();
	if (code != 2 && code != 3) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (pData->opState == rename_rnfrom) {
		pData->opState = rename_rnto;
	}
	else {
		const CServerPath& fromPath = pData->m_cmd.GetFromPath();
		const CServerPath& toPath = pData->m_cmd.GetToPath();
		engine_.GetDirectoryCache().Rename(currentServer_, fromPath, pData->m_cmd.GetFromFile(), toPath, pData->m_cmd.GetToFile());

		SendDirectoryListingNotification(fromPath, false, false);
		if (fromPath != toPath) {
			SendDirectoryListingNotification(toPath, false, false);
		}

		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	}

	return SendNextCommand();
}

int CFtpControlSocket::RenameSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::RenameSubcommandResult()");

	CFtpRenameOpData *pData = static_cast<CFtpRenameOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, L"m_pCurOpData empty");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK) {
		pData->m_useAbsolute = true;
	}

	return SendNextCommand();
}

int CFtpControlSocket::RenameSend()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::RenameSend()");

	CFtpRenameOpData *pData = static_cast<CFtpRenameOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, L"m_pCurOpData empty");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res;
	switch (pData->opState)
	{
	case rename_rnfrom:
		res = SendCommand(L"RNFR " + pData->m_cmd.GetFromPath().FormatFilename(pData->m_cmd.GetFromFile(), !pData->m_useAbsolute));
		break;
	case rename_rnto:
		{
			engine_.GetDirectoryCache().InvalidateFile(currentServer_, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile());
			engine_.GetDirectoryCache().InvalidateFile(currentServer_, pData->m_cmd.GetToPath(), pData->m_cmd.GetToFile());

			CServerPath path(engine_.GetPathCache().Lookup(currentServer_, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile()));
			if (path.empty()) {
				path = pData->m_cmd.GetFromPath();
				path.AddSegment(pData->m_cmd.GetFromFile());
			}
			engine_.InvalidateCurrentWorkingDirs(path);

			engine_.GetPathCache().InvalidatePath(currentServer_, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile());
			engine_.GetPathCache().InvalidatePath(currentServer_, pData->m_cmd.GetToPath(), pData->m_cmd.GetToFile());

			res = SendCommand(L"RNTO " + pData->m_cmd.GetToPath().FormatFilename(pData->m_cmd.GetToFile(), !pData->m_useAbsolute && pData->m_cmd.GetFromPath() == pData->m_cmd.GetToPath()));
			break;
		}
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, L"unknown op state: %d", pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

class CFtpChmodOpData final : public COpData
{
public:
	CFtpChmodOpData(const CChmodCommand& command)
		: COpData(Command::chmod), m_cmd(command)
	{
	}

	CChmodCommand m_cmd;
	bool m_useAbsolute{};
};

enum chmodStates
{
	chmod_init = 0,
	chmod_chmod
};

int CFtpControlSocket::Chmod(CChmodCommand const& command)
{
	LogMessage(MessageType::Status, _("Set permissions of '%s' to '%s'"), command.GetPath().FormatFilename(command.GetFile()), command.GetPermission());

	CFtpChmodOpData *pData = new CFtpChmodOpData(command);
	pData->opState = chmod_chmod;
	Push(pData);

	ChangeDir(command.GetPath());
	return FZ_REPLY_CONTINUE;
}

int CFtpControlSocket::ChmodParseResponse()
{
	CFtpChmodOpData *pData = static_cast<CFtpChmodOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(MessageType::Debug_Warning, L"m_pCurOpData empty");
		return FZ_REPLY_INTERNALERROR;
	}

	int code = GetReplyCode();
	if (code != 2 && code != 3) {
		return FZ_REPLY_ERROR;
	}

	engine_.GetDirectoryCache().UpdateFile(currentServer_, pData->m_cmd.GetPath(), pData->m_cmd.GetFile(), false, CDirectoryCache::unknown);

	return FZ_REPLY_OK;
}

int CFtpControlSocket::ChmodSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::ChmodSubcommandResult()");

	CFtpChmodOpData *pData = static_cast<CFtpChmodOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, L"m_pCurOpData empty");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK) {
		pData->m_useAbsolute = true;
	}

	return SendNextCommand();
}

int CFtpControlSocket::ChmodSend()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::ChmodSend()");

	CFtpChmodOpData *pData = static_cast<CFtpChmodOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, L"m_pCurOpData empty");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res;
	switch (pData->opState) {
	case chmod_chmod:
		res = SendCommand(L"SITE CHMOD " + pData->m_cmd.GetPermission() + L" " + pData->m_cmd.GetPath().FormatFilename(pData->m_cmd.GetFile(), !pData->m_useAbsolute));
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, L"unknown op state: %d", pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::GetExternalIPAddress(std::string& address)
{
	// Local IP should work. Only a complete moron would use IPv6
	// and NAT at the same time.
	if (m_pSocket->GetAddressFamily() != CSocket::ipv6) {
		int mode = engine_.GetOptions().GetOptionVal(OPTION_EXTERNALIPMODE);

		if (mode) {
			if (engine_.GetOptions().GetOptionVal(OPTION_NOEXTERNALONLOCAL) &&
				!fz::is_routable_address(m_pSocket->GetPeerIP()))
				// Skip next block, use local address
				goto getLocalIP;
		}

		if (mode == 1) {
			std::wstring ip = engine_.GetOptions().GetOption(OPTION_EXTERNALIP);
			if (!ip.empty()) {
				address = fz::to_string(ip);
				return FZ_REPLY_OK;
			}

			LogMessage(MessageType::Debug_Warning, _("No external IP address set, trying default."));
		}
		else if (mode == 2) {
			if (!m_pIPResolver) {
				std::string localAddress = m_pSocket->GetLocalIP(true);

				if (!localAddress.empty() && localAddress == fz::to_string(engine_.GetOptions().GetOption(OPTION_LASTRESOLVEDIP))) {
					LogMessage(MessageType::Debug_Verbose, L"Using cached external IP address");

					address = localAddress;
					return FZ_REPLY_OK;
				}

				std::wstring resolverAddress = engine_.GetOptions().GetOption(OPTION_EXTERNALIPRESOLVER);

				LogMessage(MessageType::Debug_Info, _("Retrieving external IP address from %s"), resolverAddress);

				m_pIPResolver = std::make_unique<CExternalIPResolver>(engine_.GetThreadPool(), *this);
				m_pIPResolver->GetExternalIP(resolverAddress, CSocket::ipv4);
				if (!m_pIPResolver->Done()) {
					LogMessage(MessageType::Debug_Verbose, L"Waiting for resolver thread");
					return FZ_REPLY_WOULDBLOCK;
				}
			}
			if (!m_pIPResolver->Successful()) {
				m_pIPResolver.reset();

				LogMessage(MessageType::Debug_Warning, _("Failed to retrieve external ip address, using local address"));
			}
			else {
				LogMessage(MessageType::Debug_Info, L"Got external IP address");
				address = m_pIPResolver->GetIP();

				engine_.GetOptions().SetOption(OPTION_LASTRESOLVEDIP, fz::to_wstring(address));

				m_pIPResolver.reset();

				return FZ_REPLY_OK;
			}
		}
	}

getLocalIP:
	address = m_pSocket->GetLocalIP(true);
	if (address.empty()) {
		LogMessage(MessageType::Error, _("Failed to retrieve local ip address."), 1);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_OK;
}

void CFtpControlSocket::OnExternalIPAddress()
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::OnExternalIPAddress()");
	if (!m_pIPResolver) {
		LogMessage(MessageType::Debug_Info, L"Ignoring event");
		return;
	}

	SendNextCommand();
}

void CFtpControlSocket::Transfer(std::wstring const& cmd, CFtpTransferOpData* oldData)
{
	assert(oldData);
	oldData->tranferCommandSent = false;

	CFtpRawTransferOpData *pData = new CFtpRawTransferOpData(*this);
	Push(pData);

	pData->cmd_ = cmd;
	pData->pOldData = oldData;
	pData->pOldData->transferEndReason = TransferEndReason::successful;

	if (m_pProxyBackend) {
		// Only passive suported
		// Theoretically could use reverse proxy ability in SOCKS5, but
		// it is too fragile to set up with all those broken routers and
		// firewalls sabotaging connections. Regular active mode is hard
		// enough already
		pData->bPasv = true;
		pData->bTriedActive = true;
	}
	else {
		switch (currentServer_.GetPasvMode())
		{
		case MODE_PASSIVE:
			pData->bPasv = true;
			break;
		case MODE_ACTIVE:
			pData->bPasv = false;
			break;
		default:
			pData->bPasv = engine_.GetOptions().GetOptionVal(OPTION_USEPASV) != 0;
			break;
		}
	}

	if ((pData->pOldData->binary && m_lastTypeBinary == 1) ||
		(!pData->pOldData->binary && m_lastTypeBinary == 0))
	{
		pData->opState = rawtransfer_port_pasv;
	}
	else {
		pData->opState = rawtransfer_type;
	}
}

int CFtpControlSocket::FileTransferTestResumeCapability()
{
	LogMessage(MessageType::Debug_Verbose, L"FileTransferTestResumeCapability()");

	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"  empty m_pCurOpData");
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);

	if (!pData->download) {
		return FZ_REPLY_OK;
	}

	for (int i = 0; i < 2; ++i) {
		if (pData->localFileSize >= (1ll << (i ? 31 : 32))) {
			switch (CServerCapabilities::GetCapability(currentServer_, i ? resume2GBbug : resume4GBbug))
			{
			case yes:
				if (pData->remoteFileSize == pData->localFileSize) {
					LogMessage(MessageType::Debug_Info, _("Server does not support resume of files > %d GB. End transfer since file sizes match."), i ? 2 : 4);
					ResetOperation(FZ_REPLY_OK);
					return FZ_REPLY_CANCELED;
				}
				LogMessage(MessageType::Error, _("Server does not support resume of files > %d GB."), i ? 2 : 4);
				ResetOperation(FZ_REPLY_CRITICALERROR);
				return FZ_REPLY_ERROR;
			case unknown:
				if (pData->remoteFileSize < pData->localFileSize) {
					// Don't perform test
					break;
				}
				if (pData->remoteFileSize == pData->localFileSize) {
					LogMessage(MessageType::Debug_Info, _("Server may not support resume of files > %d GB. End transfer since file sizes match."), i ? 2 : 4);
					ResetOperation(FZ_REPLY_OK);
					return FZ_REPLY_CANCELED;
				}
				else if (pData->remoteFileSize > pData->localFileSize) {
					LogMessage(MessageType::Status, _("Testing resume capabilities of server"));

					pData->opState = filetransfer_waitresumetest;
					pData->resumeOffset = pData->remoteFileSize - 1;

					m_pTransferSocket = std::make_unique<CTransferSocket>(engine_, *this, TransferMode::resumetest);

					Transfer(L"RETR " + pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath), pData);
					return FZ_REPLY_CONTINUE;
				}
				break;
			case no:
				break;
			}
		}
	}

	return FZ_REPLY_OK;
}

void CFtpControlSocket::Connect(CServer const& server)
{
	if (m_pCurOpData) {
		LogMessage(MessageType::Debug_Info, L"CFtpControlSocket::Connect(): deleting nonzero pData");
		delete m_pCurOpData;
	}

	currentServer_ = server;

	CFtpLogonOpData* pData = new CFtpLogonOpData(*this, server);
	Push(pData);
}

void CFtpControlSocket::OnTimer(fz::timer_id id)
{
	if (id != m_idleTimer) {
		CControlSocket::OnTimer(id);
		return;
	}

	if (m_pCurOpData) {
		return;
	}

	if (m_pendingReplies || m_repliesToSkip) {
		return;
	}

	LogMessage(MessageType::Status, _("Sending keep-alive command"));

	std::wstring cmd;
	int i = fz::random_number(0, 2);
	if (!i) {
		cmd = L"NOOP";
	}
	else if (i == 1) {
		if (m_lastTypeBinary) {
			cmd = L"TYPE I";
		}
		else {
			cmd = L"TYPE A";
		}
	}
	else {
		cmd = L"PWD";
	}

	if (!SendCommand(cmd)) {
		return;
	}
	++m_repliesToSkip;
}

void CFtpControlSocket::StartKeepaliveTimer()
{
	if (!engine_.GetOptions().GetOptionVal(OPTION_FTP_SENDKEEPALIVE)) {
		return;
	}

	if (m_repliesToSkip || m_pendingReplies) {
		return;
	}

	if (!m_lastCommandCompletionTime) {
		return;
	}

	fz::duration const span = fz::monotonic_clock::now() - m_lastCommandCompletionTime;
	if (span.get_minutes() >= 30) {
		return;
	}

	stop_timer(m_idleTimer);
	m_idleTimer = add_timer(fz::duration::from_seconds(30), true);
}

int CFtpControlSocket::ParseSubcommandResult(int prevResult, COpData const& opData)
{
	LogMessage(MessageType::Debug_Verbose, L"CFtpControlSocket::ParseSubcommandResult(%d)", prevResult);
	if (!m_pCurOpData) {
		LogMessage(MessageType::Debug_Warning, L"ParseSubcommandResult called without active operation");
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	int res = m_pCurOpData->SubcommandResult(prevResult, opData);
	if (res == FZ_REPLY_WOULDBLOCK) {
		return FZ_REPLY_WOULDBLOCK;
	}
	else if (res == FZ_REPLY_CONTINUE) {
		return SendNextCommand();
	}
	else {
		return ResetOperation(res);
	}
}

void CFtpControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::timer_event>(ev, this, &CFtpControlSocket::OnTimer)) {
		return;
	}

	if (fz::dispatch<CExternalIPResolveEvent>(ev, this, &CFtpControlSocket::OnExternalIPAddress)) {
		return;
	}

	CRealControlSocket::operator()(ev);
}