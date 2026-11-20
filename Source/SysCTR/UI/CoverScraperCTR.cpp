#include "CoverScraperCTR.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef DAEDALUS_CTR
#include "stdafx.h"
#include <3ds.h>
#include <atomic>
#include <png.h>
#include "Utility/IO.h"
#endif

namespace UI
{
namespace CoverScraper
{

namespace
{

const char * const kBaseBoxartUrl =
	"https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_64/master/Named_Boxarts/";

std::string Trim(const std::string & s)
{
	size_t a = 0;
	while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
	size_t b = s.size();
	while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
	return s.substr(a, b - a);
}

std::string StripExtension(const char * filename)
{
	if (!filename) return std::string();
	std::string s(filename);
	const size_t dot = s.rfind('.');
	if (dot != std::string::npos && dot > 0)
		s.resize(dot);
	return s;
}

// Strip GoodN64 bracket tags like [!], [b1], [h1C] and revision tags like (v1.0) / (Rev A).
std::string CleanNoIntroStem(const std::string & raw)
{
	std::string out;
	out.reserve(raw.size());
	bool inBracket = false;
	for (size_t i = 0; i < raw.size(); ++i)
	{
		char c = raw[i];
		if (c == '[') { inBracket = true; continue; }
		if (c == ']') { inBracket = false; continue; }
		if (!inBracket) out.push_back(c);
	}
	// Strip "(v1.0)", "(v1.1)", "(v1.2)" if present
	for (const char * tag : { "(v1.0)", "(v1.1)", "(v1.2)", "(V1.0)", "(V1.1)", "(V1.2)" })
	{
		size_t pos = out.find(tag);
		if (pos != std::string::npos)
			out.erase(pos, strlen(tag));
	}
	return Trim(out);
}

// Strip all (...) and [...] tags to isolate the base game title.
std::string StripAllTags(const std::string & raw)
{
	std::string out;
	out.reserve(raw.size());
	int paren = 0;
	int brack = 0;
	for (char c : raw)
	{
		if (c == '(') { ++paren; continue; }
		if (c == ')') { if (paren > 0) --paren; continue; }
		if (c == '[') { ++brack; continue; }
		if (c == ']') { if (brack > 0) --brack; continue; }
		if (paren == 0 && brack == 0) out.push_back(c);
	}
	return Trim(out);
}

// libretro-thumbnails replaces &*/:`<>?\|" with '_'
std::string SanitizeLibretroName(const std::string & name)
{
	std::string out;
	out.reserve(name.size());
	for (char c : name)
	{
		switch (c)
		{
		case '&': case '*': case '/': case ':': case '`':
		case '<': case '>': case '?': case '\\': case '|': case '"':
			out.push_back('_');
			break;
		default:
			out.push_back(c);
			break;
		}
	}
	return Trim(out);
}

// Move leading "The " -> ", The" before " - " or at end, matching No-Intro naming.
std::string MoveLeadingThe(const std::string & title)
{
	if (title.size() > 4 && strncasecmp(title.c_str(), "The ", 4) == 0)
	{
		const std::string rest = title.substr(4);
		const size_t dash = rest.find(" - ");
		if (dash != std::string::npos)
			return rest.substr(0, dash) + ", The" + rest.substr(dash);
		return rest + ", The";
	}
	return title;
}

void PushUniqueUrl(std::vector<std::string> & urls, const std::string & gameName)
{
	const std::string clean = SanitizeLibretroName(gameName);
	if (clean.empty()) return;
	std::string url = std::string(kBaseBoxartUrl) + UrlEncodePathComponent((clean + ".png").c_str());
	if (std::find(urls.begin(), urls.end(), url) == urls.end())
		urls.push_back(url);
}

} // namespace

std::string UrlEncodePathComponent(const char * text)
{
	if (!text) return std::string();
	static const char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(strlen(text) * 2);
	for (const unsigned char * p = (const unsigned char *)text; *p; ++p)
	{
		const unsigned char c = *p;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
		{
			out.push_back((char)c);
		}
		else
		{
			out.push_back('%');
			out.push_back(kHex[(c >> 4) & 0xF]);
			out.push_back(kHex[c & 0xF]);
		}
	}
	return out;
}

void BuildCandidateUrls(const char * romFilename,
                        const char * romTitle,
                        const char * regionCode,
                        std::vector<std::string> & outUrls)
{
	outUrls.clear();
	const std::string stem      = StripExtension(romFilename);
	const std::string cleanStem = CleanNoIntroStem(stem);
	const std::string baseFile  = StripAllTags(stem);
	const std::string baseTitle = romTitle ? StripAllTags(romTitle) : std::string();

	// 1. Exact No-Intro filename stem (hits most standard ROMs on first try!)
	if (!cleanStem.empty())
		PushUniqueUrl(outUrls, cleanStem);

	// 2. Base filename + (USA) (catches dumps like "Mario Kart 64 (ESP) [!].z64")
	if (!baseFile.empty())
	{
		PushUniqueUrl(outUrls, baseFile + " (USA)");
		if (regionCode && strcmp(regionCode, "EUR") == 0)
			PushUniqueUrl(outUrls, baseFile + " (Europe)");
		else if (regionCode && strcmp(regionCode, "JPN") == 0)
			PushUniqueUrl(outUrls, baseFile + " (Japan)");
		const std::string moved = MoveLeadingThe(baseFile);
		if (moved != baseFile)
			PushUniqueUrl(outUrls, moved + " (USA)");
	}

	// 3. roms.ini clean title + (USA) / No-Intro "The" form
	if (!baseTitle.empty())
	{
		PushUniqueUrl(outUrls, baseTitle + " (USA)");
		const std::string moved = MoveLeadingThe(baseTitle);
		if (moved != baseTitle)
			PushUniqueUrl(outUrls, moved + " (USA)");
	}
}

#ifdef DAEDALUS_CTR

namespace
{

constexpr size_t kMaxPngDownloadBytes = 1024 * 1024; // 1 MiB safety cap per cover
constexpr int    kTargetMaxDim        = 128;         // Crisp 128px boxart -> 32 KiB GPU texture!

Thread                   s_thread = nullptr;
LightLock                s_lock;
bool                     s_lockInit = false;
std::atomic<bool>        s_stopReq(false);
std::atomic<bool>        s_active(false);
std::atomic<int>         s_completed(0);
std::atomic<int>         s_total(0);
std::atomic<int>         s_downloaded(0);
std::atomic<uint32_t>    s_revision(0);
std::vector<ScrapeJob>   s_queue;

struct MemReader
{
	const uint8_t * data;
	size_t          size;
	size_t          offset;
};

void PngReadFromMem(png_structp png, png_bytep outBytes, png_size_t byteCount)
{
	MemReader * r = static_cast<MemReader *>(png_get_io_ptr(png));
	if (!r || r->offset + byteCount > r->size)
	{
		png_error(png, "EOF");
		return;
	}
	memcpy(outBytes, r->data + r->offset, byteCount);
	r->offset += byteCount;
}

// Decode downloaded PNG from RAM, smoothly box-downsample to <= 128x128 (preserving
// aspect ratio), and write a compact PNG to the SD card.
bool DownsampleAndSavePng(const uint8_t * pngData, size_t pngSize, const char * destPath)
{
	if (!pngData || pngSize < 8 || png_sig_cmp(pngData, 0, 8) != 0)
		return false;

	png_structp rpng = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!rpng) return false;
	png_infop rinfo = png_create_info_struct(rpng);
	if (!rinfo) { png_destroy_read_struct(&rpng, nullptr, nullptr); return false; }

	std::vector<uint8_t> rgba;
	std::vector<png_bytep> rowPtrs;
	uint32_t srcW = 0, srcH = 0;

	if (setjmp(png_jmpbuf(rpng)))
	{
		png_destroy_read_struct(&rpng, &rinfo, nullptr);
		return false;
	}

	MemReader mr = { pngData, pngSize, 0 };
	png_set_read_fn(rpng, &mr, PngReadFromMem);
	png_read_info(rpng, rinfo);

	srcW = png_get_image_width(rpng, rinfo);
	srcH = png_get_image_height(rpng, rinfo);
	const png_byte colorType = png_get_color_type(rpng, rinfo);
	const png_byte bitDepth  = png_get_bit_depth(rpng, rinfo);

	if (srcW == 0 || srcH == 0 || srcW > 2048 || srcH > 2048)
	{
		png_destroy_read_struct(&rpng, &rinfo, nullptr);
		return false;
	}

	if (bitDepth == 16) png_set_strip_16(rpng);
	if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(rpng);
	if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(rpng);
	if (png_get_valid(rpng, rinfo, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(rpng);
	if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_PALETTE)
		png_set_filler(rpng, 0xFF, PNG_FILLER_AFTER);
	if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(rpng);

	png_read_update_info(rpng, rinfo);

	rgba.resize((size_t)srcW * (size_t)srcH * 4u);
	rowPtrs.resize(srcH);
	for (uint32_t y = 0; y < srcH; ++y)
		rowPtrs[y] = rgba.data() + (size_t)y * (size_t)srcW * 4u;

	png_read_image(rpng, rowPtrs.data());
	png_destroy_read_struct(&rpng, &rinfo, nullptr);

	// Compute target dimensions <= 128x128 preserving aspect ratio
	uint32_t dstW = srcW;
	uint32_t dstH = srcH;
	if (dstW > (uint32_t)kTargetMaxDim || dstH > (uint32_t)kTargetMaxDim)
	{
		if (dstW >= dstH)
		{
			dstH = (uint32_t)((uint64_t)srcH * kTargetMaxDim / srcW);
			dstW = kTargetMaxDim;
		}
		else
		{
			dstW = (uint32_t)((uint64_t)srcW * kTargetMaxDim / srcH);
			dstH = kTargetMaxDim;
		}
		if (dstW < 1) dstW = 1;
		if (dstH < 1) dstH = 1;
	}

	std::vector<uint8_t> outRgba((size_t)dstW * (size_t)dstH * 4u);
	if (dstW == srcW && dstH == srcH)
	{
		outRgba.swap(rgba);
	}
	else
	{
		// Box-filter downsample for crisp anti-aliased cover art
		for (uint32_t dy = 0; dy < dstH; ++dy)
		{
			const uint32_t sy0 = (dy * srcH) / dstH;
			uint32_t sy1 = ((dy + 1) * srcH) / dstH;
			if (sy1 <= sy0) sy1 = sy0 + 1;
			if (sy1 > srcH) sy1 = srcH;

			for (uint32_t dx = 0; dx < dstW; ++dx)
			{
				const uint32_t sx0 = (dx * srcW) / dstW;
				uint32_t sx1 = ((dx + 1) * srcW) / dstW;
				if (sx1 <= sx0) sx1 = sx0 + 1;
				if (sx1 > srcW) sx1 = srcW;

				uint32_t sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
				for (uint32_t sy = sy0; sy < sy1; ++sy)
				{
					const uint8_t * row = rgba.data() + ((size_t)sy * srcW + sx0) * 4u;
					for (uint32_t sx = sx0; sx < sx1; ++sx, row += 4)
					{
						sr += row[0];
						sg += row[1];
						sb += row[2];
						sa += row[3];
						++cnt;
					}
				}
				uint8_t * dst = outRgba.data() + ((size_t)dy * dstW + dx) * 4u;
				dst[0] = (uint8_t)(sr / cnt);
				dst[1] = (uint8_t)(sg / cnt);
				dst[2] = (uint8_t)(sb / cnt);
				dst[3] = (uint8_t)(sa / cnt);
			}
		}
	}

	// Write compact resized PNG atomically via temp file
	std::string tmpPath = std::string(destPath) + ".tmp";
	FILE * outF = fopen(tmpPath.c_str(), "wb");
	if (!outF) return false;

	png_structp wpng = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!wpng) { fclose(outF); remove(tmpPath.c_str()); return false; }
	png_infop winfo = png_create_info_struct(wpng);
	if (!winfo) { png_destroy_write_struct(&wpng, nullptr); fclose(outF); remove(tmpPath.c_str()); return false; }

	if (setjmp(png_jmpbuf(wpng)))
	{
		png_destroy_write_struct(&wpng, &winfo);
		fclose(outF);
		remove(tmpPath.c_str());
		return false;
	}

	png_init_io(wpng, outF);
	png_set_IHDR(wpng, winfo, dstW, dstH, 8, PNG_COLOR_TYPE_RGBA,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(wpng, winfo);

	std::vector<png_bytep> outRows(dstH);
	for (uint32_t y = 0; y < dstH; ++y)
		outRows[y] = outRgba.data() + (size_t)y * (size_t)dstW * 4u;
	png_write_image(wpng, outRows.data());
	png_write_end(wpng, nullptr);
	png_destroy_write_struct(&wpng, &winfo);
	fclose(outF);

	remove(destPath);
	if (rename(tmpPath.c_str(), destPath) != 0)
	{
		remove(tmpPath.c_str());
		return false;
	}
	return true;
}

bool HttpDownloadToMemory(const char * startUrl, std::vector<uint8_t> & outBuf)
{
	outBuf.clear();
	std::string curUrl(startUrl);

	for (int redirect = 0; redirect < 4; ++redirect)
	{
		if (s_stopReq.load(std::memory_order_relaxed))
			return false;

		httpcContext ctx;
		Result ret = httpcOpenContext(&ctx, HTTPC_METHOD_GET, curUrl.c_str(), 1);
		if (R_FAILED(ret))
			return false;

		httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
		httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
		httpcAddRequestHeaderField(&ctx, "User-Agent", "DaedalusX3D/1.0");
		httpcAddRequestHeaderField(&ctx, "Connection", "Close");

		ret = httpcBeginRequest(&ctx);
		if (R_FAILED(ret))
		{
			httpcCloseContext(&ctx);
			return false;
		}

		u32 statusCode = 0;
		ret = httpcGetResponseStatusCodeTimeout(&ctx, &statusCode, 6000000000ULL);
		if (R_FAILED(ret))
		{
			httpcCloseContext(&ctx);
			return false;
		}

		if (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
		    statusCode == 307 || statusCode == 308)
		{
			char nextLoc[512] = "";
			Result hret = httpcGetResponseHeader(&ctx, "Location", nextLoc, sizeof(nextLoc) - 1);
			httpcCloseContext(&ctx);
			if (R_FAILED(hret) || !nextLoc[0])
				return false;
			curUrl = nextLoc;
			continue;
		}

		if (statusCode != 200)
		{
			httpcCloseContext(&ctx);
			return false;
		}

		u32 contentSize = 0;
		httpcGetDownloadSizeState(&ctx, nullptr, &contentSize);
		if (contentSize > kMaxPngDownloadBytes)
		{
			httpcCancelConnection(&ctx);
			httpcCloseContext(&ctx);
			return false;
		}
		if (contentSize > 0)
			outBuf.reserve(contentSize);

		uint8_t chunk[8192];
		bool ok = true;
		while (true)
		{
			if (s_stopReq.load(std::memory_order_relaxed))
			{
				httpcCancelConnection(&ctx);
				ok = false;
				break;
			}
			u32 readNow = 0;
			ret = httpcDownloadData(&ctx, chunk, sizeof(chunk), &readNow);
			if (readNow > 0)
			{
				if (outBuf.size() + readNow > kMaxPngDownloadBytes)
				{
					httpcCancelConnection(&ctx);
					ok = false;
					break;
				}
				outBuf.insert(outBuf.end(), chunk, chunk + readNow);
			}
			if (ret == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)
				continue;
			if (R_FAILED(ret))
				ok = false;
			break;
		}

		httpcCloseContext(&ctx);
		return ok && outBuf.size() >= 8;
	}
	return false;
}

void ScraperThreadMain(void * arg)
{
	(void)arg;
	s_active.store(true, std::memory_order_release);

	// Check 3DS Wi-Fi connectivity first; exit silently if offline
	bool wifiReady = false;
	if (R_SUCCEEDED(acInit()))
	{
		u32 wifiStatus = 0;
		if (R_SUCCEEDED(ACU_GetWifiStatus(&wifiStatus)) && wifiStatus != 0)
			wifiReady = true;
		acExit();
	}

	if (!wifiReady || R_FAILED(httpcInit(0)))
	{
		s_active.store(false, std::memory_order_release);
		return;
	}

	IO::Directory::EnsureExists(DAEDALUS_CTR_PATH("Resources/"));
	IO::Directory::EnsureExists(DAEDALUS_CTR_PATH("Resources/Preview/"));

	std::vector<uint8_t> dlBuf;
	std::vector<std::string> urls;

	while (!s_stopReq.load(std::memory_order_acquire))
	{
		ScrapeJob job;
		bool hasJob = false;

		LightLock_Lock(&s_lock);
		if (!s_queue.empty())
		{
			job = s_queue.front();
			s_queue.erase(s_queue.begin());
			hasJob = true;
		}
		LightLock_Unlock(&s_lock);

		if (!hasJob)
			break;

		if (!IO::File::Exists(job.destPath.c_str()))
		{
			BuildCandidateUrls(job.romFilename.c_str(), job.romTitle.c_str(), job.regionCode.c_str(), urls);
			for (const std::string & url : urls)
			{
				if (s_stopReq.load(std::memory_order_acquire))
					break;
				if (HttpDownloadToMemory(url.c_str(), dlBuf))
				{
					if (DownsampleAndSavePng(dlBuf.data(), dlBuf.size(), job.destPath.c_str()))
					{
						s_downloaded.fetch_add(1, std::memory_order_relaxed);
						s_revision.fetch_add(1, std::memory_order_release);
						break;
					}
				}
			}
		}

		s_completed.fetch_add(1, std::memory_order_relaxed);
	}

	httpcExit();
	s_active.store(false, std::memory_order_release);
}

} // namespace

void Start(const std::vector<ScrapeJob> & jobs)
{
	Stop();
	if (jobs.empty())
		return;

	if (!s_lockInit)
	{
		LightLock_Init(&s_lock);
		s_lockInit = true;
	}

	LightLock_Lock(&s_lock);
	s_queue = jobs;
	LightLock_Unlock(&s_lock);

	s_stopReq.store(false, std::memory_order_release);
	s_completed.store(0, std::memory_order_relaxed);
	s_total.store((int)jobs.size(), std::memory_order_relaxed);
	s_downloaded.store(0, std::memory_order_relaxed);

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_thread = threadCreate(ScraperThreadMain, nullptr, 32 * 1024, prio + 1, -2, false);
}

void Prioritize(const char * destPath)
{
	if (!destPath || !destPath[0] || !s_lockInit || !s_active.load(std::memory_order_relaxed))
		return;
	LightLock_Lock(&s_lock);
	for (size_t i = 1; i < s_queue.size(); ++i)
	{
		if (s_queue[i].destPath == destPath)
		{
			ScrapeJob j = s_queue[i];
			s_queue.erase(s_queue.begin() + i);
			s_queue.insert(s_queue.begin(), j);
			break;
		}
	}
	LightLock_Unlock(&s_lock);
}

void Stop()
{
	s_stopReq.store(true, std::memory_order_release);
	if (s_thread != nullptr)
	{
		threadJoin(s_thread, U64_MAX);
		threadFree(s_thread);
		s_thread = nullptr;
	}
	if (s_lockInit)
	{
		LightLock_Lock(&s_lock);
		s_queue.clear();
		LightLock_Unlock(&s_lock);
	}
	s_active.store(false, std::memory_order_release);
}

ScraperStatus GetStatus()
{
	ScraperStatus st;
	st.active     = s_active.load(std::memory_order_acquire);
	st.completed  = s_completed.load(std::memory_order_relaxed);
	st.total      = s_total.load(std::memory_order_relaxed);
	st.downloaded = s_downloaded.load(std::memory_order_relaxed);
	st.revision   = s_revision.load(std::memory_order_acquire);
	return st;
}

#endif // DAEDALUS_CTR

} // namespace CoverScraper
} // namespace UI
