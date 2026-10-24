#include "Net.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace
{
	constexpr float kLeak = 0.01f;

	// Dot product with 8 running sums, so the compiler can use vector instructions without fast-math
	inline float Dot(const float* a, const float* b, int n)
	{
		float acc[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		int j = 0;
		for (; j + 8 <= n; j += 8)
			for (int l = 0; l < 8; l++) acc[l] += a[j + l] * b[j + l];
		float s = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
		for (; j < n; j++) s += a[j] * b[j];
		return s;
	}

	template <class T> bool WriteVec(std::FILE* f, const std::vector<T>& v)
	{
		uint64_t n = v.size();
		return std::fwrite(&n, sizeof(n), 1, f) == 1 && (n == 0 || std::fwrite(v.data(), sizeof(T), n, f) == n);
	}
	template <class T> bool ReadVec(std::FILE* f, std::vector<T>& v)
	{
		uint64_t n = 0;
		if (std::fread(&n, sizeof(n), 1, f) != 1 || n > (1ull << 32)) return false;
		v.resize(n);
		return n == 0 || std::fread(v.data(), sizeof(T), n, f) == n;
	}
}

void MLP::Init(const std::vector<int>& s, std::mt19937& rng, float lastScale)
{
	sizes = s;
	const int L = static_cast<int>(s.size()) - 1;
	W.assign(L, {});
	b.assign(L, {});
	for (int l = 0; l < L; l++)
	{
		const int in = s[l], out = s[l + 1];
		W[l].resize(static_cast<size_t>(in) * out);
		b[l].assign(out, 0.0f);
		// He init for the hidden layers; the last layer starts small (a near-uniform policy, a near-zero value)
		const float scale = (l == L - 1 ? lastScale : 1.0f) * std::sqrt(2.0f / in);
		std::normal_distribution<float> nd(0.0f, scale);
		for (float& w : W[l]) w = nd(rng);
	}
}

size_t MLP::ParamCount() const
{
	size_t n = 0;
	for (size_t l = 0; l < W.size(); l++) n += W[l].size() + b[l].size();
	return n;
}

// ---- The three matrix kernels. AVX2 + FMA when the build has it (Coop's 3700X does: /arch:AVX2), plain loops otherwise ----
namespace
{
	// Y[i][j] = b[j] + sum_k X[i][k] W[k][j]   (Y: B x out, X: B x in, W: in x out)
	void MatMulAdd(const float* X, const float* W, const float* bias, float* Y, int B, int in, int out)
	{
		for (int i = 0; i < B; i++) std::memcpy(Y + static_cast<size_t>(i) * out, bias, sizeof(float) * out);
		int i = 0;
#if defined(__AVX2__) && defined(__FMA__) || (defined(_MSC_VER) && defined(__AVX2__))
		const int out16 = out / 16 * 16;
		for (; i + 4 <= B; i += 4)
		{
			const float* x0 = X + static_cast<size_t>(i) * in;
			for (int j = 0; j < out16; j += 16)
			{
				__m256 a00 = _mm256_setzero_ps(), a01 = a00, a10 = a00, a11 = a00, a20 = a00, a21 = a00, a30 = a00, a31 = a00;
				for (int k = 0; k < in; k++)
				{
					const float* wk = W + static_cast<size_t>(k) * out + j;
					const __m256 w0 = _mm256_loadu_ps(wk), w1 = _mm256_loadu_ps(wk + 8);
					__m256 xv = _mm256_broadcast_ss(x0 + k);
					a00 = _mm256_fmadd_ps(xv, w0, a00); a01 = _mm256_fmadd_ps(xv, w1, a01);
					xv = _mm256_broadcast_ss(x0 + in + k);
					a10 = _mm256_fmadd_ps(xv, w0, a10); a11 = _mm256_fmadd_ps(xv, w1, a11);
					xv = _mm256_broadcast_ss(x0 + 2 * in + k);
					a20 = _mm256_fmadd_ps(xv, w0, a20); a21 = _mm256_fmadd_ps(xv, w1, a21);
					xv = _mm256_broadcast_ss(x0 + 3 * in + k);
					a30 = _mm256_fmadd_ps(xv, w0, a30); a31 = _mm256_fmadd_ps(xv, w1, a31);
				}
				float* y = Y + static_cast<size_t>(i) * out + j;
				_mm256_storeu_ps(y, _mm256_add_ps(_mm256_loadu_ps(y), a00)); _mm256_storeu_ps(y + 8, _mm256_add_ps(_mm256_loadu_ps(y + 8), a01)); y += out;
				_mm256_storeu_ps(y, _mm256_add_ps(_mm256_loadu_ps(y), a10)); _mm256_storeu_ps(y + 8, _mm256_add_ps(_mm256_loadu_ps(y + 8), a11)); y += out;
				_mm256_storeu_ps(y, _mm256_add_ps(_mm256_loadu_ps(y), a20)); _mm256_storeu_ps(y + 8, _mm256_add_ps(_mm256_loadu_ps(y + 8), a21)); y += out;
				_mm256_storeu_ps(y, _mm256_add_ps(_mm256_loadu_ps(y), a30)); _mm256_storeu_ps(y + 8, _mm256_add_ps(_mm256_loadu_ps(y + 8), a31));
			}
			for (int r = 0; r < 4; r++)   // leftover columns
			{
				const float* xi = x0 + static_cast<size_t>(r) * in;
				float* yi = Y + static_cast<size_t>(i + r) * out;
				for (int k = 0; k < in; k++)
				{
					const float a = xi[k];
					const float* wk = W + static_cast<size_t>(k) * out;
					for (int j = out16; j < out; j++) yi[j] += a * wk[j];
				}
			}
		}
#endif
		for (; i < B; i++)
		{
			const float* xi = X + static_cast<size_t>(i) * in;
			float* yi = Y + static_cast<size_t>(i) * out;
			for (int k = 0; k < in; k++)
			{
				const float a = xi[k];
				const float* wk = W + static_cast<size_t>(k) * out;
				for (int j = 0; j < out; j++) yi[j] += a * wk[j];
			}
		}
	}

	// G[k][j] += sum_i X[i][k] D[i][j]   (G: in x out, X: B x in, D: B x out)
	void GradWBlock(const float* X, const float* D, float* G, int B, int in, int out);
	void GradW(const float* X, const float* D, float* G, int B, int in, int out)
	{
		// In chunks of rows, so the chunk of D being reused stays in the cache
		constexpr int kRows = 96;
		for (int i0 = 0; i0 < B; i0 += kRows)
			GradWBlock(X + static_cast<size_t>(i0) * in, D + static_cast<size_t>(i0) * out, G, std::min(kRows, B - i0), in, out);
	}
	void GradWBlock(const float* X, const float* D, float* G, int B, int in, int out)
	{
		int k = 0;
#if defined(__AVX2__) && defined(__FMA__) || (defined(_MSC_VER) && defined(__AVX2__))
		const int out16 = out / 16 * 16;
		for (; k + 4 <= in; k += 4)
		{
			for (int j = 0; j < out16; j += 16)
			{
				__m256 a00 = _mm256_setzero_ps(), a01 = a00, a10 = a00, a11 = a00, a20 = a00, a21 = a00, a30 = a00, a31 = a00;
				for (int i = 0; i < B; i++)
				{
					const float* di = D + static_cast<size_t>(i) * out + j;
					const __m256 d0 = _mm256_loadu_ps(di), d1 = _mm256_loadu_ps(di + 8);
					const float* xi = X + static_cast<size_t>(i) * in + k;
					__m256 xv = _mm256_broadcast_ss(xi);
					a00 = _mm256_fmadd_ps(xv, d0, a00); a01 = _mm256_fmadd_ps(xv, d1, a01);
					xv = _mm256_broadcast_ss(xi + 1);
					a10 = _mm256_fmadd_ps(xv, d0, a10); a11 = _mm256_fmadd_ps(xv, d1, a11);
					xv = _mm256_broadcast_ss(xi + 2);
					a20 = _mm256_fmadd_ps(xv, d0, a20); a21 = _mm256_fmadd_ps(xv, d1, a21);
					xv = _mm256_broadcast_ss(xi + 3);
					a30 = _mm256_fmadd_ps(xv, d0, a30); a31 = _mm256_fmadd_ps(xv, d1, a31);
				}
				float* g = G + static_cast<size_t>(k) * out + j;
				_mm256_storeu_ps(g, _mm256_add_ps(_mm256_loadu_ps(g), a00)); _mm256_storeu_ps(g + 8, _mm256_add_ps(_mm256_loadu_ps(g + 8), a01)); g += out;
				_mm256_storeu_ps(g, _mm256_add_ps(_mm256_loadu_ps(g), a10)); _mm256_storeu_ps(g + 8, _mm256_add_ps(_mm256_loadu_ps(g + 8), a11)); g += out;
				_mm256_storeu_ps(g, _mm256_add_ps(_mm256_loadu_ps(g), a20)); _mm256_storeu_ps(g + 8, _mm256_add_ps(_mm256_loadu_ps(g + 8), a21)); g += out;
				_mm256_storeu_ps(g, _mm256_add_ps(_mm256_loadu_ps(g), a30)); _mm256_storeu_ps(g + 8, _mm256_add_ps(_mm256_loadu_ps(g + 8), a31));
			}
			for (int i = 0; i < B; i++)   // leftover columns
			{
				const float* di = D + static_cast<size_t>(i) * out;
				for (int q = 0; q < 4; q++)
				{
					const float a = X[static_cast<size_t>(i) * in + k + q];
					float* gk = G + static_cast<size_t>(k + q) * out;
					for (int j = out16; j < out; j++) gk[j] += a * di[j];
				}
			}
		}
#endif
		for (; k < in; k++)
		{
			float* gk = G + static_cast<size_t>(k) * out;
			for (int i = 0; i < B; i++)
			{
				const float a = X[static_cast<size_t>(i) * in + k];
				const float* di = D + static_cast<size_t>(i) * out;
				for (int j = 0; j < out; j++) gk[j] += a * di[j];
			}
		}
	}

	// R[i][k] = sum_j D[i][j] W[k][j]   (R: B x in, D: B x out, W: in x out)
	void BackIn(const float* D, const float* W, float* R, int B, int in, int out)
	{
		int i = 0;
#if defined(__AVX2__) && defined(__FMA__) || (defined(_MSC_VER) && defined(__AVX2__))
		const int out8 = out / 8 * 8;
		auto hsum = [](__m256 v) {
			__m128 lo = _mm256_castps256_ps128(v), hi = _mm256_extractf128_ps(v, 1);
			lo = _mm_add_ps(lo, hi);
			lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
			lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 1));
			return _mm_cvtss_f32(lo);
		};
		for (; i + 4 <= B; i += 4)
		{
			const float* d0 = D + static_cast<size_t>(i) * out;
			const float* d1 = d0 + out; const float* d2 = d1 + out; const float* d3 = d2 + out;
			for (int k = 0; k < in; k++)
			{
				const float* wk = W + static_cast<size_t>(k) * out;
				__m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0;
				for (int j = 0; j < out8; j += 8)
				{
					const __m256 w = _mm256_loadu_ps(wk + j);
					s0 = _mm256_fmadd_ps(_mm256_loadu_ps(d0 + j), w, s0);
					s1 = _mm256_fmadd_ps(_mm256_loadu_ps(d1 + j), w, s1);
					s2 = _mm256_fmadd_ps(_mm256_loadu_ps(d2 + j), w, s2);
					s3 = _mm256_fmadd_ps(_mm256_loadu_ps(d3 + j), w, s3);
				}
				float g0 = hsum(s0), g1 = hsum(s1), g2 = hsum(s2), g3 = hsum(s3);
				for (int j = out8; j < out; j++) { g0 += d0[j] * wk[j]; g1 += d1[j] * wk[j]; g2 += d2[j] * wk[j]; g3 += d3[j] * wk[j]; }
				R[static_cast<size_t>(i) * in + k] = g0; R[static_cast<size_t>(i + 1) * in + k] = g1;
				R[static_cast<size_t>(i + 2) * in + k] = g2; R[static_cast<size_t>(i + 3) * in + k] = g3;
			}
		}
#endif
		for (; i < B; i++)
			for (int k = 0; k < in; k++)
				R[static_cast<size_t>(i) * in + k] = Dot(D + static_cast<size_t>(i) * out, W + static_cast<size_t>(k) * out, out);
	}
}

const float* MLP::Forward(const float* x, int B, Work& w) const
{
	const int L = static_cast<int>(W.size());
	w.h.resize(L + 1);
	w.h[0].assign(x, x + static_cast<size_t>(B) * sizes[0]);
	for (int l = 0; l < L; l++)
	{
		const int in = sizes[l], out = sizes[l + 1];
		std::vector<float>& y = w.h[l + 1];
		y.resize(static_cast<size_t>(B) * out);
		MatMulAdd(w.h[l].data(), W[l].data(), b[l].data(), y.data(), B, in, out);
		if (l < L - 1)
		{
			float* yy = y.data();
			const size_t total = static_cast<size_t>(B) * out;
			for (size_t q = 0; q < total; q++) yy[q] = yy[q] > 0.0f ? yy[q] : kLeak * yy[q];
		}
	}
	return w.h[L].data();
}

void MLP::Backward(const float* dOut, int B, Work& w, std::vector<std::vector<float>>& gW, std::vector<std::vector<float>>& gb) const
{
	const int L = static_cast<int>(W.size());
	w.d.resize(L + 1);
	w.d[L].assign(dOut, dOut + static_cast<size_t>(B) * sizes[L]);
	for (int l = L - 1; l >= 0; l--)
	{
		const int in = sizes[l], out = sizes[l + 1];
		const float* dZ = w.d[l + 1].data();
		const float* X = w.h[l].data();
		float* gbl = gb[l].data();
		for (int i = 0; i < B; i++)
		{
			const float* dzi = dZ + static_cast<size_t>(i) * out;
			for (int j = 0; j < out; j++) gbl[j] += dzi[j];
		}
		GradW(X, dZ, gW[l].data(), B, in, out);
		if (l == 0) break;
		// Gradient into the layer below, through its leaky ReLU (its output's sign is its input's sign)
		std::vector<float>& dX = w.d[l];
		dX.resize(static_cast<size_t>(B) * in);
		BackIn(dZ, W[l].data(), dX.data(), B, in, out);
		const size_t total = static_cast<size_t>(B) * in;
		for (size_t q = 0; q < total; q++) if (!(X[q] > 0.0f)) dX[q] *= kLeak;
	}
}

void MLP::ZeroLike(std::vector<std::vector<float>>& gW, std::vector<std::vector<float>>& gb) const
{
	gW.resize(W.size()); gb.resize(b.size());
	for (size_t l = 0; l < W.size(); l++) { gW[l].assign(W[l].size(), 0.0f); gb[l].assign(b[l].size(), 0.0f); }
}

bool MLP::Write(std::FILE* f) const
{
	if (!WriteVec(f, sizes)) return false;
	for (size_t l = 0; l < W.size(); l++) if (!WriteVec(f, W[l]) || !WriteVec(f, b[l])) return false;
	return true;
}

bool MLP::Read(std::FILE* f)
{
	if (!ReadVec(f, sizes) || sizes.size() < 2) return false;
	W.assign(sizes.size() - 1, {}); b.assign(sizes.size() - 1, {});
	for (size_t l = 0; l < W.size(); l++)
	{
		if (!ReadVec(f, W[l]) || !ReadVec(f, b[l])) return false;
		if (W[l].size() != static_cast<size_t>(sizes[l]) * sizes[l + 1] || b[l].size() != static_cast<size_t>(sizes[l + 1])) return false;
	}
	return true;
}

void Adam::Init(const MLP& net)
{
	net.ZeroLike(mW, mb);
	net.ZeroLike(vW, vb);
	t = 0;
}

float Adam::Step(MLP& net, std::vector<std::vector<float>>& gW, std::vector<std::vector<float>>& gb, float maxNorm)
{
	double sq = 0;
	for (size_t l = 0; l < gW.size(); l++)
	{
		for (float g : gW[l]) sq += static_cast<double>(g) * g;
		for (float g : gb[l]) sq += static_cast<double>(g) * g;
	}
	const float norm = static_cast<float>(std::sqrt(sq));
	const float scale = (maxNorm > 0 && norm > maxNorm) ? maxNorm / norm : 1.0f;
	t++;
	const float c1 = 1.0f - std::pow(b1, static_cast<float>(t)), c2 = 1.0f - std::pow(b2, static_cast<float>(t));
	const float step = lr * std::sqrt(c2) / c1;
	auto upd = [&](std::vector<float>& p, std::vector<float>& g, std::vector<float>& m, std::vector<float>& v) {
		const size_t n = p.size();
		for (size_t i = 0; i < n; i++)
		{
			const float gi = g[i] * scale;
			m[i] = b1 * m[i] + (1.0f - b1) * gi;
			v[i] = b2 * v[i] + (1.0f - b2) * gi * gi;
			p[i] -= step * m[i] / (std::sqrt(v[i]) + eps);
		}
	};
	for (size_t l = 0; l < net.W.size(); l++) { upd(net.W[l], gW[l], mW[l], vW[l]); upd(net.b[l], gb[l], mb[l], vb[l]); }
	return norm;
}

bool Adam::Write(std::FILE* f) const
{
	if (std::fwrite(&t, sizeof(t), 1, f) != 1) return false;
	for (size_t l = 0; l < mW.size(); l++)
		if (!WriteVec(f, mW[l]) || !WriteVec(f, vW[l]) || !WriteVec(f, mb[l]) || !WriteVec(f, vb[l])) return false;
	return true;
}

bool Adam::Read(std::FILE* f)
{
	if (std::fread(&t, sizeof(t), 1, f) != 1) return false;
	for (size_t l = 0; l < mW.size(); l++)
		if (!ReadVec(f, mW[l]) || !ReadVec(f, vW[l]) || !ReadVec(f, mb[l]) || !ReadVec(f, vb[l])) return false;
	return true;
}

void ObsNorm::Add(const float* rows, int B)
{
	// Chan et al. parallel update: merge this batch's mean / variance into the running ones
	if (B <= 0) return;
	for (int k = 0; k < n; k++)
	{
		double bm = 0;
		for (int i = 0; i < B; i++) bm += rows[static_cast<size_t>(i) * n + k];
		bm /= B;
		double bv = 0;
		for (int i = 0; i < B; i++) { double d = rows[static_cast<size_t>(i) * n + k] - bm; bv += d * d; }
		const double tot = count + B;
		const double delta = bm - mean[k];
		mean[k] += delta * B / tot;
		m2[k] += bv + delta * delta * count * B / tot;
	}
	count += B;
}

void ObsNorm::Apply(float* row) const
{
	for (int k = 0; k < n; k++)
	{
		const double var = m2[k] / std::max(1.0, count);
		const double z = (row[k] - mean[k]) / std::sqrt(var + 1e-8);
		row[k] = static_cast<float>(std::clamp(z, -5.0, 5.0));
	}
}

bool ObsNorm::Write(std::FILE* f) const
{
	return std::fwrite(&n, sizeof(n), 1, f) == 1 && std::fwrite(&count, sizeof(count), 1, f) == 1 && WriteVec(f, mean) && WriteVec(f, m2);
}

bool ObsNorm::Read(std::FILE* f)
{
	return std::fread(&n, sizeof(n), 1, f) == 1 && std::fread(&count, sizeof(count), 1, f) == 1 && ReadVec(f, mean) && ReadVec(f, m2)
		&& static_cast<int>(mean.size()) == n && static_cast<int>(m2.size()) == n;
}
