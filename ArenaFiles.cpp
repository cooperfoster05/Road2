// No pch on purpose: plain C++ and miniz only.
#include "ArenaFiles.h"
#include "third_party/miniz/miniz.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	// RocketSim's own list of the standard arena's 16 meshes (RocketSim.cpp, MeshHashSet). A mesh that hashes to one
	// of these is the real thing. They're in the order RLArenaCollisionDumper numbers its files (mesh_0 to mesh_15),
	// so the files written here match the dumper's by name as well as by content.
	constexpr uint32_t kKnown[16] = {
		0xA160BAF9, 0x2811EEE8, 0x14B84668, 0xEC759EBF,
		0x94FB0D5C, 0xDEA07102, 0xBD4FBEA8, 0x39A47F63,
		0x3D79D25D, 0xD84C7A68, 0xB81AC8B9, 0x760358D3,
		0x73AE4940, 0x918F4A4E, 0x1F8EE550, 0x255BA8C1
	};
	constexpr uint32_t kPackageTag = 0x9E2A83C1;   // starts an Unreal package, and each compressed chunk in it
	constexpr size_t kMaxUnpacked = 512u * 1024u * 1024u;

	uint32_t U32(const std::vector<uint8_t>& b, size_t at) { uint32_t v; std::memcpy(&v, b.data() + at, 4); return v; }

	bool ReadFile(const fs::path& p, std::vector<uint8_t>& out)
	{
		std::error_code ec;
		const uintmax_t size = fs::file_size(p, ec);
		if (ec || size < 64 || size > 1024u * 1024u * 1024u) return false;
		std::ifstream in(p, std::ios::binary);
		if (!in) return false;
		out.resize(static_cast<size_t>(size));
		return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)));
	}

	// The package's data, unpacked. After the header the file is a run of compressed chunks, one after another; each
	// is: tag, block size, packed size, unpacked size, a table of (packed, unpacked) per block, then the zlib blocks.
	// The header (the part that's encrypted) is never needed.
	bool Unpack(const std::vector<uint8_t>& file, std::vector<uint8_t>& out)
	{
		out.clear();
		if (file.size() < 16 || U32(file, 0) != kPackageTag) return false;
		size_t p = U32(file, 8);   // the header's size: where the first chunk starts
		while (p + 16 <= file.size() && U32(file, p) == kPackageTag)
		{
			const uint32_t blockSize = U32(file, p + 4), unpacked = U32(file, p + 12);
			if (blockSize == 0 || blockSize > 16u * 1024u * 1024u || unpacked == 0 || unpacked > kMaxUnpacked) return false;
			const size_t blocks = (static_cast<size_t>(unpacked) + blockSize - 1) / blockSize;
			size_t table = p + 16, data = table + blocks * 8;
			if (data > file.size()) return false;
			for (size_t i = 0; i < blocks; i++)
			{
				const uint32_t cz = U32(file, table + i * 8), uz = U32(file, table + i * 8 + 4);
				if (uz == 0 || uz > blockSize || data + cz > file.size() || out.size() + uz > kMaxUnpacked) return false;
				const size_t at = out.size();
				out.resize(at + uz);
				mz_ulong len = uz;
				if (mz_uncompress(out.data() + at, &len, file.data() + data, cz) != MZ_OK || len != uz) return false;
				data += cz;
			}
			p = data;
		}
		return !out.empty();
	}

	struct Mesh
	{
		std::vector<float> verts;   // x, y, z per vertex
		std::vector<int32_t> tris;  // three vertex numbers per triangle
	};

	// The same hash RocketSim takes of a mesh file (CollisionMeshFile::UpdateHash)
	uint32_t Hash(const Mesh& m)
	{
		const uint32_t nv = static_cast<uint32_t>(m.verts.size() / 3), nt = static_cast<uint32_t>(m.tris.size() / 3);
		uint32_t hash = nv + nt * nv;
		for (int32_t idx : m.tris)
			for (int j = 0; j < 3; j++)
			{
				uint32_t cur = static_cast<uint32_t>(static_cast<int64_t>(m.verts[static_cast<size_t>(idx) * 3 + j]));
				for (int k = 0; k < 2; k++) cur = ((cur >> 16) ^ cur) * 0x45D9F3Bu;
				cur = (cur >> 16) ^ cur;
				hash ^= cur + 0x9E3779B9u + (hash << 6) + (hash >> 2);
			}
		return hash;
	}

	int KnownIndex(uint32_t hash)
	{
		for (int i = 0; i < 16; i++) if (kKnown[i] == hash) return i;
		return -1;
	}

	// One cooked physics mesh at `at` (just past its "NXS\1MESH" mark): version, flags, a float, two numbers, the
	// vertex and triangle counts, the vertices, then the triangles as 8, 16 or 32-bit vertex numbers (by the flags)
	bool ReadMesh(const std::vector<uint8_t>& d, size_t at, Mesh& m)
	{
		if (at + 28 > d.size()) return false;
		const uint32_t flags = U32(d, at + 4), nv = U32(d, at + 20), nt = U32(d, at + 24);
		if (nv < 3 || nt < 1 || nv > 200000 || nt > 400000) return false;
		const size_t width = (flags & 8) ? 1 : (flags & 16) ? 2 : 4;
		size_t p = at + 28;
		if (p + static_cast<size_t>(nv) * 12 + static_cast<size_t>(nt) * 3 * width > d.size()) return false;
		m.verts.resize(static_cast<size_t>(nv) * 3);
		std::memcpy(m.verts.data(), d.data() + p, m.verts.size() * 4);
		p += m.verts.size() * 4;
		m.tris.resize(static_cast<size_t>(nt) * 3);
		for (size_t i = 0; i < m.tris.size(); i++, p += width)
		{
			uint32_t v = 0;
			std::memcpy(&v, d.data() + p, width);
			if (v >= nv) return false;
			m.tris[i] = static_cast<int32_t>(v);
		}
		return true;
	}

	// RocketSim's mesh file: triangle count, vertex count, the triangles, the vertices
	bool WriteCmf(const fs::path& file, const Mesh& m)
	{
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		const int32_t nt = static_cast<int32_t>(m.tris.size() / 3), nv = static_cast<int32_t>(m.verts.size() / 3);
		out.write(reinterpret_cast<const char*>(&nt), 4);
		out.write(reinterpret_cast<const char*>(&nv), 4);
		out.write(reinterpret_cast<const char*>(m.tris.data()), static_cast<std::streamsize>(m.tris.size() * 4));
		out.write(reinterpret_cast<const char*>(m.verts.data()), static_cast<std::streamsize>(m.verts.size() * 4));
		out.flush();
		return static_cast<bool>(out);
	}

	// Every one of the 16 that this stadium file holds. The side and corner pieces are stored where they sit; the
	// four goal pieces are stored at the middle of the pitch and moved to their goal line by the game, so each mesh
	// is also tried moved up and down the pitch by that much (the hash says which, if any, is real).
	int Collect(const std::vector<uint8_t>& data, Mesh found[16], bool have[16])
	{
		static const uint8_t kMark[8] = { 'N', 'X', 'S', 1, 'M', 'E', 'S', 'H' };
		const float kGoalY = 102.39998626708984f;   // the goal line (5120) in the physics engine's units, as the game has it
		int got = 0;
		auto it = data.begin();
		while ((it = std::search(it, data.end(), std::begin(kMark), std::end(kMark))) != data.end())
		{
			const size_t at = static_cast<size_t>(it - data.begin()) + 8;
			it += 8;
			Mesh m;
			if (!ReadMesh(data, at, m)) continue;
			for (float dy : { 0.0f, kGoalY, -kGoalY })
			{
				Mesh t = m;
				// moved the way the game moves it: all three axes get an add (0 on x and z), which also turns a -0 into 0,
				// so the files come out byte for byte what the dumper wrote
				if (dy != 0.0f) for (size_t i = 0; i < t.verts.size(); i += 3) { t.verts[i] += 0.0f; t.verts[i + 1] += dy; t.verts[i + 2] += 0.0f; }
				const int k = KnownIndex(Hash(t));
				if (k < 0 || have[k]) continue;
				found[k] = std::move(t);
				have[k] = true;
				got++;
				break;
			}
		}
		return got;
	}
}

bool ArenaFiles::FromGameFiles(const fs::path& cookedFolder, const fs::path& meshesFolder, std::string& message)
{
	std::error_code ec;
	if (cookedFolder.empty() || !fs::is_directory(cookedFolder, ec))
	{
		message = "Couldn't find Rocket League's files (TAGame\\CookedPCConsole)";
		return false;
	}
	// Any standard stadium holds the same 16; these are tried in turn in case one is missing or changes
	static const char* kStadiums[] = { "EuroStadium_Night_P.upk", "Stadium_P.upk", "EuroStadium_P.upk", "Park_P.upk", "TrainStation_P.upk", "UtopiaStadium_P.upk" };
	Mesh found[16];
	bool have[16] = {};
	int total = 0, opened = 0;
	try
	{
		for (const char* name : kStadiums)
		{
			std::vector<uint8_t> file, data;
			if (!ReadFile(cookedFolder / name, file)) continue;
			opened++;
			if (!Unpack(file, data)) continue;
			total += Collect(data, found, have);
			if (total == 16) break;
		}
	}
	catch (const std::exception&)
	{
		message = "Ran out of memory reading the game's stadium file";
		return false;
	}
	if (total < 16)
	{
		message = opened == 0 ? "Couldn't open a stadium file in Rocket League's CookedPCConsole folder"
			: std::format("Found {} of the arena's 16 pieces in the game's files (a game update may have changed them)", total);
		return false;
	}
	const fs::path out = meshesFolder / "soccar";
	fs::create_directories(out, ec);
	for (int i = 0; i < 16; i++)
		if (!WriteCmf(out / std::format("mesh_{}.cmf", i), found[i]))
		{
			message = "Couldn't write the arena files to " + out.string();
			return false;
		}
	message = "Read the arena from your Rocket League files";
	return true;
}
