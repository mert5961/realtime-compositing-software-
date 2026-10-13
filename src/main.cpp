#include "compositor/core.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace compositor;
using namespace std::chrono_literals;

namespace {
constexpr std::uint32_t width = 320;
constexpr std::uint32_t height = 180;

void generate(FramePool& pool, std::uint64_t frame, Time time, bool foreground) {
    auto writer = pool.try_write();
    if (!writer) throw std::runtime_error("Synthetic producer ran out of surfaces");
    auto pixels = writer->pixels();
    const int center = 70 + static_cast<int>(frame % 180);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
            if (foreground) {
                const int dx = static_cast<int>(x) - center;
                const int dy = static_cast<int>(y) - 90;
                const auto alpha = static_cast<std::uint8_t>(dx * dx + dy * dy < 32 * 32 ? 220 : 0);
                pixel = {245, 135, 45, alpha};
            } else {
                pixel = {static_cast<std::uint8_t>(20 + x / 4),
                         static_cast<std::uint8_t>(35 + y / 2),
                         static_cast<std::uint8_t>(90 + frame % 80), 255};
            }
        }
    }
    writer->publish({frame, time, time});
}

void save_ppm(const std::filesystem::path& path, std::span<const Pixel> pixels) {
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto pixel : pixels) {
        const char rgb[] = {static_cast<char>(pixel.r), static_cast<char>(pixel.g), static_cast<char>(pixel.b)};
        stream.write(rgb, 3);
    }
}

std::string_view reason(SelectionStatus status) {
    switch (status) {
    case SelectionStatus::ready: return "ready";
    case SelectionStatus::missing_input: return "missing_or_expired_input";
    case SelectionStatus::excessive_skew: return "excessive_skew";
    case SelectionStatus::deadline_missed: return "deadline_missed";
    }
    return "unknown";
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto directory = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("output");
        std::filesystem::create_directories(directory);
        std::ofstream trace(directory / "frames.csv");
        trace.exceptions(std::ios::failbit | std::ios::badbit);
        trace << "tick,target_ns,action,reason,foreground_sequence,scene_sequence,output_age_ns\n";

        FramePool camera(3, width, height), blender(3, width, height), output(3, width, height);
        FrameScheduler scheduler;
        std::optional<FramePool::ReadLease> last_output;
        std::size_t composited = 0, repeated = 0, unavailable = 0;
        std::uint64_t last_camera = 0, last_scene = 0;

        for (std::uint64_t frame = 0; frame < 120; ++frame) {
            // Exact rational 60 Hz timestamps for this bounded virtual-clock run.
            const Time target(static_cast<Time::rep>(frame * 1'000'000'000 / 60));
            const Time deadline(static_cast<Time::rep>((frame + 1) * 1'000'000'000 / 60));
            generate(camera, frame, target, true);
            const bool blender_stalled = frame >= 44 && frame < 70;
            if (frame % 2 == 0 && !blender_stalled) generate(blender, frame, target, false);
            // Exercise the admission deadline independently of source availability.
            const Time now = frame == 95 ? deadline : target;
            const auto selected = scheduler.select(camera, blender, {target, now, deadline});
            std::string_view action;
            if (selected.status == SelectionStatus::ready) {
                auto writer = output.try_write();
                if (!writer) throw std::runtime_error("Output pool exhausted");
                composite(selected.foreground->pixels(), selected.scene->pixels(), writer->pixels());
                writer->publish({frame, target, now + 2ms});
                last_output = output.latest(target, deadline, 100ms);
                if (!last_output) throw std::runtime_error("Output publication failed");
                last_camera = selected.foreground->info().sequence;
                last_scene = selected.scene->info().sequence;
                ++composited;
                action = "composite";
            } else if (last_output) {
                ++repeated;
                action = "repeat";
            } else {
                ++unavailable;
                action = "unavailable";
            }
            trace << frame << ',' << target.count() << ',' << action << ',' << reason(selected.status) << ',';
            if (last_output) {
                trace << last_camera << ',' << last_scene << ',' << (target - last_output->info().content_time).count();
            } else {
                trace << ",,";
            }
            trace << '\n';
        }
        if (last_output) save_ppm(directory / "composite.ppm", last_output->pixels());
        std::cout << "Virtual-clock CPU simulation (no Blender connection or live display)\n"
                  << "120 output ticks; camera 60 fps; simulated Blender 30 fps\n"
                  << "Blender stalls on ticks 44-69; deadline miss injected on tick 95\n"
                  << "Composited: " << composited << "; repeated: " << repeated
                  << "; unavailable: " << unavailable << '\n'
                  << "Fixed pixel storage: " << camera.storage_bytes() + blender.storage_bytes() + output.storage_bytes()
                  << " bytes across 9 surfaces\n"
                  << "Trace: " << (directory / "frames.csv").string() << '\n'
                  << "Image: " << (directory / "composite.ppm").string() << '\n';
        return composited == 94 && repeated == 26 && unavailable == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
