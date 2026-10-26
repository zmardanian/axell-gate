// Axell Gate - offline render through the complete engine (benchmarks, preset checks, listening files).
// GateRender in.wav out.wav [key=value ...]   keys: mode detect speed threshold sens breath range attack hold release
//                                                  tail sensdb pattern rate swing edge bpm output block gainout
// Output is latency-compensated (same length and alignment as the input), float32. gainout=path.f32 also writes the
// applied gain per sample (aligned), for exact scoring.
#include "../Source/Engine.h"
#include "WavIO.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: GateRender in.wav out.wav [key=value ...]\n";
        return 2;
    }
    wav::Audio a;
    if (! wav::read (argv[1], a))
    {
        std::cerr << "cannot read " << argv[1] << "\n";
        return 1;
    }
    axg::Params p;
    axg::Transport tr;
    int block = 512;
    std::string gainOut, scPath;
    for (int i = 3; i < argc; ++i)
    {
        std::string kv = argv[i];
        const auto eq = kv.find ('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = kv.substr (0, eq), sv = kv.substr (eq + 1);
        const float v = (float) std::atof (sv.c_str());
        if (k == "mode") p.mode = (int) v;
        else if (k == "detect") p.detect = (int) v;
        else if (k == "speed") p.speed = (int) v;
        else if (k == "threshold") p.thresholdDb = v;
        else if (k == "sens") p.sensitivity = v;
        else if (k == "breath") p.breathDb = v;
        else if (k == "range") p.rangeDb = v;
        else if (k == "attack") p.attackMs = v;
        else if (k == "hold") p.holdMs = v;
        else if (k == "pitchhold") p.pitchHold = v > 0.5;
        else if (k == "release") p.releaseMs = v;
        else if (k == "tail") p.tailPct = v;
        else if (k == "sensdb") p.sensDb = v;
        else if (k == "pattern") p.pattern = (uint32_t) std::strtoul (sv.c_str(), nullptr, 0);
        else if (k == "rate") p.rate = (int) v;
        else if (k == "swing") p.swingPct = v;
        else if (k == "edge") p.edgeMs = v;
        else if (k == "bpm") { tr.valid = true; tr.playing = true; tr.bpm = v; }
        else if (k == "output") p.outputDb = v;
        else if (k == "preopen") p.preOpenMs = v;
        else if (k == "closeearly") p.closeEarlyMs = v;
        else if (k == "veto") p.roomVeto = v;
        else if (k == "roomrun") p.roomRunMs = v;
        else if (k == "resume") p.resumePerFrame = v;
        else if (k == "tolong") p.toLongPerFrame = v;
        else if (k == "ramplaw") p.rampLaw = (int) v;
        else if (k == "lag") p.lagFrames = (int) v;
        else if (k == "block") block = (int) v;
        else if (k == "gainout") gainOut = sv;
        else if (k == "sc") scPath = sv;
        else if (k == "bleed") p.bleed = v != 0;
        else if (k == "freeze") p.bleedFreeze = v != 0;
        else std::cerr << "unknown key " << k << "\n";
    }
    const int nch = std::min<int> (2, (int) a.ch.size());
    axg::Engine e;
    e.setParams (p);
    e.prepare (a.fs, nch, block);
    const int lat = e.getLatencySamples();
    const size_t n = a.frames(), total = n + (size_t) lat;
    std::vector<std::vector<float>> buf ((size_t) nch, std::vector<float> (total, 0.0f));
    for (int c = 0; c < nch; ++c)
        std::copy (a.ch[(size_t) c].begin(), a.ch[(size_t) c].end(), buf[(size_t) c].begin());
    // gain probe: the same engine on a constant 1.0 signal would not see the audio; instead run a twin on the real
    // audio and divide where the input is not tiny (exact: the gain is a pure multiplication)
    wav::Audio sca;
    if (! scPath.empty() && ! wav::read (scPath, sca))
    {
        std::cerr << "cannot read " << scPath << "\n";
        return 1;
    }
    const int scCh = std::min<int> (2, (int) sca.ch.size());
    for (auto& c : sca.ch)
        c.resize (total, 0.0f);
    std::vector<float> gains (total, 0.0f);
    e.gainTap = gains.data();
    const auto t0 = std::chrono::steady_clock::now();
    axg::DisplayFrame f;
    for (size_t s = 0; s < total; s += (size_t) block)
    {
        const int len = (int) std::min<size_t> ((size_t) block, total - s);
        float* io[2] = { buf[0].data() + s, nch > 1 ? buf[1].data() + s : nullptr };
        tr.ppq = tr.valid ? (double) s / a.fs * tr.bpm / 60.0 : 0.0;
        e.setTransport (tr);
        const float* sc[2] = { scCh > 0 ? sca.ch[0].data() + s : nullptr, scCh > 1 ? sca.ch[1].data() + s : nullptr };
        e.process (io, nch, len, sc, scCh);
        while (e.popDisplay (f)) {}
    }
    const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    wav::Audio o;
    o.fs = a.fs;
    o.ch.resize ((size_t) nch);
    for (int c = 0; c < nch; ++c)
        o.ch[(size_t) c].assign (buf[(size_t) c].begin() + lat, buf[(size_t) c].end());
    if (! gainOut.empty())
    {
        std::vector<float> g (gains.begin() + lat, gains.end()); // the engine's own gain, aligned
        std::ofstream fo (gainOut, std::ios::binary);
        fo.write ((const char*) g.data(), (std::streamsize) (g.size() * sizeof (float)));
    }
    std::cout << "latency " << lat << " samples (" << 1000.0 * lat / a.fs << " ms); " << 100.0 * sec / ((double) n / a.fs)
              << " % of real time\n";
    return wav::write (argv[2], o) ? 0 : 1;
}
