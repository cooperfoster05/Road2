#pragma once
// Names and colors for 1-5 grades, shared by the panel, the rep list and the settings.

inline void OverallRgb(int o, unsigned char& r, unsigned char& g, unsigned char& b)
{
	if (o >= 4) { r = 115; g = 230; b = 115; }
	else if (o == 3) { r = 255; g = 204; b = 77; }
	else if (o == 2) { r = 255; g = 160; b = 80; }
	else if (o == 1) { r = 255; g = 115; b = 102; }
	else { r = 160; g = 160; b = 160; }
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
