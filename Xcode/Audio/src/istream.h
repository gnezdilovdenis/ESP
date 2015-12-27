/*
 * IStream encapsulates a number of stream input devices: serial,
 * audio, etc.
 */
#pragma once

#include "ofMain.h"

class IStream {
  public:
    IStream();
    virtual ~IStream() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    bool hasStarted() { return has_started_; }

    typedef std::function<void(vector<double>)> onDataReadyCallback;

    void onDataReadyEvent(onDataReadyCallback callback) {
        data_ready_callback_ = callback;
    }

    template<typename T1, typename arg, class T>
    void onDataReadyEvent(T1* owner, void (T::*listenerMethod)(arg)) {
        using namespace std::placeholders;
        data_ready_callback_ = std::bind(listenerMethod, owner, _1);
    }

  protected:
    bool has_started_;
    onDataReadyCallback data_ready_callback_;
};

class AudioStream : public ofBaseApp, public IStream {
  public:
    AudioStream();
    void audioIn(float *input, int buffer_size, int nChannel);
    virtual void start() final;
    virtual void stop() final;
  private:
    unique_ptr<ofSoundStream> sound_stream_;
};

class SerialStream : public IStream {
  public:
    SerialStream(int i);
    virtual void start() final;
    virtual void stop() final;
  private:
    unique_ptr<ofSerial> serial_;

    // A separate reading thread to read data from Serial.
    unique_ptr<std::thread> reading_thread_;
    void readSerial();

    // Serial buffer size
    int buffer_size_ = 32;
};