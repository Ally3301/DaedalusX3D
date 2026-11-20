#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace UI
{

namespace CoverScraper
{

// Pure, host-testable URL encoding & libretro-thumbnails candidate builder.
std::string UrlEncodePathComponent(const char * text);
void BuildCandidateUrls(const char * romFilename,
                        const char * romTitle,
                        const char * regionCode,
                        std::vector<std::string> & outUrls);

struct ScrapeJob
{
	std::string romFilename;
	std::string romTitle;
	std::string regionCode;
	std::string destPath;
};

struct ScraperStatus
{
	bool active;
	int  completed;
	int  total;
	int  downloaded;
	uint32_t revision; // Incremented each time a new cover is saved to SD
};

#ifdef DAEDALUS_CTR
// Starts a low-priority background thread on 3DS (Wi-Fi checked via ACU) to fetch
// missing N64 boxarts from libretro-thumbnails, downsample them to <= 128x128 PNG,
// and save them under sdmc:/3ds/DaedalusX64/Resources/Preview/.
void Start(const std::vector<ScrapeJob> & jobs);

// Prioritize a specific destination path (e.g., the currently highlighted ROM).
void Prioritize(const char * destPath);

// Cleanly stops and joins the background thread (must be called before gameplay!).
void Stop();

// Non-blocking snapshot of download progress for the ROM Browser footer.
ScraperStatus GetStatus();
#endif

} // namespace CoverScraper

} // namespace UI
