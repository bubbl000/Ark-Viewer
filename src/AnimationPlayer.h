#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <chrono>

struct AnimationFrame {
    int width=0, height=0, stride=0, delayMs=100;
    std::vector<uint8_t> pixels;
};

class AnimationPlayer {
public:
    AnimationPlayer();
    ~AnimationPlayer();
    bool Open(const uint8_t* data, size_t len, const std::string& ext);
    int  NextFrame();
    const AnimationFrame* CurrentFrame() const;
    void Reset();
    int  FrameCount() const { return (int)_frames.size(); }
    bool IsPlaying() const { return _playing; }
    void SetPlaying(bool p) { _playing = p; }
    bool SetFrame(int index);  // 跳帧：clamp 到 [0,FrameCount-1]，自动暂停
    int  CurrentIndex() const { return _currentFrame; }
private:
    std::vector<AnimationFrame> _frames;
    int _currentFrame = 0;
    bool _playing = false;
    std::chrono::steady_clock::time_point _lastTick;
    bool LoadGif(const uint8_t* data, size_t len);
};
