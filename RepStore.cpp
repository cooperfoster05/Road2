#include "pch.h"
#include "RepStore.h"
#include "third_party/miniz/miniz.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
	constexpr char kMagic[4] = { 'R', 'L', 'C', '1' };

	struct Loc
	{
		fs::path file;
		bool legacy = false;
		uint64_t offset = 0;     // where the packed bytes start
		uint32_t rawLen = 0;
		uint32_t packLen = 0;
	};

	struct State
	{
		std::mutex mtx;
		fs::path root;
		bool scanned = false;
		std::unordered_map<std::string, Loc> locs;
		std::map<std::string, uint64_t> validEnd;   // bundle path -> end of its last complete chunk
	};

	State& S() { static State s; return s; }

	void PutU16(std::string& b, uint16_t v) { b.push_back(char(v & 0xFF)); b.push_back(char(v >> 8)); }
	void PutU32(std::string& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(char((v >> (8 * i)) & 0xFF)); }
	uint32_t GetU(const unsigned char* p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= uint32_t(p[i]) << (8 * i); return v; }

	struct Chunk { std::string name; uint64_t headerAt = 0; uint64_t offset = 0; uint32_t rawLen = 0, packLen = 0; };

	// Reads a bundle's chunk headers; stops at the first broken or cut-off chunk
	std::vector<Chunk> ScanBundle(const fs::path& file, uint64_t& validEnd)
	{
		std::vector<Chunk> out;
		validEnd = 0;
		std::error_code ec;
		uint64_t size = fs::file_size(file, ec);
		if (ec) return out;
		std::ifstream in(file, std::ios::binary);
		uint64_t pos = 0;
		while (in && pos + 10 <= size)
		{
			unsigned char hdr[6];
			in.seekg(static_cast<std::streamoff>(pos));
			if (!in.read(reinterpret_cast<char*>(hdr), 6)) break;
			if (std::memcmp(hdr, kMagic, 4) != 0) break;
			uint32_t nameLen = GetU(hdr + 4, 2);
			std::string name(nameLen, '\0');
			if (nameLen && !in.read(name.data(), nameLen)) break;
			unsigned char lens[8];
			if (!in.read(reinterpret_cast<char*>(lens), 8)) break;
			Chunk c;
			c.name = name;
			c.headerAt = pos;
			c.rawLen = GetU(lens, 4);
			c.packLen = GetU(lens + 4, 4);
			c.offset = pos + 6 + nameLen + 8;
			if (c.offset + c.packLen > size) break;   // cut off by a crash
			out.push_back(c);
			pos = c.offset + c.packLen;
			validEnd = pos;
		}
		return out;
	}

	bool IsBundle(const fs::path& p) { return p.extension() == ".rlc"; }

	// Caller holds the lock
	void ScanAll(const fs::path& root)
	{
		State& s = S();
		if (s.scanned && s.root == root) return;
		s.locs.clear();
		s.validEnd.clear();
		s.root = root;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(root / "reps", ec))
		{
			if (!e.is_regular_file() || e.path().extension() != ".csv") continue;
			Loc l;
			l.file = e.path();
			l.legacy = true;
			l.packLen = static_cast<uint32_t>(e.file_size(ec));
			s.locs[e.path().filename().string()] = l;
		}
		for (const auto& e : fs::directory_iterator(root / "sessions", ec))
		{
			if (!e.is_regular_file() || !IsBundle(e.path())) continue;
			uint64_t end = 0;
			for (const Chunk& c : ScanBundle(e.path(), end))
				s.locs[c.name] = Loc{ e.path(), false, c.offset, c.rawLen, c.packLen };   // a bundle copy wins over a loose file
			s.validEnd[e.path().string()] = end;
		}
		s.scanned = true;
	}

	bool ReadLoose(const fs::path& file, std::string& out)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in) return false;
		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return true;
	}

	bool ReadPacked(const Loc& l, std::string& packed)
	{
		std::ifstream in(l.file, std::ios::binary);
		if (!in) return false;
		packed.resize(l.packLen);
		in.seekg(static_cast<std::streamoff>(l.offset));
		return static_cast<bool>(in.read(packed.data(), l.packLen));
	}

	bool Unpack(const std::string& packed, uint32_t rawLen, std::string& out)
	{
		out.resize(rawLen);
		mz_ulong len = rawLen;
		int rc = mz_uncompress(reinterpret_cast<unsigned char*>(out.data()), &len,
			reinterpret_cast<const unsigned char*>(packed.data()), static_cast<mz_ulong>(packed.size()));
		if (rc != MZ_OK || len != rawLen) return false;
		return true;
	}

	std::string ChunkBytes(const std::string& name, uint32_t rawLen, const std::string& packed)
	{
		std::string b;
		b.reserve(packed.size() + name.size() + 14);
		b.append(kMagic, 4);
		PutU16(b, static_cast<uint16_t>(name.size()));
		b += name;
		PutU32(b, rawLen);
		PutU32(b, static_cast<uint32_t>(packed.size()));
		b += packed;
		return b;
	}
}

bool RepStore::Read(const fs::path& root, const std::string& name, std::string& csv, std::string& err)
{
	State& s = S();
	std::lock_guard<std::mutex> lock(s.mtx);
	ScanAll(root);
	auto it = s.locs.find(name);
	if (it == s.locs.end())
	{
		// Not seen in the last scan: maybe an old loose file
		if (ReadLoose(root / "reps" / name, csv)) return true;
		err = "Recording not found (it may have been cleaned up)";
		return false;
	}
	const Loc& l = it->second;
	if (l.legacy)
	{
		if (ReadLoose(l.file, csv)) return true;
		err = "Couldn't open " + name;
		return false;
	}
	std::string packed;
	if (!ReadPacked(l, packed)) { err = "Couldn't read " + l.file.filename().string(); return false; }
	if (!Unpack(packed, l.rawLen, csv)) { err = "Recording is damaged"; return false; }
	return true;
}

bool RepStore::Has(const fs::path& root, const std::string& name)
{
	State& s = S();
	std::lock_guard<std::mutex> lock(s.mtx);
	ScanAll(root);
	return s.locs.count(name) > 0;
}

bool RepStore::Append(const fs::path& root, const fs::path& bundle, const std::string& name, const std::string& csv, std::string& err)
{
	// Pack outside the lock (the slow part)
	mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(csv.size()));
	std::string packed(bound, '\0');
	mz_ulong packLen = bound;
	if (mz_compress2(reinterpret_cast<unsigned char*>(packed.data()), &packLen,
		reinterpret_cast<const unsigned char*>(csv.data()), static_cast<mz_ulong>(csv.size()), 9) != MZ_OK)
	{
		err = "Compression failed";
		return false;
	}
	packed.resize(packLen);
	std::string bytes = ChunkBytes(name, static_cast<uint32_t>(csv.size()), packed);

	State& s = S();
	std::lock_guard<std::mutex> lock(s.mtx);
	ScanAll(root);
	std::error_code ec;
	fs::create_directories(bundle.parent_path(), ec);

	// If a crash left half a chunk at the end, cut it off so new chunks stay readable
	uint64_t size = fs::exists(bundle, ec) ? fs::file_size(bundle, ec) : 0;
	auto ve = s.validEnd.find(bundle.string());
	uint64_t end = 0;
	if (ve != s.validEnd.end()) end = ve->second;
	else if (size > 0) ScanBundle(bundle, end);
	if (size != end) { fs::resize_file(bundle, end, ec); size = end; }

	std::ofstream out(bundle, std::ios::binary | std::ios::app);
	if (!out) { err = "Couldn't open " + bundle.filename().string(); return false; }
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	out.flush();
	if (!out) { err = "Couldn't write " + bundle.filename().string(); return false; }
	out.close();

	uint64_t headerLen = bytes.size() - packed.size();
	s.locs[name] = Loc{ bundle, false, size + headerLen, static_cast<uint32_t>(csv.size()), static_cast<uint32_t>(packed.size()) };
	s.validEnd[bundle.string()] = size + bytes.size();
	return true;
}

std::vector<RepStore::RecInfo> RepStore::List(const fs::path& root)
{
	State& s = S();
	std::lock_guard<std::mutex> lock(s.mtx);
	ScanAll(root);
	std::vector<RecInfo> out;
	out.reserve(s.locs.size());
	for (const auto& [name, l] : s.locs)
		out.push_back(RecInfo{ name, l.file, l.legacy ? l.packLen : uint64_t(l.packLen) + name.size() + 14, l.legacy });
	return out;
}

uint64_t RepStore::TotalBytes(const fs::path& root)
{
	uint64_t total = 0;
	for (const RecInfo& r : List(root)) total += r.bytes;
	return total;
}

int RepStore::Remove(const fs::path& root, const std::unordered_set<std::string>& names, const fs::path& skipBundle)
{
	if (names.empty()) return 0;
	State& s = S();
	std::map<std::string, fs::path> bundles;   // bundles holding any of the names
	std::vector<fs::path> loose;
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		ScanAll(root);
		for (const std::string& n : names)
		{
			auto it = s.locs.find(n);
			if (it == s.locs.end()) continue;
			if (it->second.legacy) loose.push_back(it->second.file);
			else if (it->second.file != skipBundle) bundles[it->second.file.string()] = it->second.file;
		}
	}

	int removed = 0;
	std::error_code ec;
	for (const fs::path& f : loose)
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		if (fs::remove(f, ec))
		{
			s.locs.erase(f.filename().string());
			removed++;
		}
	}

	for (const auto& [key, bundle] : bundles)
	{
		// Build the slimmer copy without holding the lock (only the session being recorded is ever appended to)
		uint64_t end = 0;
		std::vector<Chunk> chunks = ScanBundle(bundle, end);
		std::vector<Chunk> keep;
		int dropped = 0;
		for (const Chunk& c : chunks)
		{
			if (names.count(c.name)) dropped++;
			else keep.push_back(c);
		}
		if (dropped == 0) continue;

		fs::path tmp = bundle;
		tmp += ".tmp";
		std::vector<std::pair<std::string, Loc>> newLocs;
		bool ok = true;
		if (!keep.empty())
		{
			std::ifstream in(bundle, std::ios::binary);
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			uint64_t pos = 0;
			for (const Chunk& c : keep)
			{
				uint64_t len = c.offset + c.packLen - c.headerAt;
				std::string buf(len, '\0');
				in.seekg(static_cast<std::streamoff>(c.headerAt));
				if (!in.read(buf.data(), static_cast<std::streamsize>(len))) { ok = false; break; }
				out.write(buf.data(), static_cast<std::streamsize>(len));
				newLocs.push_back({ c.name, Loc{ bundle, false, pos + (c.offset - c.headerAt), c.rawLen, c.packLen } });
				pos += len;
			}
			out.flush();
			if (!out) ok = false;
		}
		if (!ok) { fs::remove(tmp, ec); continue; }

		std::lock_guard<std::mutex> lock(s.mtx);
		if (keep.empty()) fs::remove(bundle, ec);
		else fs::rename(tmp, bundle, ec);
		if (ec) { fs::remove(tmp, ec); continue; }   // in use: try again next time

		for (const Chunk& c : chunks)
		{
			auto it = s.locs.find(c.name);
			if (it != s.locs.end() && it->second.file == bundle) s.locs.erase(it);
		}
		uint64_t newEnd = 0;
		for (auto& [n, l] : newLocs) { s.locs[n] = l; newEnd = l.offset + l.packLen; }
		if (keep.empty()) s.validEnd.erase(bundle.string());
		else s.validEnd[bundle.string()] = newEnd;
		removed += dropped;
	}
	return removed;
}

int RepStore::MigrateLegacy(const fs::path& root, const std::unordered_set<std::string>& drop, const std::atomic<bool>* stop)
{
	std::vector<fs::path> loose;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(root / "reps", ec))
		if (e.is_regular_file() && e.path().extension() == ".csv") loose.push_back(e.path());
	std::sort(loose.begin(), loose.end());

	State& s = S();
	int handled = 0;
	for (const fs::path& file : loose)
	{
		if (stop && *stop) break;
		std::string name = file.filename().string();
		bool inBundle = false;
		{
			std::lock_guard<std::mutex> lock(s.mtx);
			ScanAll(root);
			auto it = s.locs.find(name);
			inBundle = it != s.locs.end() && !it->second.legacy;
		}
		if (!inBundle && !drop.count(name))
		{
			// rep_YYYY-MM-DD_... -> one bundle per day
			std::string day = name.size() >= 14 ? name.substr(4, 10) : std::string("unknown");
			fs::path bundle = root / "sessions" / ("session_" + day + "_imported.rlc");
			std::string csv, err;
			if (!ReadLoose(file, csv)) continue;
			if (!Append(root, bundle, name, csv, err)) continue;
		}
		std::lock_guard<std::mutex> lock(s.mtx);
		if (fs::remove(file, ec))
		{
			// Append already pointed this name at its bundle copy; forget it if there's no copy
			auto it = s.locs.find(name);
			if (it != s.locs.end() && it->second.legacy) s.locs.erase(it);
			handled++;
		}
	}
	return handled;
}

bool RepStore::WriteBundle(const fs::path& file, const std::vector<std::pair<std::string, std::string>>& namedCsv, std::string& err)
{
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) { err = "Couldn't create " + file.filename().string(); return false; }
	for (const auto& [name, csv] : namedCsv)
	{
		mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(csv.size()));
		std::string packed(bound, '\0');
		mz_ulong packLen = bound;
		if (mz_compress2(reinterpret_cast<unsigned char*>(packed.data()), &packLen,
			reinterpret_cast<const unsigned char*>(csv.data()), static_cast<mz_ulong>(csv.size()), 9) != MZ_OK)
		{
			err = "Compression failed";
			return false;
		}
		packed.resize(packLen);
		std::string bytes = ChunkBytes(name, static_cast<uint32_t>(csv.size()), packed);
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	}
	out.flush();
	if (!out) { err = "Couldn't write " + file.filename().string(); return false; }
	return true;
}

bool RepStore::ReadBundle(const fs::path& file, std::vector<std::pair<std::string, std::string>>& namedCsv, std::string& err)
{
	namedCsv.clear();
	uint64_t end = 0;
	std::vector<Chunk> chunks = ScanBundle(file, end);
	if (chunks.empty()) { err = "No reps in " + file.filename().string(); return false; }
	for (const Chunk& c : chunks)
	{
		Loc l{ file, false, c.offset, c.rawLen, c.packLen };
		std::string packed, csv;
		if (!ReadPacked(l, packed) || !Unpack(packed, c.rawLen, csv)) { err = "Damaged chunk " + c.name; return false; }
		namedCsv.emplace_back(c.name, std::move(csv));
	}
	return true;
}
