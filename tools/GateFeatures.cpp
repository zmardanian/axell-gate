// Axell Gate - voice-activity feature dump for training (research/train_gate.py): runs the shipping VoiceActivity on
// raw float32 mono files. Manifest lines: <in.f32> <sampleRate> <out.f32>. Rows of 30 float32:
//   centre (s), x[23], levelDb, floorDb, pBack, pVoice, pBreath, loud
#include "../Source/VoiceActivity.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: GateFeatures manifest.txt\n";
        return 2;
    }
    std::ifstream man (argv[1]);
    std::string line;
    while (std::getline (man, line))
    {
        std::istringstream ss (line);
        std::string in, out;
        double fs = 0;
        if (! (ss >> in >> fs >> out))
            continue;
        std::ifstream f (in, std::ios::binary | std::ios::ate);
        const auto bytes = (size_t) f.tellg();
        std::vector<float> x (bytes / sizeof (float));
        f.seekg (0);
        f.read ((char*) x.data(), (std::streamsize) (x.size() * sizeof (float)));
        axg::VoiceActivity va;
        va.prepare (fs);
        std::vector<float> rows;
        for (float v : x)
            if (va.push (v))
            {
                const auto& o = va.out();
                rows.push_back ((float) ((double) o.center / fs));
                rows.insert (rows.end(), o.x, o.x + axg::kVadInputs);
                rows.push_back (o.levelDb);
                rows.push_back (o.floorDb);
                rows.push_back (o.pBack);
                rows.push_back (o.pVoice);
                rows.push_back (o.pBreath);
                rows.push_back (o.loud);
            }
        std::ofstream fo (out, std::ios::binary);
        fo.write ((const char*) rows.data(), (std::streamsize) (rows.size() * sizeof (float)));
    }
    return 0;
}
