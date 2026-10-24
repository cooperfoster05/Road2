#pragma once
// A small neural network (fully connected, leaky ReLU) with its own backprop and the Adam optimizer.
// Plain C++ on purpose: no PyTorch/LibTorch to install, builds with the plugin's Visual Studio.
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

struct MLP
{
	std::vector<int> sizes;                  // input, hidden..., output
	std::vector<std::vector<float>> W, b;     // W[l] is in x out (row = input), b[l] is out

	void Init(const std::vector<int>& layerSizes, std::mt19937& rng, float lastScale);
	size_t ParamCount() const;
	int In() const { return sizes.front(); }
	int Out() const { return sizes.back(); }

	// Scratch for one batch: the activations of every layer (kept for backprop)
	struct Work
	{
		std::vector<std::vector<float>> h;    // h[0] = input copy, h[l] = layer l output
		std::vector<std::vector<float>> d;    // gradients
	};
	// Forward for B rows of x (B x In). Returns a pointer to the output (B x Out) inside w.
	const float* Forward(const float* x, int B, Work& w) const;
	// Backward: dOut is B x Out. Adds parameter gradients into gW / gb (same shapes as W / b).
	void Backward(const float* dOut, int B, Work& w, std::vector<std::vector<float>>& gW, std::vector<std::vector<float>>& gb) const;

	void ZeroLike(std::vector<std::vector<float>>& gW, std::vector<std::vector<float>>& gb) const;
	bool Write(std::FILE* f) const;
	bool Read(std::FILE* f);
};

struct Adam
{
	float lr = 2e-4f, b1 = 0.9f, b2 = 0.999f, eps = 1e-7f;
	long long t = 0;
	std::vector<std::vector<float>> mW, vW, mb, vb;
	void Init(const MLP& net);
	// Steps the net with these (already averaged) gradients; clips the global gradient norm to maxNorm first
	float Step(MLP& net, std::vector<std::vector<float>>& gW, std::vector<std::vector<float>>& gb, float maxNorm);
	bool Write(std::FILE* f) const;
	bool Read(std::FILE* f);
};

// Running mean / variance of the observations (so every input is on a similar scale)
struct ObsNorm
{
	int n = 0;
	double count = 1e-4;
	std::vector<double> mean, m2;
	void Init(int size) { n = size; mean.assign(size, 0.0); m2.assign(size, 0.0); count = 1e-4; }
	void Add(const float* rows, int B);
	void Apply(float* row) const;   // in place, clipped to +-5
	bool Write(std::FILE* f) const;
	bool Read(std::FILE* f);
};
