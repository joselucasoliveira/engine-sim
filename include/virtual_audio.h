#ifndef ENGINE_SIM_VIRTUAL_AUDIO_H
#define ENGINE_SIM_VIRTUAL_AUDIO_H
#include "desktop_host.h"
#include <algorithm>
#include <chrono>

namespace desktop {
// Explicit clocked software sink for the legacy Windows smoke path. It never
// represents physical playback and is never selected as a device fallback.
class VirtualAudio {
public:
    void open() {
        if (opened) throw DesktopError("virtual audio","already open");
        samples.fill(0); valid.fill(false); opened=true; position=0;
        consumed=source=silence=nonzero=0; locked=playing=false;
    }
    void setPlaying(bool value) {
        if (!opened) throw DesktopError("virtual audio","not open");
        if (playing) cursor();
        playing=value; previous=std::chrono::steady_clock::now();
    }
    void quiesce() noexcept { playing=false; }
    void shutdown() noexcept { quiesce(); opened=false; }
    AudioOffset cursor() {
        if (!opened) throw DesktopError("virtual audio","not open");
        if (!playing) return position;
        const auto now=std::chrono::steady_clock::now();
        const auto elapsed=std::chrono::duration<double>(now-previous).count();
        if (elapsed>1.0) throw DesktopError("virtual audio","one-second poll deadline exceeded");
        const auto count=static_cast<unsigned>(elapsed*44100);
        for (unsigned i=0;i<count;++i) {
            if (valid[position]) { ++source; if (samples[position]) ++nonzero; valid[position]=false; }
            else ++silence;
            position=(position+1)%samples.size(); ++consumed;
        }
        previous+=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(count/44100.0));
        return position;
    }
    AudioSegments lock(AudioOffset offset,AudioOffset count) {
        if (!opened || locked || offset>=samples.size() || count>samples.size()) throw DesktopError("virtual audio","invalid source span");
        for (AudioOffset i=0;i<count;++i) if (valid[(offset+i)%samples.size()]) throw DesktopError("virtual audio","source ring overflow");
        const auto first=std::min<AudioOffset>(count,samples.size()-offset);
        active={samples.data()+offset,samples.data(),first,count-first}; locked=true;
        return active;
    }
    void unlock(const AudioSegments &segments) {
        if (!locked || segments.first!=active.first || segments.second!=active.second
            || segments.firstCount!=active.firstCount || segments.secondCount!=active.secondCount)
            throw DesktopError("virtual audio","source span does not match the active lock");
        const auto offset=static_cast<std::size_t>(segments.first-samples.data());
        for (std::size_t i=0;i<segments.firstCount+segments.secondCount;++i) valid[(offset+i)%samples.size()]=true;
        locked=false;
    }
    std::uint64_t consumed=0,source=0,silence=0,nonzero=0;
private:
    std::array<std::int16_t,44100> samples{};
    std::array<bool,44100> valid{};
    AudioSegments active;
    std::chrono::steady_clock::time_point previous;
    AudioOffset position=0;
    bool opened=false,playing=false,locked=false;
};
}
#endif
