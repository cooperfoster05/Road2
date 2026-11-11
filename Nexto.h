#pragma once
// Nexto (Nov 4): the RLGym community bot (Rolv, Soren and contributors, github.com/Rolv-Arild/Necto), run inside
// Road2 so the Road2 Bot can play for real. About Grand Champion 1 level in 1v1, by its authors' measure.
// The network is a small attention model: you (a query) look at every other thing on the field (the cars, the ball,
// the 34 boost pads) and it scores 90 fixed control choices. A fresh choice every 8 ticks, as it was trained.
// Its weights (nexto.bin, from the original nexto-model.pt) are CC BY-NC-SA 4.0: free, credited, never sold.
// This file has no BakkesMod or RocketSim in it, so the same code runs in the plugin and in the test tools.
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Nexto
{
	constexpr int kActions = 90;
	constexpr int kPads = 34;
	constexpr int kTickSkip = 8;

	struct Car
	{
		float pos[3]{}, vel[3]{}, fw[3]{ 1, 0, 0 }, up[3]{ 0, 0, 1 }, ang[3]{};   // world units, rad/s
		float boost = 0.0f;       // 0-1
		bool onGround = false, hasFlip = false, demoed = false;
		int team = 0;             // 0 blue, 1 orange
	};
	struct Ball { float pos[3]{}, vel[3]{}, ang[3]{}; };

	// Controls as Nexto picks them: throttle, steer, pitch, yaw, roll, jump, boost, handbrake (each -1..1 or 0/1)
	using Controls = std::array<float, 8>;

	// The pads in the order Nexto knows them (x, y, z); z 73 = a big pad
	extern const float kPadPos[kPads][3];
	// Which pad a pickup at (x, y) was (-1 if none is close)
	int PadAt(float x, float y);

	class Net
	{
	public:
		bool Load(const std::filesystem::path& file, std::string& error);
		bool Ready() const { return ready; }
		// One decision. `others` are every other car (teammates and opponents, by their team). `prev` is the controls
		// it chose last time (zeros at the start). beta 1 = always its best choice; below 1 it samples (0.5 = by its
		// own odds), which is how the strength is turned down. Returns the choice (0-89); logits filled if given.
		int Act(const Car& self, const std::vector<Car>& others, const Ball& ball, const bool padsUp[kPads],
			const Controls& prev, float beta, uint32_t seed, float* logits = nullptr) const;
		const Controls& Action(int i) const { return lut[static_cast<size_t>(i)]; }

	private:
		struct Lin { int in = 0, out = 0; std::vector<float> w, b; };
		struct LN { std::vector<float> w, b; };
		struct Block { LN n1, n2, n3; std::vector<float> inW, inB; Lin outP, l1, l2; };
		Lin q0, q2, k0, k2;
		Block blocks[2];
		Lin o0, o2, o4, emb;
		std::vector<Controls> lut;
		std::vector<float> actEmb;   // the 90 actions through the output net, once (90 x 32)
		bool ready = false;
	};
}
