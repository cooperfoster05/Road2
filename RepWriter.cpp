#include "pch.h"
#include "RepWriter.h"
#include "RepStore.h"

#include <ctime>
#include <fstream>
#include <format>

namespace
{
	std::string NowStamp(const char* fmt)
	{
		std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char buf[64];
		std::strftime(buf, sizeof(buf), fmt, &local);
		return buf;
	}

	// Opens a CSV for appending and writes the header if the file is new
	std::ofstream OpenAppend(const std::filesystem::path& path, const char* header)
	{
		bool isNew = !std::filesystem::exists(path);
		std::ofstream out(path, std::ios::app);
		if (out && isNew) out << header << "\n";
		return out;
	}

	std::string FramesToCsv(const std::vector<Frame>& frames)
	{
		std::string out;
		out.reserve(frames.size() * 260 + 512);
		out += "tick,t,wall_ms,"
			"throttle,steer,pitch,yaw,roll,jump,boost,handbrake,"
			"car_x,car_y,car_z,car_pitch,car_yaw,car_roll,car_vx,car_vy,car_vz,car_avx,car_avy,car_avz,"
			"boost_amount,on_ground,on_wall,has_flip,"
			"ball_x,ball_y,ball_z,ball_vx,ball_vy,ball_vz,ball_avx,ball_avy,ball_avz,"
			"touch,touch_x,touch_y,touch_z\n";
		for (const Frame& f : frames)
		{
			out += std::format("{},{:.4f},{:.1f},", f.tick, f.t, f.wallMs);
			out += std::format("{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},",
				f.throttle, f.steer, f.pitch, f.yaw, f.roll, (int)f.jump, (int)f.boost, (int)f.handbrake);
			out += std::format("{:.2f},{:.2f},{:.2f},{},{},{},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},",
				f.carX, f.carY, f.carZ, f.carPitch, f.carYaw, f.carRoll,
				f.carVX, f.carVY, f.carVZ, f.carAVX, f.carAVY, f.carAVZ);
			out += std::format("{:.1f},{},{},{},", f.boostAmount, (int)f.onGround, (int)f.onWall, (int)f.hasFlip);
			out += std::format("{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},",
				f.ballX, f.ballY, f.ballZ, f.ballVX, f.ballVY, f.ballVZ, f.ballAVX, f.ballAVY, f.ballAVZ);
			out += std::format("{},{:.2f},{:.2f},{:.2f}\n", (int)f.touch, f.touchX, f.touchY, f.touchZ);
		}
		return out;
	}
}

RepWriter::RepWriter(std::filesystem::path rootFolder) : root(std::move(rootFolder))
{
	std::error_code ec;
	std::filesystem::create_directories(root / "sessions", ec);
	bundle = root / "sessions" / ("session_" + NowStamp("%Y-%m-%d_%H-%M-%S") + ".rlc");
	worker = std::thread([this]() { Run(); });
}

RepWriter::~RepWriter()
{
	Shutdown();
}

void RepWriter::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(qmtx);
		stopping = true;
	}
	cv.notify_all();
	if (worker.joinable()) worker.join();
}

bool RepWriter::Busy()
{
	std::lock_guard<std::mutex> lock(qmtx);
	return working || !queue.empty();
}

void RepWriter::Enqueue(std::function<void()> job)
{
	{
		std::lock_guard<std::mutex> lock(qmtx);
		if (stopping) { job(); return; }   // shutting down: just do it here
		queue.push_back(std::move(job));
	}
	cv.notify_one();
}

void RepWriter::Run()
{
	for (;;)
	{
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(qmtx);
			cv.wait(lock, [this]() { return stopping || !queue.empty(); });
			if (queue.empty()) return;   // stopping, and everything is written
			job = std::move(queue.front());
			queue.pop_front();
			working = true;
		}
		job();
		std::lock_guard<std::mutex> lock(qmtx);
		working = false;
	}
}

bool RepWriter::SaveRep(const std::vector<Frame>& frames, RepSummary& summary)
{
	summary.savedAt = NowStamp("%Y-%m-%d %H:%M:%S");
	summary.fileName = std::format("rep_{}_{:03}.csv", NowStamp("%Y-%m-%d_%H-%M-%S"), ++counter);

	RepSummary s = summary;
	bool keepRecording = s.endReason != "reset";
	std::vector<Frame> copy = keepRecording ? frames : std::vector<Frame>{};
	Enqueue([this, s, copy = std::move(copy), keepRecording]() {
		if (keepRecording)
		{
			std::string err;
			if (!RepStore::Append(root, bundle, s.fileName, FramesToCsv(copy), err)) failed++;
		}
		std::ofstream index = OpenAppend(root / "reps_index.csv",
			"file,saved_at,mode,map,end_reason,duration_s,ticks,touches,goal,event_tick,floor_before_goal,shot_key");
		if (!index) { failed++; return; }
		index << std::format("{},{},{},{},{},{:.3f},{},{},{},{},{},{}\n",
			s.fileName, s.savedAt, s.mode, s.map, s.endReason,
			s.durationS, s.ticks, s.touches, (int)s.goal,
			s.eventTick, (int)s.floorBeforeGoal, s.shotKey);
		if (!index) failed++;
	});
	return true;
}

bool RepWriter::SaveRating(const std::string& repFile, int rating)
{
	std::string at = NowStamp("%Y-%m-%d %H:%M:%S");
	Enqueue([this, repFile, rating, at]() {
		std::ofstream out = OpenAppend(root / "ratings.csv", "file,rating,rated_at");
		if (out) out << std::format("{},{},{}\n", repFile, rating, at);
		if (!out) failed++;
	});
	return true;
}

void RepWriter::SetKept(const std::string& repFile, bool kept)
{
	std::string at = NowStamp("%Y-%m-%d %H:%M:%S");
	Enqueue([this, repFile, kept, at]() {
		std::ofstream out = OpenAppend(root / "kept.csv", "file,kept,at");
		if (out) out << std::format("{},{},{}\n", repFile, kept ? 1 : 0, at);
		if (!out) failed++;
	});
}
