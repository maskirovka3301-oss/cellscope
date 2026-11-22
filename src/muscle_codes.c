#include "muscle_codes.h"

#include <stddef.h>
#include <string.h>

typedef struct { uint8_t r, g, b; const char *code; const char *region; } Entry;

static const Entry kCodes[] = {
    {220, 156, 156, "GMa", "Gluteal"}, {212, 198, 153, "F", "Fascia"},
    {200, 156, 219, "LD", "Back"},     {219, 156, 211, "G", "Gluteal"},
    {220, 156, 190, "Pso", "Hip"},     {157, 200, 219, "T", "Lower leg"},
    {144, 150, 208, "SA", "Thorax"},   {161, 156, 220, "BF", "Thigh"},
    {157, 176, 219, "VL", "Thigh"},    {189, 219, 157, "RF", "Thigh"},
    {216, 142, 179, "RA", "Abdomen"},  {157, 194, 220, "AM", "Thigh"},
    {175, 156, 220, "FCR", "Forearm"}, {156, 219, 174, "FCR", "Forearm"},
    {171, 219, 157, "TA", "Lower leg"},{156, 187, 218, "Om", "Shoulder"},
    {156, 173, 219, "S", "Arm"},       {209, 194, 191, "FDS", "Forearm"},
    {182, 156, 220, "FCR", "Forearm"}, {157, 219, 182, "SM", "Lower leg"},
    {164, 218, 156, "TFL", "Hip"},     {235, 144, 161, "SA", "Thorax"},
    {156, 167, 220, "C", "Arm"},       {216, 156, 218, "SB", "Arm"},
    {156, 219, 200, "IB", "Leg"},      {210, 156, 218, "SY", "Leg"},
    {157, 157, 219, "LE", "Forearm"},  {166, 172, 220, "F", "Fascia"},
    {185, 156, 220, "EDM", "Forearm"}, {157, 218, 158, "C", "Arm"},
};

const char *muscle_code_for_color(uint8_t r, uint8_t g, uint8_t b)
{
    int best = 1 << 30;
    const char *code = "MUS";
    for (size_t i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); i++) {
        int dr = (int)r - kCodes[i].r;
        int dg = (int)g - kCodes[i].g;
        int db = (int)b - kCodes[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best) {
            best = d;
            code = kCodes[i].code;
        }
    }
    return best <= 30 * 30 ? code : "MUS";
}

const char *muscle_region_for_code(const char *code)
{
    if (!code) return "Muscle";
    for (size_t i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); i++)
        if (strcmp(code, kCodes[i].code) == 0) return kCodes[i].region;
    return "Muscle";
}
