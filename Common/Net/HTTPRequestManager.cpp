#include "Common/StringUtils.h"
#include "Common/System/System.h"
#include "Common/System/Request.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/HTTPRequest.h"
#include "Common/Net/HTTPRequestManager.h"
#include "Common/Net/HTTPNaettRequest.h"
#include "Common/File/FileUtil.h"
#include "Common/TimeUtil.h"

namespace http {

static bool IsHttpsUrl(std::string_view url) {
	return startsWith(url, "https:");
}

Path UrlToCachePath(const Path &cacheDir, std::string_view url) {
	std::string fn = "DLCACHE_";
	for (auto c : url) {
		if (isalnum(c) || c == '.' || c == '-' || c == '_') {
			fn.push_back(tolower(c));
		} else {
			fn.push_back('_');
		}
	}
	return cacheDir / fn;
}

Path RequestManager::UrlToCachePath(const std::string_view url) {
	if (cacheDir_.empty()) {
		return Path();
	}
	return http::UrlToCachePath(cacheDir_, url);
}

static std::shared_ptr<Request> CreateRequest(RequestMethod method, std::string_view url, std::string_view postdata, std::string_view postMime, const Path &outfile, RequestFlags flags, net::ResolveFunc customResolve, std::string_view name) {
	if (IsHttpsUrl(url) && System_GetPropertyBool(SYSPROP_SUPPORTS_HTTPS)) {
#ifndef HTTPS_NOT_AVAILABLE
		return std::make_shared<HTTPSRequest>(method, url, postdata, postMime, outfile, flags, name);
#else
		return std::shared_ptr<Request>();
#endif
	} else {
		return std::make_shared<HTTPRequest>(method, url, postdata, postMime, outfile, flags, customResolve, name);
	}
}

std::shared_ptr<Request> RequestManager::StartDownload(std::string_view url, const Path &outfile, RequestFlags flags, const char *acceptMime) {
	const bool enableCache = !cacheDir_.empty() && (flags & RequestFlags::Cached24H);

	// Come up with a cache file path.
	Path cacheFile = UrlToCachePath(url);

	if (enableCache) {
		_dbg_assert_(outfile.empty());  // It's automatically replaced below

		// TODO: This should be done on the thread, maybe. But let's keep it simple for now.
		time_t cacheFileTime;
		if (File::GetModifTimeT(cacheFile, &cacheFileTime)) {
			time_t now = (time_t)time_now_unix_utc();
			if (cacheFileTime > now - 24 * 60 * 60) {
				// The file is new enough. Let's construct a fake, already finished download so we don't need
				// to modify the calling code.
				std::string contents;
				if (File::ReadBinaryFileToString(cacheFile, &contents)) {
					INFO_LOG(Log::sceNet, "Returning cached file for %.*s: %s", (int)url.size(), url.data(), cacheFile.c_str());
					// All is well, but we've indented a bit much here.
					std::shared_ptr<Request> dl(new CachedRequest(RequestMethod::GET, url, "infra-dns.json", nullptr, flags, contents));
					newDownloads_.push_back(dl);
					return dl;
				} else {
					INFO_LOG(Log::sceNet, "Failed reading from cache, proceeding with request");
				}
			} else {
				INFO_LOG(Log::sceNet, "Cached file too old, proceeding with request");
			}
		} else {
			INFO_LOG(Log::sceNet, "Failed to check time modified. Proceeding with request.");
		}
	}

	std::shared_ptr<Request> dl = CreateRequest(RequestMethod::GET, url, "", "", outfile, flags, nullptr, "");

	// OK, didn't get it from cache, so let's continue with the download, putting it in the cache.
	if (enableCache) {
		dl->OverrideOutFile(cacheFile);
		dl->AddFlag(RequestFlags::KeepInMemory);
	}

	if (!userAgent_.empty()) {
		dl->SetUserAgent(userAgent_);
	}
	if (acceptMime) {
		dl->SetAccept(acceptMime);
	}
	newDownloads_.push_back(dl);
	dl->Start();
	return dl;
}

std::shared_ptr<Request> RequestManager::StartDownloadWithCallback(
	std::string_view url,
	const Path &outfile,
	RequestFlags flags,
	std::function<void(Request &)> callback,
	std::string_view name,
	const char *acceptMime) {
	std::shared_ptr<Request> dl = CreateRequest(RequestMethod::GET, url, "", "", outfile, flags, nullptr, name);

	if (!userAgent_.empty())
		dl->SetUserAgent(userAgent_);
	if (acceptMime)
		dl->SetAccept(acceptMime);
	dl->SetCallback(callback);
	newDownloads_.push_back(dl);
	dl->Start();
	return dl;
}

std::shared_ptr<Request> RequestManager::AsyncPostWithCallback(
	std::string_view url,
	std::string_view postData,
	std::string_view postMime,
	RequestFlags flags,
	std::function<void(Request &)> callback,
	std::string_view name) {
	std::shared_ptr<Request> dl = CreateRequest(RequestMethod::POST, url, postData, postMime, Path(), flags, nullptr, name);
	if (!userAgent_.empty())
		dl->SetUserAgent(userAgent_);
	dl->SetCallback(callback);
	newDownloads_.push_back(dl);
	dl->Start();
	return dl;
}

void RequestManager::Update() {
	for (auto &iter : newDownloads_) {
		downloads_.push_back(iter);
	}
	newDownloads_.clear();

restart:
	for (size_t i = 0; i < downloads_.size(); i++) {
		auto dl = downloads_[i];
		if (dl->Done()) {
			dl->RunCallback();
			dl->Join();
			downloads_.erase(downloads_.begin() + i);
			goto restart;
		}
	}
}

void RequestManager::CancelAll() {
	for (size_t i = 0; i < downloads_.size(); i++) {
		downloads_[i]->Cancel();
	}
	for (size_t i = 0; i < downloads_.size(); i++) {
		downloads_[i]->Join();
	}
	downloads_.clear();
}

}  // namespace http
