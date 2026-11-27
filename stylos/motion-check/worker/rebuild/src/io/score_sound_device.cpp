#include "retas/score_sound_playback.hpp"
#ifdef RETAS_HAS_QT_SOUND
#include <QAudioSink>
#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>

namespace retas {
namespace {
struct Device {
    std::shared_ptr<const ScoreSoundPcm> clip;
    std::unique_ptr<QBuffer> buffer;
    std::unique_ptr<QAudioSink> sink; // destroyed before its borrowed QIODevice
    QThread* thread{QThread::currentThread()};
    void check()const{if(thread!=QThread::currentThread())throw std::logic_error("Score sound output changed event thread");}
    bool playing(){check();if(!sink)return false;if(sink->error()!=QtAudio::NoError){stop();throw std::runtime_error("Score audio output failed");}if(sink->state()==QtAudio::ActiveState||sink->state()==QtAudio::SuspendedState)return true;stop();return false;}
    void stop(){check();if(sink)sink->reset();sink.reset();buffer.reset();clip.reset();}
    void start(std::shared_ptr<const ScoreSoundPcm> source){
        check();if(!QCoreApplication::instance())throw std::logic_error("Score audio output requires QCoreApplication and an event loop");stop();
        if(!source||!source->fixed_rate||(source->channels!=1&&source->channels!=2)||
           (source->bits_per_sample!=8&&source->bits_per_sample!=16)||source->bytes.empty()||
           source->bytes.size()>score_sound_max_file_bytes||source->bytes.size()%(source->channels*(source->bits_per_sample/8)))
            throw std::invalid_argument("Invalid Score audio device PCM");
        const auto device=QMediaDevices::defaultAudioOutput();if(device.isNull())throw std::runtime_error("No default Score audio output device");
        QAudioFormat format;format.setChannelCount(source->channels);format.setSampleFormat(source->bits_per_sample==8?QAudioFormat::UInt8:QAudioFormat::Int16);
        if(!(source->fixed_rate&65535))format.setSampleRate(int(source->fixed_rate/65536));
        if(!format.isValid()||!device.isFormatSupported(format)){
            format.setSampleRate(48000);
            if(!device.isFormatSupported(format)){format.setSampleRate(44100);if(!device.isFormatSupported(format))throw std::runtime_error("Score audio device does not support mono/stereo8/16-bit PCM");}
            source=std::make_shared<const ScoreSoundPcm>(resample_score_sound_pcm(*source,std::uint32_t(format.sampleRate())));
        }
        clip=std::move(source);buffer=std::make_unique<QBuffer>();
        auto bytes=QByteArray::fromRawData(reinterpret_cast<const char*>(clip->bytes.data()),qsizetype(clip->bytes.size()));
        if constexpr(std::endian::native==std::endian::big){if(clip->bits_per_sample==16){bytes.detach();for(qsizetype i=0;i<bytes.size();i+=2)std::swap(bytes[i],bytes[i+1]);}}
        buffer->setData(bytes);if(!buffer->open(QIODevice::ReadOnly))throw std::runtime_error("Cannot open Score audio buffer");
        sink=std::make_unique<QAudioSink>(device,format);sink->start(buffer.get());
        if(sink->error()!=QtAudio::NoError){stop();throw std::runtime_error("Cannot start Score audio device");}
    }
};
}
bool score_sound_device_available()noexcept{return true;}
ScoreSoundOutput make_score_sound_device_output(){auto state=std::make_shared<Device>();return {
    [state](auto clip){state->start(std::move(clip));},[state]{return state->playing();},[state]{state->stop();}};}
void play_score_sound_device(std::shared_ptr<const ScoreSoundPcm> pcm,std::stop_token stop){
    if(!pcm||pcm->bytes.empty())return;
    if(stop.stop_requested())throw std::runtime_error("Score audio output cancelled");
    int argc=1;char name[]="paintman-sound";char* argv[]{name,nullptr};std::unique_ptr<QCoreApplication> app;
    if(!QCoreApplication::instance())app=std::make_unique<QCoreApplication>(argc,argv);
    auto output=make_score_sound_device_output();
    struct Finish {ScoreSoundOutput& output;~Finish(){try{output.stop();}catch(...) {}}} finish{output};
    output.start(std::move(pcm));QEventLoop loop;QTimer timer;std::exception_ptr error;
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{try{if(stop.stop_requested())throw std::runtime_error("Score audio output cancelled");if(!output.is_playing())loop.quit();}catch(...){error=std::current_exception();loop.quit();}});
    timer.start(10);loop.exec();if(error)std::rethrow_exception(error);
}
}
#else
namespace retas {
bool score_sound_device_available()noexcept{return false;}
ScoreSoundOutput make_score_sound_device_output(){return {
    [](auto){throw std::runtime_error("Score audio device support was not built");},[]{return false;},[]{}};}
void play_score_sound_device(std::shared_ptr<const ScoreSoundPcm>,std::stop_token){throw std::runtime_error("Score audio device support was not built");}
}
#endif
