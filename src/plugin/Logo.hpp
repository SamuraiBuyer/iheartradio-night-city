// iHeart Radio - logo engine (C++ port of src/manager/ui/logo-core.js, same steps and numbers).
//
// Turns any station logo into a Cyberpunk 2077 radio icon: one flat white shape on transparent (the game tints it),
// like base\gameplay\gui\common\icons\radiostations_icons.
//   Decode(bytes)           PNG / JPEG -> RGBA, at most 512 px on its longest side (like the app's canvas)
//   Analyze(image)          background removal + colour clusters
//   Candidates(an, gap)     several ways to draw the logo as one colour, best first
//   Fit(ink, w, h, size)    crop + centre into a size x size alpha map (the icon), top row first
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Logo
{
struct Image
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};
bool Decode(const std::string& aBytes, Image& aOut, int aMaxSide = 512);

struct Analysis
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> alpha, lum;
    std::string bgRemoved;
    std::vector<std::array<double, 3>> centers;
    std::vector<int8_t> labels;
    std::vector<float> share;
};
Analysis Analyze(const Image& aImage);

struct Candidate
{
    std::string key, label;
    std::vector<float> ink;
    double score = 0;
};
std::vector<Candidate> Candidates(const Analysis& aAn, int aGap);

std::vector<uint8_t> Fit(const std::vector<float>& aInk, int aW, int aH, int aSize, bool aInvert);
} // namespace Logo
