#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Common/Net/HTTPClient.h"
#include "Common/TimeUtil.h"
#include "Common/StringUtils.h"
#include "Common/System/OSD.h"

#include "Common/Net/SocketCompat.h"
#include "Common/Net/Resolve.h"
#include "Common/Net/URL.h"

#include "Common/File/FileDescriptor.h"
#include "Common/SysError.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Data/Encoding/Compression.h"
#include "Common/Net/NetBuffer.h"
#include "Common/Log.h"

namespace http {

// TODO: do something sane here
constexpr const char *DEFAULT_USERAGENT = "PPSSPP";
constexpr const char *HTTP_VERSION = "1.1";

Client::Client(net::ResolveFunc func) : Connection(func) {
	userAgent_ = DEFAULT_USERAGENT;
	httpVersion_ = HTTP_VERSION;
}

Client::~Client() {
	Disconnect();
}

// Ignores line folding (deprecated), but respects field combining.
// Don't use for Set-Cookie, which is a special header per RFC 7230.
bool GetHeaderValue(const std::vector<std::string> &responseHeaders, std::string_view header, std::string *value) {
	std::string search(header);
	search.push_back(':');

	bool found = false;

	value->clear();
	for (const std::string &line : responseHeaders) {
		auto stripped = StripSpaces(line);
		if (startsWithNoCase(stripped, search)) {
			size_t value_pos = search.length();
			size_t after_white = stripped.find_first_not_of(" \t", value_pos);
			if (after_white != stripped.npos)
				value_pos = after_white;

			if (!found)
				*value = stripped.substr(value_pos);
			else
				*value += "," + std::string(stripped.substr(value_pos));
			found = true;
		}
	}

	return found;
}

static bool DeChunk(Buffer *inbuffer, Buffer *outbuffer, int contentLength) {
	_dbg_assert_(outbuffer->empty());
	int dechunkedBytes = 0;
	while (true) {
		std::string line;
		inbuffer->TakeLineCRLF(&line);
		if (!line.size())
			return false;
		unsigned int chunkSize = 0;
		if (sscanf(line.c_str(), "%x", &chunkSize) != 1) {
			return false;
		}
		if (chunkSize) {
			std::string data;
			inbuffer->Take(chunkSize, &data);
			outbuffer->Append(data);
		} else {
			// a zero size chunk should mean the end.
			inbuffer->clear();
			return true;
		}
		dechunkedBytes += chunkSize;
		inbuffer->Skip(2);
	}
	// Unreachable
	return true;
}

int Client::GET(const RequestParams &req, Buffer *output, std::vector<std::string> &responseHeaders, net::RequestProgress *progress) {
	const char *otherHeaders =
		"Accept-Encoding: gzip\r\n";
	int err = SendRequest("GET", req, otherHeaders, progress);
	if (err < 0) {
		return err;
	}

	net::Buffer readbuf;
	int code = ReadResponseHeaders(&readbuf, responseHeaders, progress);
	if (code < 0) {
		return code;
	}

	err = ReadResponseEntity(&readbuf, responseHeaders, output, progress);
	if (err < 0) {
		return err;
	}
	return code;
}

int Client::GET(const RequestParams &req, Buffer *output, net::RequestProgress *progress) {
	std::vector<std::string> responseHeaders;
	int code = GET(req, output, responseHeaders, progress);
	return code;
}

int Client::POST(const RequestParams &req, std::string_view data, std::string_view mime, Buffer *output, net::RequestProgress *progress) {
	char otherHeaders[2048];
	if (mime.empty()) {
		snprintf(otherHeaders, sizeof(otherHeaders), "Content-Length: %lld\r\n", (long long)data.size());
	} else {
		snprintf(otherHeaders, sizeof(otherHeaders), "Content-Length: %lld\r\nContent-Type: %.*s\r\n", (long long)data.size(), (int)mime.size(), mime.data());
	}

	int err = SendRequestWithData("POST", req, data, otherHeaders, progress);
	if (err < 0) {
		return err;
	}

	net::Buffer readbuf;
	std::vector<std::string> responseHeaders;
	int code = ReadResponseHeaders(&readbuf, responseHeaders, progress);
	if (code < 0) {
		return code;
	}

	err = ReadResponseEntity(&readbuf, responseHeaders, output, progress);
	if (err < 0) {
		return err;
	}
	return code;
}

int Client::POST(const RequestParams &req, std::string_view data, Buffer *output, net::RequestProgress *progress) {
	return POST(req, data, "", output, progress);
}

int Client::SendRequest(const char *method, const RequestParams &req, const char *otherHeaders, net::RequestProgress *progress) {
	return SendRequestWithData(method, req, "", otherHeaders, progress);
}

int Client::SendRequestWithData(const char *method, const RequestParams &req, std::string_view data, const char *otherHeaders, net::RequestProgress *progress) {
	progress->Update(0, 0, false);

	net::Buffer buffer;
	const char *tpl =
		"%s %s HTTP/%s\r\n"
		"Host: %s\r\n"
		"User-Agent: %s\r\n"
		"Accept: %s\r\n"
		"Connection: close\r\n"
		"%s"
		"\r\n";

	buffer.Printf(tpl,
		method, req.resource.c_str(), HTTP_VERSION,
		host_.c_str(),
		userAgent_.c_str(),
		req.acceptMime,
		otherHeaders ? otherHeaders : "");
	buffer.Append(data);
	bool flushed = buffer.FlushSocket(sock(), dataTimeout_, progress->cancelled);
	if (!flushed) {
		return -1;  // TODO error code.
	}
	return 0;
}

int Client::ReadResponseHeaders(net::Buffer *readbuf, std::vector<std::string> &responseHeaders, net::RequestProgress *progress, std::string *statusLine) {
	// Snarf all the data we can into RAM. A little unsafe but hey.
	static constexpr float CANCEL_INTERVAL = 0.25f;
	bool ready = false;
	double endTimeout = time_now_d() + dataTimeout_;
	while (!ready) {
		if (progress->cancelled && *progress->cancelled)
			return -1;
		ready = fd_util::WaitUntilReady(sock(), CANCEL_INTERVAL, false);
		if (!ready && time_now_d() > endTimeout) {
			ERROR_LOG(Log::HTTP, "HTTP headers timed out");
			return -1;
		}
	};
	// Let's hope all the headers are available in a single packet...
	if (readbuf->Read(sock(), 4096) < 0) {
		ERROR_LOG(Log::HTTP, "Failed to read HTTP headers :(");
		return -1;
	}

	if (readbuf->empty()) {
		ERROR_LOG(Log::HTTP, "Empty HTTP header read buffer :(");
		return -1;
	}

	// Grab the first header line that contains the http code.

	std::string line;
	readbuf->TakeLineCRLF(&line);

	int code;
	size_t code_pos = line.find(' ');
	if (code_pos != line.npos) {
		code_pos = line.find_first_not_of(' ', code_pos);
	}

	if (code_pos != line.npos) {
		code = atoi(&line[code_pos]);
	} else {
		ERROR_LOG(Log::HTTP, "Could not parse HTTP status code: '%s'", line.c_str());
		return -1;
	}

	if (statusLine)
		*statusLine = line;

	while (true) {
		int sz = readbuf->TakeLineCRLF(&line);
		if (!sz || sz < 0)
			break;
		VERBOSE_LOG(Log::HTTP, "Header line: %s", line.c_str());
		responseHeaders.push_back(line);
	}

	if (responseHeaders.size() == 0) {
		ERROR_LOG(Log::HTTP, "No HTTP response headers");
		return -1;
	}

	return code;
}

int Client::ReadResponseEntity(net::Buffer *readbuf, const std::vector<std::string> &responseHeaders, Buffer *output, net::RequestProgress *progress) {
	_dbg_assert_(progress->cancelled);

	bool gzip = false;
	bool chunked = false;
	int contentLength = 0;
	for (std::string line : responseHeaders) {
		if (startsWithNoCase(line, "Content-Length:")) {
			size_t size_pos = line.find_first_of(' ');
			if (size_pos != line.npos) {
				size_pos = line.find_first_not_of(' ', size_pos);
			}
			if (size_pos != line.npos) {
				contentLength = atoi(&line[size_pos]);
				chunked = false;
			}
		} else if (startsWithNoCase(line, "Content-Encoding:")) {
			// TODO: Case folding...
			if (line.find("gzip") != std::string::npos) {
				gzip = true;
			}
		} else if (startsWithNoCase(line, "Transfer-Encoding:")) {
			// TODO: Case folding...
			if (line.find("chunked") != std::string::npos) {
				chunked = true;
			}
		}
	}

	if (contentLength < 0) {
		WARN_LOG(Log::HTTP, "Negative content length %d", contentLength);
		// Just sanity checking...
		contentLength = 0;
	}

	if (!readbuf->ReadAllWithProgress(sock(), contentLength, progress))
		return -1;

	// output now contains the rest of the reply. Dechunk it.
	if (!output->IsVoid()) {
		if (chunked) {
			if (!DeChunk(readbuf, output, contentLength)) {
				ERROR_LOG(Log::HTTP, "Bad chunked data, couldn't read chunk size");
				progress->Update(0, 0, true);
				return -1;
			}
		} else {
			output->Append(*readbuf);
		}

		// If it's gzipped, we decompress it and put it back in the buffer.
		if (gzip) {
			std::string compressed, decompressed;
			output->TakeAll(&compressed);
			bool result = decompress_string(compressed, &decompressed);
			if (!result) {
				ERROR_LOG(Log::HTTP, "Error decompressing using zlib");
				progress->Update(0, 0, true);
				return -1;
			}
			output->Append(decompressed);
		}
	}

	progress->Update(contentLength, contentLength, true);
	return 0;
}

HTTPRequest::HTTPRequest(RequestMethod method, std::string_view url, std::string_view postData, std::string_view postMime, const Path &outfile, RequestFlags flags, net::ResolveFunc customResolve, std::string_view name)
	: Request(method, url, name, &cancelled_, flags), postData_(postData), postMime_(postMime), customResolve_(customResolve) {
	outfile_ = outfile;
}

HTTPRequest::~HTTPRequest() {
	g_OSD.RemoveProgressBar(url_, !failed_, 0.5f);

	if (thread_.joinable()) {
		_dbg_assert_msg_(false, "Download destructed without join");
	}
}

void HTTPRequest::Start() {
	thread_ = std::thread([this] { Do(); });
}

void HTTPRequest::Join() {
	if (!thread_.joinable()) {
		ERROR_LOG(Log::HTTP, "Already joined thread!");
		_dbg_assert_(false);
	}
	thread_.join();
}

void HTTPRequest::SetFailed(int code) {
	failed_ = true;
	progress_.Update(0, 0, true);
	completed_ = true;
}

int HTTPRequest::Perform(const std::string &url) {
	Url fileUrl(url);
	if (!fileUrl.Valid()) {
		return -1;
	}

	http::Client client(customResolve_);
	if (!userAgent_.empty()) {
		client.SetUserAgent(userAgent_);
	}

	if (!client.Resolve(fileUrl.Host().c_str(), fileUrl.Port())) {
		ERROR_LOG(Log::HTTP, "Failed resolving %s", url.c_str());
		return -1;
	}

	if (cancelled_) {
		return -1;
	}

	if (!client.Connect(2, 20.0, &cancelled_)) {
		ERROR_LOG(Log::HTTP, "Failed connecting to server or cancelled (=%d).", cancelled_);
		return -1;
	}

	if (cancelled_) {
		return -1;
	}

	RequestParams req(fileUrl.Resource(), acceptMime_);
	if (method_ == RequestMethod::GET) {
		return client.GET(req, &buffer_, responseHeaders_, &progress_);
	} else {
		return client.POST(req, postData_, postMime_, &buffer_, &progress_);
	}
}

std::string HTTPRequest::RedirectLocation(const std::string &baseUrl) const {
	std::string redirectUrl;
	if (GetHeaderValue(responseHeaders_, "Location", &redirectUrl)) {
		Url url(baseUrl);
		url = url.Relative(redirectUrl);
		redirectUrl = url.ToString();
	}
	return redirectUrl;
}

void HTTPRequest::Do() {
	SetCurrentThreadName("HTTPDownload::Do");

	AndroidJNIThreadContext jniContext;
	resultCode_ = 0;

	std::string downloadURL = url_;
	while (resultCode_ == 0) {
		// This is where the new request is performed.
		int resultCode = Perform(downloadURL);
		if (resultCode == -1) {
			SetFailed(resultCode);
			return;
		}

		if (resultCode == 301 || resultCode == 302 || resultCode == 303 || resultCode == 307 || resultCode == 308) {
			std::string redirectURL = RedirectLocation(downloadURL);
			if (redirectURL.empty()) {
				ERROR_LOG(Log::HTTP, "Could not find Location header for redirect");
				resultCode_ = resultCode;
			} else if (redirectURL == downloadURL || redirectURL == url_) {
				// Simple loop detected, bail out.
				resultCode_ = resultCode;
			}

			// Perform the next GET.
			if (resultCode_ == 0) {
				INFO_LOG(Log::HTTP, "Download of %s redirected to %s", downloadURL.c_str(), redirectURL.c_str());
				buffer_.clear();
				responseHeaders_.clear();
			}
			downloadURL = redirectURL;
			continue;
		}

		if (resultCode == 200) {
			INFO_LOG(Log::HTTP, "Completed requesting %s (storing result to %s)", url_.c_str(), outfile_.empty() ? "memory" : outfile_.c_str());
			bool clear = !(flags_ & RequestFlags::KeepInMemory);
			if (!outfile_.empty() && !buffer_.FlushToFile(outfile_, clear)) {
				ERROR_LOG(Log::HTTP, "Failed writing download to '%s'", outfile_.c_str());
			}
		} else {
			ERROR_LOG(Log::HTTP, "Error requesting '%s' (storing result to '%s'): %i", url_.c_str(), outfile_.empty() ? "memory" : outfile_.c_str(), resultCode);
		}
		resultCode_ = resultCode;
	}

	// Set this last to ensure no race conditions when checking Done. Users must always check
	// Done before looking at the result code.
	completed_ = true;
}

}  // namespace http
