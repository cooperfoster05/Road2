// Offline preview of Road2's windows: draws the real UI code into a PNG-ready RGBA file.
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <filesystem>
#include <functional>
#include <sstream>
#include <fstream>
#include <condition_variable>
#include <deque>
#include <optional>
#include <format>
#include <random>
#include <array>
#include <future>


#include "src/pch.h"
#include "RLCoach.h"
#include "UiFonts.h"
#include "UiStyle.h"
#undef private
#undef protected


static int W = 1920, H = 1080;
static std::vector<unsigned char> fb;
static unsigned char* atlas; static int aw, ah;

static void Raster(ImDrawData* dd)
{
	for (int n = 0; n < dd->CmdListsCount; n++)
	{
		const ImDrawList* cl = dd->CmdLists[n];
		const ImDrawVert* vb = cl->VtxBuffer.Data;
		const ImDrawIdx* ib = cl->IdxBuffer.Data;
		for (const ImDrawCmd& cmd : cl->CmdBuffer)
		{
			if (cmd.UserCallback) { ib += cmd.ElemCount; continue; }
			const int cx0 = std::max(0, (int)cmd.ClipRect.x), cy0 = std::max(0, (int)cmd.ClipRect.y), cx1 = std::min(W, (int)cmd.ClipRect.z), cy1 = std::min(H, (int)cmd.ClipRect.w);
			for (unsigned i = 0; i + 2 < cmd.ElemCount + 0u; i += 3)
			{
				const ImDrawVert& a = vb[cmd.VtxOffset + ib[i]], & b = vb[cmd.VtxOffset + ib[i + 1]], & c = vb[cmd.VtxOffset + ib[i + 2]];
				const float minx = std::min({ a.pos.x, b.pos.x, c.pos.x }), maxx = std::max({ a.pos.x, b.pos.x, c.pos.x });
				const float miny = std::min({ a.pos.y, b.pos.y, c.pos.y }), maxy = std::max({ a.pos.y, b.pos.y, c.pos.y });
				const int x0 = std::max(cx0, (int)std::floor(minx)), x1 = std::min(cx1, (int)std::ceil(maxx));
				const int y0 = std::max(cy0, (int)std::floor(miny)), y1 = std::min(cy1, (int)std::ceil(maxy));
				const float den = (b.pos.y - c.pos.y) * (a.pos.x - c.pos.x) + (c.pos.x - b.pos.x) * (a.pos.y - c.pos.y);
				if (std::fabs(den) < 1e-9f) continue;
				const bool flatUv = a.uv.x == b.uv.x && a.uv.y == b.uv.y && a.uv.x == c.uv.x && a.uv.y == c.uv.y;
				for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++)
				{
					// 2x2 supersample for coverage
					float cov = 0, w0s = 0, w1s = 0, w2s = 0;
					for (int sy = 0; sy < 1; sy++) for (int sx = 0; sx < 1; sx++)
					{
						const float px = x + 0.5007f, py = y + 0.5013f;
						const float w0 = ((b.pos.y - c.pos.y) * (px - c.pos.x) + (c.pos.x - b.pos.x) * (py - c.pos.y)) / den;
						const float w1 = ((c.pos.y - a.pos.y) * (px - c.pos.x) + (a.pos.x - c.pos.x) * (py - c.pos.y)) / den;
						const float w2 = 1 - w0 - w1;
						if (w0 < 0 || w1 < 0 || w2 < 0) continue;
						cov += 1.0f; w0s += w0; w1s += w1; w2s += w2;
					}
					if (cov <= 0) continue;
					const float inv = 1.0f;
					const float w0 = w0s * inv, w1 = w1s * inv, w2 = w2s * inv;
					float col[4];
					for (int k = 0; k < 4; k++)
						col[k] = (w0 * ((a.col >> (8 * k)) & 255) + w1 * ((b.col >> (8 * k)) & 255) + w2 * ((c.col >> (8 * k)) & 255)) / 255.0f;
					float ta = 1.0f;
					if (!flatUv)
					{
						const float u = (w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x) * aw - 0.5f, v = (w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y) * ah - 0.5f;
						const int ux = std::clamp((int)std::floor(u), 0, aw - 2), vy = std::clamp((int)std::floor(v), 0, ah - 2);
						const float fx = std::clamp(u - ux, 0.0f, 1.0f), fy = std::clamp(v - vy, 0.0f, 1.0f);
						auto A = [&](int xx, int yy) { return atlas[(yy * aw + xx) * 4 + 3] / 255.0f; };
						ta = (A(ux, vy) * (1 - fx) + A(ux + 1, vy) * fx) * (1 - fy) + (A(ux, vy + 1) * (1 - fx) + A(ux + 1, vy + 1) * fx) * fy;
					}
					else ta = atlas[((int)(a.uv.y * ah) * aw + (int)(a.uv.x * aw)) * 4 + 3] / 255.0f;
					const float al = col[3] * ta * cov;
					unsigned char* p = &fb[(y * W + x) * 4];
					for (int k = 0; k < 3; k++) p[k] = (unsigned char)std::clamp(col[k] * 255.0f * al + p[k] * (1 - al), 0.0f, 255.0f);
				}
			}
			ib += cmd.ElemCount;
		}
	}
}

#include <signal.h>
#include <ucontext.h>
// Calls into BakkesMod (not linked here) land on address 0: treat each as a function that does nothing and returns 0
static void OnSegv(int, siginfo_t*, void* ctx)
{
	ucontext_t* uc = (ucontext_t*)ctx;
	auto& g = uc->uc_mcontext.gregs;
	if (g[REG_RIP] != 0) { fprintf(stderr, "real crash at %llx\n", (unsigned long long)g[REG_RIP]); _exit(3); }
	g[REG_RIP] = *(long long*)g[REG_RSP];
	g[REG_RSP] += 8;
	g[REG_RAX] = 0;
	if (uc->uc_mcontext.fpregs) memset(&uc->uc_mcontext.fpregs->_xmm[0], 0, sizeof(uc->uc_mcontext.fpregs->_xmm[0]));
}
#include "scenes.inc"

int main(int argc, char** argv)
{
	const std::string scene = argc > 1 ? argv[1] : "list";
	const std::string out = argc > 2 ? argv[2] : "out.rgba";
	const std::string data = "/home/claude/h/data";
	struct sigaction sa{}; sa.sa_sigaction = OnSegv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigaction(SIGSEGV, &sa, nullptr);
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)W, (float)H);
	io.DeltaTime = 1.0f / 60.0f;
	io.IniFilename = nullptr;
	io.Fonts->AddFontDefault();
	ImFont* fonts[UiFonts::kCount] = {};
	for (int i = 0; i < UiFonts::kCount; i++)
		fonts[i] = io.Fonts->AddFontFromFileTTF((data + "/fonts/" + UiFonts::kSpecs[i].source).c_str(), (float)UiFonts::kSpecs[i].size, nullptr, UiFonts::Ranges());
	io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);
	io.Fonts->TexID = (ImTextureID)1;

	RLCoach& p = *new RLCoach();
#include "cvars.inc"
	p.hudX = std::make_shared<float>(0.02f); p.hudY = std::make_shared<float>(0.2f);
	p.padX = std::make_shared<float>(0.02f); p.padY = std::make_shared<float>(0.8f); p.padScale = std::make_shared<float>(1.0f);
	p.padJump = std::make_shared<int>(0); p.padBoost = std::make_shared<int>(1); p.padSlide = std::make_shared<int>(2); p.padRollLeft = std::make_shared<int>(3); p.padRollRight = std::make_shared<int>(4);
	*p.welcomeDone = true;
	for (int i = 0; i < UiFonts::kCount; i++) p.uiFonts[i] = fonts[i];
	p.writer = std::make_unique<RepWriter>(data);
	p.predictor.ready = true;
	p.isWindowOpen_ = true;
	Setup(p, scene);

	fb.assign(W * H * 4, 255);
	// a stand-in for the game behind the window: a pitch-green to sky gradient with some bright detail
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
	{
		unsigned char* q = &fb[(y * W + x) * 4];
		const float t = y / (float)H;
		q[0] = (unsigned char)(60 + 40 * (1 - t) + ((x / 64 + y / 64) % 2) * 10);
		q[1] = (unsigned char)(110 + 30 * t);
		q[2] = (unsigned char)(160 * (1 - t) + 50);
	}
	const int frames = argc > 3 ? atoi(argv[3]) : 8;
	for (int f = 0; f < frames; f++)
	{
		io.MousePos = MousePos(scene);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(40, 20), ImGuiCond_Always);
		Frame(p, scene, f);
		ImGui::Render();
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
	}
	Raster(ImGui::GetDrawData());
	FILE* fo = fopen(out.c_str(), "wb");
	fwrite(fb.data(), 1, fb.size(), fo);
	fclose(fo);
	fprintf(stderr, "ok %s\n", scene.c_str());
	_exit(0);
}
