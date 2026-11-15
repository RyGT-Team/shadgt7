#include "input/feel_settings.h"
#include "input/telemetry_haptics.h"
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace Input::TelemetryHaptics;
static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static Frame Cruising() {
    Frame frame;
    frame.flags = 1;
    frame.speed = 30;
    frame.velocity = {30, 0, 0};
    frame.rpm = 5000;
    frame.rpm_limit = 8000;
    frame.gear = 4;
    frame.throttle = 128;
    frame.radius.fill(0.3f);
    frame.omega.fill(100);
    frame.suspension.fill(0.05f);
    return frame;
}
static float Peak(Synth& synth, int frames, int channel) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 4);
    synth.Render(out.data(), frames, 4, 2, 3);
    float peak = 0;
    for (int i = 0; i < frames; ++i) peak = std::max(peak, std::abs(out[static_cast<std::size_t>(i) * 4 + channel]));
    return peak;
}
int main() {
    constexpr float dt = 1.0f / 60;
    {
        Analyzer analyzer;
        Frame frame = Cruising();
        for (int i = 0; i < 5; ++i) {
            const Events events = analyzer.Update(frame, dt);
            Check(events.knock[0] == 0 && events.knock[1] == 0, "a smooth road does not knock");
            Check(events.slip[0] == 0 && events.slip[1] == 0, "rolling wheels do not slip");
            Check(i == 0 || (events.engine > 0 && events.engine_hz > 35 && events.engine_hz < 140), "the engine hums at a pitch between idle and limiter");
        }
        frame.suspension[0] += 0.012f; // a kerb under the front-left wheel
        Events events = analyzer.Update(frame, dt);
        Check(events.knock[0] > 0.5f && events.knock[1] == 0, "a left kerb knocks the left side only");
        Check(events.knock_hz[0] < 60, "a kerb is a low thud");
        frame.suspension[0] -= 0.012f;
        for (int i = 0; i < 3; ++i) analyzer.Update(frame, dt); // the left wheel settles
        frame.suspension[3] += 0.012f; // rear-right
        events = analyzer.Update(frame, dt);
        Check(events.knock[1] > 0.5f && events.knock[0] == 0, "a right kerb knocks the right side only");
    }
    {
        Analyzer analyzer;
        Frame frame = Cruising();
        analyzer.Update(frame, dt);
        analyzer.Update(frame, dt);
        frame.gear = 5;
        const Events events = analyzer.Update(frame, dt);
        Check(events.knock[0] > 0.5f && events.knock[1] > 0.5f && events.knock_hz[0] > 80, "a gear change is a sharp click on both sides");
    }
    {
        Analyzer analyzer;
        Frame frame = Cruising();
        analyzer.Update(frame, dt);
        analyzer.Update(frame, dt);
        frame.omega[1] = 150; // front-right spinning
        frame.omega[3] = 150;
        const Events events = analyzer.Update(frame, dt);
        Check(events.slip[1] > 0.9f && events.slip[0] == 0, "wheelspin on the right is felt on the right");
    }
    {
        Analyzer analyzer;
        Frame frame = Cruising();
        analyzer.Update(frame, dt);
        analyzer.Update(frame, dt);
        frame.velocity = {28, 0, 0}; // 2 m/s in one frame: 120 m/s^2
        Events events = analyzer.Update(frame, dt);
        Check(events.knock[0] > 0.9f && events.knock_noise[0] == 1 && events.knock_hz[0] < 45, "an impact is a heavy, rough hit");
        frame.velocity = {28 - 30, 0, 0}; // 30 m/s in a frame: a reset, not a crash
        events = analyzer.Update(frame, dt);
        Check(events.knock[0] == 0, "a reset is not a crash");
    }
    {
        Analyzer analyzer;
        Frame frame = Cruising();
        frame.flags = 0; // replay or menu
        analyzer.Update(frame, dt);
        frame.suspension[0] += 0.05f;
        frame.gear = 2;
        const Events events = analyzer.Update(frame, dt);
        Check(events.knock[0] == 0 && events.knock[1] == 0 && events.engine == 0, "nothing plays when not driving");
    }
    {
        Synth synth;
        Check(Peak(synth, 4800, 2) == 0 && Peak(synth, 4800, 3) == 0, "silent until told otherwise");
        Events events;
        events.knock[0] = 1;
        events.knock_hz[0] = 50;
        synth.Apply(events);
        const float left = Peak(synth, 2400, 2);
        Synth right_only;
        right_only.Apply(events);
        std::vector<float> out(4800 * 4, 0.0f);
        right_only.Render(out.data(), 4800, 4, 2, 3);
        float right = 0, other = 0;
        for (int i = 0; i < 4800; ++i) {
            right = std::max(right, std::abs(out[static_cast<std::size_t>(i) * 4 + 3]));
            other = std::max(other, std::max(std::abs(out[static_cast<std::size_t>(i) * 4]), std::abs(out[static_cast<std::size_t>(i) * 4 + 1])));
        }
        Check(left > 0.4f && right == 0 && other == 0, "a left knock plays on the left actuator only, never the speaker");
        Peak(synth, 2400, 2); // 50-100 ms
        Check(Peak(synth, 4800, 2) < 0.1f, "a knock dies away within about 100 ms");
        events = {};
        events.engine = 0.3f;
        events.engine_hz = 100;
        synth.Apply(events);
        Peak(synth, 9600, 2);
        Check(Peak(synth, 4800, 2) > 0.1f && Peak(synth, 4800, 3) > 0.1f, "the engine hums on both sides");
        synth.Stop();
        Peak(synth, 48000, 2);
        Check(Peak(synth, 4800, 2) < 0.01f, "stopping fades out");
    }
    // FEAT-GT7-179: the launcher's sliders. 0 turns an effect off, others scale it.
    {
        Gains off;
        off.road = 0;
        Analyzer analyzer;
        Frame frame = Cruising();
        analyzer.Update(frame, dt, off);
        analyzer.Update(frame, dt, off);
        frame.suspension[0] += 0.012f;
        Check(analyzer.Update(frame, dt, off).knock[0] == 0, "road at 0 silences kerbs");
    }
    for (int effect = 0; effect < 4; ++effect) {
        Gains half, full;
        float* field = effect == 0 ? &half.shift : effect == 1 ? &half.impact : effect == 2 ? &half.engine : &half.slip;
        *field = 0.5f;
        auto run = [&](const Gains& gains) {
            Analyzer analyzer;
            Frame frame = Cruising();
            analyzer.Update(frame, dt, gains);
            analyzer.Update(frame, dt, gains);
            if (effect == 0) frame.gear = 5;
            if (effect == 1) frame.velocity = {28, 0, 0};
            if (effect == 3) frame.omega[1] = frame.omega[3] = 150;
            const Events events = analyzer.Update(frame, dt, gains);
            return effect == 2 ? events.engine : effect == 3 ? events.slip[1] : events.knock[0];
        };
        const float a = run(half), b = run(full);
        Check(b > 0 && std::abs(a - b * 0.5f) < 0.02f, "a slider at 50% halves its effect");
        *field = 0;
        Check(run(half) == 0, "a slider at 0 turns its effect off");
    }
    {
        Synth synth;
        synth.SetStrength(0);
        Events events;
        events.knock[0] = events.knock[1] = 1;
        events.engine = 0.3f;
        events.engine_hz = 80;
        synth.Apply(events);
        Check(Peak(synth, 4800, 2) == 0 && Peak(synth, 4800, 3) == 0, "overall HD rumble at 0 is silent");
    }
    {
        Input::Feel::Settings settings;
        Check(Input::Feel::ParseArgument("haptics_engine=0", settings) && settings.engine == 0, "settings parse");
        Check(Input::Feel::ParseArgument("trigger_brake=999", settings) && settings.brake == 200, "settings clamp to 200");
        Check(!Input::Feel::ParseArgument("haptics_unknown=50", settings) && !Input::Feel::ParseArgument("haptics_road=x", settings),
              "unknown names and bad values are rejected");
        Check(std::size(Input::Feel::Keys) == 13, "every setting has a config key");
    }
    std::puts("telemetry haptics tests passed");
    return 0;
}
