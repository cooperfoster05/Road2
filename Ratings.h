#pragma once
// Names and colors for 1-5 grades, shared by the panel, the rep list and the settings.

inline void OverallRgb(int o, unsigned char& r, unsigned char& g, unsigned char& b)
{
	// The rep viewer's grade colors (Oct 24): 5 cyan, 4 green, 3 amber, 2 orange, 1 red
	if (o >= 5) { r = 34; g = 211; b = 238; }
	else if (o == 4) { r = 74; g = 222; b = 128; }
	else if (o == 3) { r = 251; g = 191; b = 36; }
	else if (o == 2) { r = 251; g = 146; b = 60; }
	else if (o == 1) { r = 248; g = 113; b = 113; }
	else { r = 141; g = 160; b = 188; }
}

// Your opinion of the recovery after a rep (keys 6-8): kept separate from the 1-5 shot rating
inline const char* RecoveryName(int r)
{
	switch (r)
	{
	case 1: return "Poor";
	case 2: return "Eh";
	case 3: return "Good";
	default: return "";
	}
}

inline void RecoveryRgb(int r, unsigned char& red, unsigned char& g, unsigned char& b)
{
	if (r == 3) { red = 74; g = 222; b = 128; }
	else if (r == 2) { red = 251; g = 191; b = 36; }
	else if (r == 1) { red = 248; g = 113; b = 113; }
	else { red = 141; g = 160; b = 188; }
}

inline const char* RatingName(int r)
{
	switch (r)
	{
	case 1: return "Missed";
	case 2: return "Close";
	case 3: return "Bad shot, went in";
	case 4: return "Good shot, went in";
	case 5: return "Great shot, went in";
	default: return "";
	}
}
