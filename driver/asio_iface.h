// asio_iface.h – eigene, saubere Deklaration der oeffentlichen ASIO-Treiber-Schnittstelle (COM, vtable-kompatibel)
// (MIT, © Ben Buchholz · Buccloud Software (DE))
//
// Beschreibt nur das, was ein Treiber fuer Hosts (DAWs) bereitstellen muss. Enthaelt KEINEN Code des Steinberg-ASIO-SDKs.
// "ASIO" ist eine Marke der Steinberg Media Technologies GmbH; dieses Projekt ist nicht von Steinberg.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

namespace asio {
typedef long Bool;       // 32 Bit (Windows)
typedef long Error;
typedef double SampleRate;
static const Bool TRUE_ = 1, FALSE_ = 0;
enum : long { ASE_OK = 0, ASE_SUCCESS = 0x3f4847a0, ASE_NotPresent = -1000, ASE_HWMalfunction = -999, ASE_InvalidParameter = -998, ASE_InvalidMode = -997, ASE_SPNotAdvancing = -996, ASE_NoClock = -995, ASE_NoMemory = -994 };
enum : long { ST_Float32LSB = 19 };   // einziges Format dieses Treibers (32 Bit Float, little endian)
enum : long { kSelectorSupported = 1, kEngineVersion = 2, kResetRequest = 3, kBufferSizeChange = 4, kResyncRequest = 5, kLatenciesChanged = 6, kSupportsTimeInfo = 7, kSupportsTimeCode = 8 };
struct Samples { unsigned long hi, lo; };
struct TimeStamp { unsigned long hi, lo; };
struct ClockSource { long index, associatedChannel, associatedGroup; Bool isCurrentSource; char name[32]; };
struct ChannelInfo { long channel; Bool isInput, isActive; long channelGroup, type; char name[32]; };
struct BufferInfo { Bool isInput; long channelNum; void* buffers[2]; };
struct TimeCode { double speed; Samples timeCodeSamples; unsigned long flags; char future[64]; };
struct TimeInfo { double speed; TimeStamp systemTime; Samples samplePosition; SampleRate sampleRate; unsigned long flags; char reserved[12]; };
struct Time { long reserved[4]; TimeInfo timeInfo; TimeCode timeCode; };
struct Callbacks {
    void (*bufferSwitch)(long doubleBufferIndex, Bool directProcess);
    void (*sampleRateDidChange)(SampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    Time* (*bufferSwitchTimeInfo)(Time* params, long doubleBufferIndex, Bool directProcess);
};
// Reihenfolge der virtuellen Methoden = Reihenfolge in der vtable (nach IUnknown) - NICHT aendern
struct IAsio : public IUnknown {
    virtual Bool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* str) = 0;
    virtual Error start() = 0;
    virtual Error stop() = 0;
    virtual Error getChannels(long* numIn, long* numOut) = 0;
    virtual Error getLatencies(long* inLat, long* outLat) = 0;
    virtual Error getBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) = 0;
    virtual Error canSampleRate(SampleRate rate) = 0;
    virtual Error getSampleRate(SampleRate* rate) = 0;
    virtual Error setSampleRate(SampleRate rate) = 0;
    virtual Error getClockSources(ClockSource* clocks, long* numSources) = 0;
    virtual Error setClockSource(long reference) = 0;
    virtual Error getSamplePosition(Samples* pos, TimeStamp* ts) = 0;
    virtual Error getChannelInfo(ChannelInfo* info) = 0;
    virtual Error createBuffers(BufferInfo* infos, long numChannels, long bufferSize, Callbacks* callbacks) = 0;
    virtual Error disposeBuffers() = 0;
    virtual Error controlPanel() = 0;
    virtual Error future(long selector, void* opt) = 0;
    virtual Error outputReady() = 0;
};
}  // namespace asio
