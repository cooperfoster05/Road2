#pragma once
// Speed units for everything the plugin shows (rlcoach_speed_units). Plain C++.

#include <atomic>
#include <format>
#include <string>

namespace Units
{
	enum SpeedUnit { UU = 0, KMH = 1, MPH = 2 };

	inline std::atomic<int>& SpeedMode() { static std::atomic<int> mode{ UU }; return mode; }

	// Game units are centimeters, so uu/s * 0.036 = km/h
	inline float ConvertSpeed(float uuPerSec)
	{
		switch (SpeedMode().load())
		{
		case KMH: return uuPerSec * 0.036f;
		case MPH: return uuPerSec * 0.0223694f;
		default:  return uuPerSec;
		}
	}

	inline const char* SpeedLabel()
	{
		switch (SpeedMode().load())
		{
		case KMH: return "km/h";
		case MPH: return "mph";
		default:  return "uu/s";
		}
	}

	inline std::string Speed(float uuPerSec) { return std::format("{:.0f} {}", ConvertSpeed(uuPerSec), SpeedLabel()); }
}
