#pragma once
// The result card (Nov 10): a finished Combine set as a 1200 x 630 PNG to post anywhere: the event, the level, the
// score and medal, how each of the 10 went, and the code so anyone can play the same set. Drawn here in plain C++
// (the fonts are Road2's own, read with stb_truetype; the PNG is written with miniz), so it looks the same on every PC.
// Include this from one .cpp only (it carries its own copy of stb_truetype).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "third_party/miniz/miniz.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "IMGUI/imstb_truetype.h"

namespace ResultCard
{
	struct Data
	{
		std::string event;        // "SAVES"
		std::string level;        // "Sharp"
		std::string medal;        // "Gold", "Silver", "Bronze" or ""
		std::string code;         // "SV3-1A2B3C4D"
		int points = 0, maxPoints = 30;
		std::vector<int> each;    // points per item (-1 = didn't count)
	};

	namespace detail
	{
		struct Rgb { unsigned char r, g, b; };
		constexpr int W = 1200, H = 630;
		constexpr float kSlant = 0.2126f;   // the 12 degree slant of style A

		struct Canvas
		{
			std::vector<unsigned char> px = std::vector<unsigned char>(static_cast<size_t>(W) * H * 3);
			void Blend(int x, int y, Rgb c, float a)
			{
				if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.0f) return;
				unsigned char* p = &px[(static_cast<size_t>(y) * W + x) * 3];
				a = std::min(a, 1.0f);
				p[0] = static_cast<unsigned char>(p[0] + (c.r - p[0]) * a);
				p[1] = static_cast<unsigned char>(p[1] + (c.g - p[1]) * a);
				p[2] = static_cast<unsigned char>(p[2] + (c.b - p[2]) * a);
			}
			void Fill(Rgb c) { for (size_t i = 0; i < px.size(); i += 3) { px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b; } }
			// A parallelogram leaning right: (x, y) is its top-left before the lean; soft left and right edges
			void Slant(float x, float y, float w, float h, Rgb c)
			{
				for (int yy = static_cast<int>(y); yy < static_cast<int>(y + h); yy++)
				{
					const float shift = (y + h - yy) * kSlant;
					const float x0 = x + shift, x1 = x + w + shift;
					for (int xx = static_cast<int>(std::floor(x0)); xx <= static_cast<int>(x1); xx++)
						Blend(xx, yy, c, std::min(xx + 1.0f - x0, 1.0f) * std::min(x1 - xx, 1.0f));
				}
			}
		};

		struct Font
		{
			std::vector<unsigned char> file;
			stbtt_fontinfo info{};
			bool ok = false;
			bool Load(const std::filesystem::path& p)
			{
				std::ifstream f(p, std::ios::binary);
				file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
				ok = !file.empty() && stbtt_InitFont(&info, file.data(), stbtt_GetFontOffsetForIndex(file.data(), 0)) != 0;
				return ok;
			}
			float Width(const std::string& s, float px) const
			{
				const float sc = stbtt_ScaleForPixelHeight(&info, px);
				float w = 0;
				for (size_t i = 0; i < s.size(); i++)
				{
					int adv = 0, lsb = 0;
					stbtt_GetCodepointHMetrics(&info, static_cast<unsigned char>(s[i]), &adv, &lsb);
					w += adv * sc;
					if (i + 1 < s.size()) w += stbtt_GetCodepointKernAdvance(&info, static_cast<unsigned char>(s[i]), static_cast<unsigned char>(s[i + 1])) * sc;
				}
				return w;
			}
			// Draws with its baseline at y; returns the x after the last letter
			float Draw(Canvas& cv, float x, float y, const std::string& s, float px, Rgb c) const
			{
				const float sc = stbtt_ScaleForPixelHeight(&info, px);
				for (size_t i = 0; i < s.size(); i++)
				{
					const int cp = static_cast<unsigned char>(s[i]);
					int w = 0, h = 0, ox = 0, oy = 0;
					unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&info, sc, sc, x - std::floor(x), 0.0f, cp, &w, &h, &ox, &oy);
					if (bmp)
					{
						for (int yy = 0; yy < h; yy++)
							for (int xx = 0; xx < w; xx++)
								cv.Blend(static_cast<int>(std::floor(x)) + ox + xx, static_cast<int>(y) + oy + yy, c, bmp[yy * w + xx] / 255.0f);
						stbtt_FreeBitmap(bmp, nullptr);
					}
					int adv = 0, lsb = 0;
					stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
					x += adv * sc;
					if (i + 1 < s.size()) x += stbtt_GetCodepointKernAdvance(&info, cp, static_cast<unsigned char>(s[i + 1])) * sc;
				}
				return x;
			}
		};
		inline std::string Upper(std::string s) { for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return s; }
	}

	// fonts: the folder with BarlowCondensed-ExtraBoldItalic.ttf and BarlowCondensed-Bold.ttf (data\Road2\fonts)
	inline bool Save(const std::filesystem::path& png, const std::filesystem::path& fonts, const Data& d, std::string& error)
	{
		using namespace detail;
		Font heavy, bold;
		if (!heavy.Load(fonts / "BarlowCondensed-ExtraBoldItalic.ttf") || !bold.Load(fonts / "BarlowCondensed-Bold.ttf")) { error = "the fonts aren't in " + fonts.string(); return false; }
		const Rgb navy{ 11, 15, 26 }, panel{ 18, 26, 44 }, track{ 26, 36, 64 }, blue{ 46, 139, 255 }, orange{ 255, 138, 31 }, white{ 238, 242, 248 },
			sub{ 167, 179, 200 }, good{ 61, 220, 132 }, warn{ 255, 176, 32 }, bad{ 255, 90, 90 }, ink{ 4, 16, 31 }, silver{ 199, 208, 222 }, bronze{ 205, 140, 80 };
		Canvas cv;
		cv.Fill(navy);
		cv.Slant(-60, 0, 1400, 150, panel);                      // the header band
		cv.Slant(-40, 150, 1400, 6, blue);
		// ROAD2, and what this is
		float x = heavy.Draw(cv, 56, 104, "ROAD", 92, white);
		heavy.Draw(cv, x, 104, "2", 92, orange);
		{
			const std::string tag = "THE COMBINE";
			const float tw = bold.Width(tag, 34);
			cv.Slant(1200 - 56 - tw - 44, 56, tw + 36, 54, track);
			bold.Draw(cv, 1200 - 56 - tw - 44 + 18 + 54 * kSlant * 0.5f, 95, tag, 34, sub);
		}
		// The event and level
		{
			const std::string ev = Upper(d.event);
			const float ew = heavy.Width(ev, 78);
			cv.Slant(56, 196, ew + 52, 96, blue);
			heavy.Draw(cv, 56 + 26 + 96 * kSlant * 0.5f, 270, ev, 78, white);
			bold.Draw(cv, 56 + ew + 52 + 96 * kSlant + 26, 262, Upper(d.level), 50, sub);
		}
		// The score, in its medal's colour
		const Rgb medalCol = d.medal == "Gold" ? warn : d.medal == "Silver" ? silver : d.medal == "Bronze" ? bronze : sub;
		{
			const std::string pts = std::to_string(d.points), of = " / " + std::to_string(d.maxPoints);
			float sx = heavy.Draw(cv, 60, 468, pts, 190, medalCol);
			heavy.Draw(cv, sx + 6, 468, of, 84, sub);
			if (!d.medal.empty())
			{
				const std::string m = Upper(d.medal);
				const float mw = heavy.Width(m, 60);
				cv.Slant(1200 - 56 - mw - 64, 352, mw + 56, 84, medalCol);
				heavy.Draw(cv, 1200 - 56 - mw - 64 + 28 + 84 * kSlant * 0.5f, 414, m, 60, ink);
			}
		}
		// How each one went
		{
			const int n = std::max(1, static_cast<int>(d.each.size()));
			const float gap = 10, w = (1200 - 112 - 44 * kSlant - gap * (n - 1)) / n;
			for (int i = 0; i < n && i < static_cast<int>(d.each.size()); i++)
			{
				const int p = d.each[i];
				cv.Slant(56 + i * (w + gap), 500, w, 44, p < 0 ? track : p == 0 ? bad : p >= 3 ? good : warn);
				const std::string t = p < 0 ? "-" : "+" + std::to_string(p);
				bold.Draw(cv, 56 + i * (w + gap) + (w - bold.Width(t, 30)) * 0.5f + 44 * kSlant * 0.5f, 533, t, 30, p < 0 ? sub : ink);
			}
		}
		// The code, and where it's from
		x = bold.Draw(cv, 56, 598, "PLAY THIS SET:  ", 34, sub);
		bold.Draw(cv, x, 598, d.code, 34, white);
		{
			const std::string foot = "FREE ROCKET LEAGUE TRAINING  /  BAKKESMOD";
			bold.Draw(cv, 1200 - 56 - bold.Width(foot, 26), 598, foot, 26, sub);
		}
		size_t len = 0;
		void* out = tdefl_write_image_to_png_file_in_memory(cv.px.data(), W, H, 3, &len);
		if (!out) { error = "couldn't make the image"; return false; }
		std::error_code ec;
		std::filesystem::create_directories(png.parent_path(), ec);
		std::ofstream f(png, std::ios::binary | std::ios::trunc);
		f.write(static_cast<const char*>(out), static_cast<std::streamsize>(len));
		mz_free(out);
		if (!f) { error = "couldn't write " + png.string(); return false; }
		return true;
	}
}
