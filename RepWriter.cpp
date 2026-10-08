#include "pch.h"
#include "RepWriter.h"

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
}

RepWriter::RepWriter(std::filesystem::path rootFolder) : root(std::move(rootFolder))
{
	std::error_code ec;
	std::filesystem::create_directories(root / "reps", ec);
}

bool RepWriter::SaveRep(const std::vector<Frame>& frames, RepSummary& summary)
{
	summary.savedAt = NowStamp("%Y-%m-%d %H:%M:%S");
	summary.fileName = std::format("rep_{}_{:03}.csv", NowStamp("%Y-%m-%d_%H-%M-%S"), ++counter);

	std::ofstream out(root / "reps" / summary.fileName);
	if (!out) return false;

	out << "tick,t,wall_ms,"
		"throttle,steer,pitch,yaw,roll,jump,boost,handbrake,"
		"car_x,car_y,car_z,car_pitch,car_yaw,car_roll,car_vx,car_vy,car_vz,car_avx,car_avy,car_avz,"
		"boost_amount,on_ground,on_wall,has_flip,"
		"ball_x,ball_y,ball_z,ball_vx,ball_vy,ball_vz,ball_avx,ball_avy,ball_avz,"
		"touch,touch_x,touch_y,touch_z\n";

	for (const Frame& f : frames)
	{
		out << std::format("{},{:.4f},{:.1f},", f.tick, f.t, f.wallMs)
			<< std::format("{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},",
				f.throttle, f.steer, f.pitch, f.yaw, f.roll, (int)f.jump, (int)f.boost, (int)f.handbrake)
			<< std::format("{:.2f},{:.2f},{:.2f},{},{},{},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},",
				f.carX, f.carY, f.carZ, f.carPitch, f.carYaw, f.carRoll,
				f.carVX, f.carVY, f.carVZ, f.carAVX, f.carAVY, f.carAVZ)
			<< std::format("{:.1f},{},{},{},", f.boostAmount, (int)f.onGround, (int)f.onWall, (int)f.hasFlip)
			<< std::format("{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.4f},{:.4f},{:.4f},",
				f.ballX, f.ballY, f.ballZ, f.ballVX, f.ballVY, f.ballVZ, f.ballAVX, f.ballAVY, f.ballAVZ)
			<< std::format("{},{:.2f},{:.2f},{:.2f}\n", (int)f.touch, f.touchX, f.touchY, f.touchZ);
	}
	if (!out) return false;
	out.close();

	std::ofstream index = OpenAppend(root / "reps_index.csv",
		"file,saved_at,mode,map,end_reason,duration_s,ticks,touches,goal,event_tick,floor_before_goal,shot_key");
	if (!index) return false;
	index << std::format("{},{},{},{},{},{:.3f},{},{},{},{},{},{}\n",
		summary.fileName, summary.savedAt, summary.mode, summary.map, summary.endReason,
		summary.durationS, summary.ticks, summary.touches, (int)summary.goal,
		summary.eventTick, (int)summary.floorBeforeGoal, summary.shotKey);
	return static_cast<bool>(index);
}

bool RepWriter::SaveRating(const std::string& repFile, int rating)
{
	std::ofstream out = OpenAppend(root / "ratings.csv", "file,rating,rated_at");
	if (!out) return false;
	out << std::format("{},{},{}\n", repFile, rating, NowStamp("%Y-%m-%d %H:%M:%S"));
	return static_cast<bool>(out);
}
