#include "Nexto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

namespace Nexto
{
	const float kPadPos[kPads][3] = {
		{ 0.0f, -4240.0f, 70.0f }, { -1792.0f, -4184.0f, 70.0f }, { 1792.0f, -4184.0f, 70.0f }, { -3072.0f, -4096.0f, 73.0f },
		{ 3072.0f, -4096.0f, 73.0f }, { -940.0f, -3308.0f, 70.0f }, { 940.0f, -3308.0f, 70.0f }, { 0.0f, -2816.0f, 70.0f },
		{ -3584.0f, -2484.0f, 70.0f }, { 3584.0f, -2484.0f, 70.0f }, { -1788.0f, -2300.0f, 70.0f }, { 1788.0f, -2300.0f, 70.0f },
		{ -2048.0f, -1036.0f, 70.0f }, { 0.0f, -1024.0f, 70.0f }, { 2048.0f, -1036.0f, 70.0f }, { -3584.0f, 0.0f, 73.0f },
		{ -1024.0f, 0.0f, 70.0f }, { 1024.0f, 0.0f, 70.0f }, { 3584.0f, 0.0f, 73.0f }, { -2048.0f, 1036.0f, 70.0f },
		{ 0.0f, 1024.0f, 70.0f }, { 2048.0f, 1036.0f, 70.0f }, { -1788.0f, 2300.0f, 70.0f }, { 1788.0f, 2300.0f, 70.0f },
		{ -3584.0f, 2484.0f, 70.0f }, { 3584.0f, 2484.0f, 70.0f }, { 0.0f, 2816.0f, 70.0f }, { -940.0f, 3310.0f, 70.0f },
		{ 940.0f, 3308.0f, 70.0f }, { -3072.0f, 4096.0f, 73.0f }, { 3072.0f, 4096.0f, 73.0f }, { -1792.0f, 4184.0f, 70.0f },
		{ 1792.0f, 4184.0f, 70.0f }, { 0.0f, 4240.0f, 70.0f },
	};

	int PadAt(float x, float y)
	{
		int best = -1;
		float bd = 250.0f * 250.0f;
		for (int i = 0; i < kPads; i++)
		{
			const float dx = x - kPadPos[i][0], dy = y - kPadPos[i][1], d = dx * dx + dy * dy;
			if (d < bd) { bd = d; best = i; }
		}
		return best;
	}

	namespace
	{
		constexpr int D = 128, H = 4, HD = 32, F = 24, Q = 32, E = 32;

		void Linear(const float* x, int in, const std::vector<float>& w, const std::vector<float>& b, int out, float* y)
		{
			for (int o = 0; o < out; o++)
			{
				const float* row = w.data() + static_cast<size_t>(o) * in;
				float s = b[o];
				for (int i = 0; i < in; i++) s += row[i] * x[i];
				y[o] = s;
			}
		}
		void Relu(float* x, int n) { for (int i = 0; i < n; i++) x[i] = std::max(0.0f, x[i]); }
		void LayerNorm(const float* x, const std::vector<float>& w, const std::vector<float>& b, float* y)
		{
			double m = 0, v = 0;
			for (int i = 0; i < D; i++) m += x[i];
			m /= D;
			for (int i = 0; i < D; i++) v += (x[i] - m) * (x[i] - m);
			v /= D;
			const float inv = static_cast<float>(1.0 / std::sqrt(v + 1e-5));
			for (int i = 0; i < D; i++) y[i] = static_cast<float>(x[i] - m) * inv * w[i] + b[i];
		}

		// The relative frame (Nexto's convert_to_relative): positions from you, then x/y turned so you face +y
		void Relative(float* row, const float* q)
		{
			for (int k = 0; k < 3; k++) row[5 + k] -= q[5 + k];
			const float theta = std::atan2(q[11], q[12]);
			const float ct = std::cos(theta), st = std::sin(theta);
			for (int c = 5; c < 20; c += 3)
			{
				const float x = row[c], y = row[c + 1];
				row[c] = ct * x - st * y;
				row[c + 1] = st * x + ct * y;
			}
		}
	}

	bool Net::Load(const std::filesystem::path& file, std::string& error)
	{
		ready = false;
		std::ifstream in(file, std::ios::binary);
		if (!in) { error = "no Nexto weights at " + file.string(); return false; }
		char magic[4];
		int32_t count = 0;
		in.read(magic, 4);
		in.read(reinterpret_cast<char*>(&count), 4);
		if (!in || std::memcmp(magic, "NXT1", 4) != 0 || count != 45) { error = "the Nexto weights file isn't the right format"; return false; }
		auto next = [&](size_t want, std::vector<float>& v) {
			int32_t n = 0;
			in.read(reinterpret_cast<char*>(&n), 4);
			if (!in || static_cast<size_t>(n) != want) return false;
			v.resize(want);
			in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(want * sizeof(float)));
			return static_cast<bool>(in);
		};
		auto lin = [&](Lin& l, int i, int o) { l.in = i; l.out = o; return next(static_cast<size_t>(i) * o, l.w) && next(o, l.b); };
		auto ln = [&](LN& l) { return next(D, l.w) && next(D, l.b); };
		bool ok = lin(q0, Q, D) && lin(q2, D, D) && lin(k0, F, D) && lin(k2, D, D);
		for (Block& b : blocks)
			ok = ok && ln(b.n1) && ln(b.n2) && next(3 * D * D, b.inW) && next(3 * D, b.inB) && lin(b.outP, D, D) && ln(b.n3)
				&& lin(b.l1, D, 4 * D) && lin(b.l2, 4 * D, D);
		ok = ok && lin(o0, 8, E) && lin(o2, E, E) && lin(o4, E, E) && lin(emb, D, E);
		std::vector<float> table;
		ok = ok && next(static_cast<size_t>(kActions) * 8, table);
		if (!ok) { error = "the Nexto weights file is cut short or the wrong size"; return false; }
		lut.assign(kActions, Controls{});
		for (int a = 0; a < kActions; a++) for (int j = 0; j < 8; j++) lut[a][j] = table[static_cast<size_t>(a) * 8 + j];
		// The actions' side of the output never changes: run it once
		actEmb.assign(static_cast<size_t>(kActions) * E, 0.0f);
		for (int a = 0; a < kActions; a++)
		{
			float h1[E], h2[E];
			Linear(lut[a].data(), 8, o0.w, o0.b, E, h1); Relu(h1, E);
			Linear(h1, E, o2.w, o2.b, E, h2); Relu(h2, E);
			Linear(h2, E, o4.w, o4.b, E, actEmb.data() + static_cast<size_t>(a) * E); Relu(actEmb.data() + static_cast<size_t>(a) * E, E);
		}
		ready = true;
		return true;
	}

	int Net::Act(const Car& self, const std::vector<Car>& others, const Ball& ball, const bool padsUp[kPads],
		const Controls& prev, float beta, uint32_t seed, float* logitsOut) const
	{
		if (!ready) return 0;
		// ---- The observation (nexto_obs.py) ----
		static const float kInvert[F] = { 1, 1, 1, 1, 1, -1, -1, 1, -1, -1, 1, -1, -1, 1, -1, -1, 1, -1, -1, 1, 1, 1, 1, 1 };
		static const float kNorm[F] = { 1, 1, 1, 1, 1, 2300, 2300, 2300, 2300, 2300, 2300, 1, 1, 1, 1, 1, 1, 5.5f, 5.5f, 5.5f, 1, 1, 1, 1 };
		const int nCars = 1 + static_cast<int>(others.size());
		const int N = nCars + 1 + kPads;
		std::vector<float> kv(static_cast<size_t>(N) * F, 0.0f);
		auto carRow = [&](float* r, const Car& c, bool isSelf) {
			r[0] = isSelf ? 1.0f : 0.0f;
			r[1] = c.team == 0 ? 1.0f : 0.0f;   // "mate" of blue, swapped below for orange
			r[2] = c.team == 1 ? 1.0f : 0.0f;
			for (int k = 0; k < 3; k++) { r[5 + k] = c.pos[k]; r[8 + k] = c.vel[k]; r[11 + k] = c.fw[k]; r[14 + k] = c.up[k]; r[17 + k] = c.ang[k]; }
			r[20] = c.boost;
			r[21] = c.demoed ? 1.0f : 0.0f;
			r[22] = c.onGround ? 1.0f : 0.0f;
			r[23] = c.hasFlip ? 1.0f : 0.0f;
		};
		carRow(&kv[0], self, true);
		for (int i = 0; i < static_cast<int>(others.size()); i++) carRow(&kv[static_cast<size_t>(1 + i) * F], others[i], false);
		{
			float* r = &kv[static_cast<size_t>(nCars) * F];
			r[3] = 1.0f;
			for (int k = 0; k < 3; k++) { r[5 + k] = ball.pos[k]; r[8 + k] = ball.vel[k]; r[17 + k] = ball.ang[k]; }
		}
		for (int p = 0; p < kPads; p++)
		{
			float* r = &kv[static_cast<size_t>(nCars + 1 + p) * F];
			r[4] = 1.0f;
			for (int k = 0; k < 3; k++) r[5 + k] = kPadPos[p][k];
			r[20] = kPadPos[p][2] > 72.0f ? 1.0f : 0.12f;
			r[21] = padsUp[p] ? 1.0f : 0.0f;
		}
		const bool orange = self.team == 1;
		for (int e = 0; e < N; e++)
		{
			float* r = &kv[static_cast<size_t>(e) * F];
			if (orange) { for (int k = 0; k < F; k++) r[k] *= kInvert[k]; std::swap(r[1], r[2]); }
			for (int k = 0; k < F; k++) r[k] /= kNorm[k];
		}
		float q[Q];
		std::memcpy(q, &kv[0], sizeof(float) * F);
		for (int k = 0; k < 8; k++) q[F + k] = prev[k];
		for (int e = 0; e < N; e++) Relative(&kv[static_cast<size_t>(e) * F], q);

		// ---- The network ----
		float x[D], t[D];
		Linear(q, Q, q0.w, q0.b, D, t); Relu(t, D);
		Linear(t, D, q2.w, q2.b, D, x); Relu(x, D);
		std::vector<float> keys(static_cast<size_t>(N) * D);
		for (int e = 0; e < N; e++)
		{
			Linear(&kv[static_cast<size_t>(e) * F], F, k0.w, k0.b, D, t); Relu(t, D);
			float* o = &keys[static_cast<size_t>(e) * D];
			Linear(t, D, k2.w, k2.b, D, o); Relu(o, D);
		}
		std::vector<float> kn(static_cast<size_t>(N) * D), K(static_cast<size_t>(N) * D), V(static_cast<size_t>(N) * D);
		const float scale = 1.0f / std::sqrt(static_cast<float>(HD));
		for (const Block& b : blocks)
		{
			float xn[D], qv[D];
			LayerNorm(x, b.n1.w, b.n1.b, xn);
			for (int o = 0; o < D; o++)
			{
				const float* row = b.inW.data() + static_cast<size_t>(o) * D;
				float s = b.inB[o];
				for (int i = 0; i < D; i++) s += row[i] * xn[i];
				qv[o] = s;
			}
			for (int e = 0; e < N; e++)
			{
				float* n = &kn[static_cast<size_t>(e) * D];
				LayerNorm(&keys[static_cast<size_t>(e) * D], b.n2.w, b.n2.b, n);
				for (int o = 0; o < D; o++)
				{
					const float* rk = b.inW.data() + static_cast<size_t>(D + o) * D;
					const float* rv = b.inW.data() + static_cast<size_t>(2 * D + o) * D;
					float sk = b.inB[D + o], sv = b.inB[2 * D + o];
					for (int i = 0; i < D; i++) { sk += rk[i] * n[i]; sv += rv[i] * n[i]; }
					K[static_cast<size_t>(e) * D + o] = sk;
					V[static_cast<size_t>(e) * D + o] = sv;
				}
			}
			float att[D];
			std::vector<float> w(N);
			for (int h = 0; h < H; h++)
			{
				float mx = -1e30f;
				for (int e = 0; e < N; e++)
				{
					float s = 0.0f;
					for (int d = 0; d < HD; d++) s += qv[h * HD + d] * scale * K[static_cast<size_t>(e) * D + h * HD + d];
					w[e] = s;
					mx = std::max(mx, s);
				}
				float sum = 0.0f;
				for (int e = 0; e < N; e++) { w[e] = std::exp(w[e] - mx); sum += w[e]; }
				for (int d = 0; d < HD; d++)
				{
					float s = 0.0f;
					for (int e = 0; e < N; e++) s += w[e] * V[static_cast<size_t>(e) * D + h * HD + d];
					att[h * HD + d] = s / sum;
				}
			}
			float a[D];
			Linear(att, D, b.outP.w, b.outP.b, D, a);
			for (int i = 0; i < D; i++) x[i] += a[i];
			float xn3[D], hid[4 * D], ff[D];
			LayerNorm(x, b.n3.w, b.n3.b, xn3);
			Linear(xn3, D, b.l1.w, b.l1.b, 4 * D, hid); Relu(hid, 4 * D);
			Linear(hid, 4 * D, b.l2.w, b.l2.b, D, ff);
			for (int i = 0; i < D; i++) x[i] += ff[i];
		}
		Relu(x, D);
		float e32[E];
		Linear(x, D, emb.w, emb.b, E, e32);
		float logits[kActions];
		for (int a = 0; a < kActions; a++)
		{
			float s = 0.0f;
			for (int k = 0; k < E; k++) s += actEmb[static_cast<size_t>(a) * E + k] * e32[k];
			logits[a] = s;
		}
		if (logitsOut) std::memcpy(logitsOut, logits, sizeof(logits));

		// ---- The choice (agent.py) ----
		if (beta >= 1.0f) return static_cast<int>(std::max_element(logits, logits + kActions) - logits);
		const float k = beta <= 0.0f ? 0.0f : std::log((beta + 1.0f) / (1.0f - beta)) / std::log(3.0f);
		float mx = -1e30f;
		for (float l : logits) mx = std::max(mx, l * k);
		double p[kActions], sum = 0;
		for (int a = 0; a < kActions; a++) { p[a] = std::exp(logits[a] * k - mx); sum += p[a]; }
		std::mt19937 rng(seed);
		double u = std::uniform_real_distribution<double>(0.0, sum)(rng);
		for (int a = 0; a < kActions; a++) { u -= p[a]; if (u <= 0) return a; }
		return kActions - 1;
	}
}
