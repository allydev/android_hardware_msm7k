/*
** Copyright 2008, Google Inc.
** Copyright (c) 2009, Code Aurora Forum. All rights reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <math.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioHardwareMSM7X30"
#include <utils/Log.h>
#include <utils/String8.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>
#include "inc/control.h"
// hardware specific functions

#include "AudioHardware.h"
#include <media/AudioRecord.h>

#define LOG_SND_RPC 0  // Set to 1 to log sound RPC's

#define AMRNB_DEVICE_IN "/dev/msm_amrnb_in"
#define EVRC_DEVICE_IN "/dev/msm_evrc_in"
#define QCELP_DEVICE_IN "/dev/msm_qcelp_in"
#define AAC_DEVICE_IN "/dev/msm_aac_in"
#define AMRNB_FRAME_SIZE 32
#define EVRC_FRAME_SIZE 23
#define QCELP_FRAME_SIZE 35

namespace android {
static int audpre_index, tx_iir_index;
static void * acoustic;
const uint32_t AudioHardware::inputSamplingRates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

static uint32_t SND_DEVICE_CURRENT=-1;
static uint32_t SND_DEVICE_HANDSET=-1;
static uint32_t SND_DEVICE_SPEAKER=-1;
static uint32_t SND_DEVICE_BT=-1;
static uint32_t SND_DEVICE_BT_EC_OFF=-1;
static uint32_t SND_DEVICE_HEADSET=-1;
static uint32_t SND_DEVICE_HEADSET_AND_SPEAKER=-1;
static uint32_t SND_DEVICE_IN_S_SADC_OUT_HANDSET=-1;
static uint32_t SND_DEVICE_IN_S_SADC_OUT_SPEAKER_PHONE=-1;
static uint32_t SND_DEVICE_TTY_HEADSET=-1;
static uint32_t SND_DEVICE_TTY_HCO=-1;
static uint32_t SND_DEVICE_TTY_VCO=-1;

static uint32_t SND_DEVICE_TTY_FULL=-1;
static uint32_t SND_DEVICE_CARKIT=-1;
static uint32_t SND_DEVICE_FM_SPEAKER=-1;
static uint32_t SND_DEVICE_FM_HEADSET=-1;
static uint32_t SND_DEVICE_NO_MIC_HEADSET=-1;

char* name[20][44];
int dev_cnt = 0;
int mixer_cnt = 0;
unsigned short dec_id = 65535;
unsigned short in_dec_id = 65535;
bool VoiceDeviceIsEnabled = false;
typedef struct routing_table
{
    unsigned short dec_id;
    int dev_id;
    int stream_type;
    struct routing_table *next;
} Routing_table;
Routing_table* head;

typedef struct device_table
{
    int dev_id;
    int class_id;
    int capability;
}Device_table;
Device_table device_list[20];
static void amr_transcode(unsigned char *src, unsigned char *dst);
bool fmState = false;
//
// ----------------------------------------------------------------------------

AudioHardware::AudioHardware() :
    mInit(false), mMicMute(true), mBluetoothNrec(true), mBluetoothId(0),
    mOutput(0), mSndEndpoints(NULL), mCurSndDevice(-1)
{

        int control;
        int i = 0;

        LOGE("msm_mixer_open: Opening the device");
        control = msm_mixer_open("/dev/snd/controlC0", 0);
        if(control< 0)
                LOGE("ERROR opening the device");

        mixer_cnt = msm_mixer_count();
        LOGE("msm_mixer_count:mixer_cnt =%d",mixer_cnt);

        dev_cnt = msm_get_device_list(name);
        LOGE("got device_list %d",dev_cnt);

        for(i = 0; i < dev_cnt;) {
                device_list[i].dev_id = msm_get_device((char* )&name[i][0]);
                if(device_list[i].dev_id >= 0) {
                        LOGV("Found device: %s:dev_id: %d",( char* )&name[i][0], device_list[i].dev_id);
                }
                device_list[i].class_id = msm_get_device_class(device_list[i].dev_id);
                device_list[i].capability = msm_get_device_capability(device_list[i].dev_id);
                LOGV("class ID = %d,capablity = %d for device %d",device_list[i].class_id,device_list[i].capability,device_list[i].dev_id);
                i++;
        }
       mInit = true;
       VoiceDeviceIsEnabled = false;
}

AudioHardware::~AudioHardware()
{
    for (size_t index = 0; index < mInputs.size(); index++) {
        closeInputStream((AudioStreamIn*)mInputs[index]);
    }
    mInputs.clear();
    closeOutputStream((AudioStreamOut*)mOutput);
    delete [] mSndEndpoints;
    if (acoustic) {
        ::dlclose(acoustic);
        acoustic = 0;
    }
    msm_mixer_close();

    mInit = false;
}

status_t AudioHardware::initCheck()
{
    return mInit ? NO_ERROR : NO_INIT;
}

AudioStreamOut* AudioHardware::openOutputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status)
{
    { // scope for the lock
        Mutex::Autolock lock(mLock);

        // only one output stream allowed
        if (mOutput) {
            if (status) {
                *status = INVALID_OPERATION;
            }
            return 0;
        }

        // create new output stream
        AudioStreamOutMSM72xx* out = new AudioStreamOutMSM72xx();
        status_t lStatus = out->set(this, devices, format, channels, sampleRate);
        if (status) {
            *status = lStatus;
        }
        if (lStatus == NO_ERROR) {
            mOutput = out;
        } else {
            delete out;
        }
    }
    return mOutput;
}

void AudioHardware::closeOutputStream(AudioStreamOut* out) {
    Mutex::Autolock lock(mLock);
    if (mOutput == 0 || mOutput != out) {
        LOGW("Attempt to close invalid output stream");
    }
    else {
        delete mOutput;
        mOutput = 0;
    }
}

AudioStreamIn* AudioHardware::openInputStream(
        uint32_t devices, int *format, uint32_t *channels, uint32_t *sampleRate, status_t *status,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    // check for valid input source
    if (!AudioSystem::isInputDevice((AudioSystem::audio_devices)devices)) {
        return 0;
    }

    mLock.lock();

    AudioStreamInMSM72xx* in = new AudioStreamInMSM72xx();
    status_t lStatus = in->set(this, devices, format, channels, sampleRate, acoustic_flags);
    if (status) {
        *status = lStatus;
    }
    if (lStatus != NO_ERROR) {
        mLock.unlock();
        delete in;
        return 0;
    }

    mInputs.add(in);
    mLock.unlock();

    return in;
}

void AudioHardware::closeInputStream(AudioStreamIn* in) {
    Mutex::Autolock lock(mLock);

    ssize_t index = mInputs.indexOf((AudioStreamInMSM72xx *)in);
    if (index < 0) {
        LOGW("Attempt to close invalid input stream");
    } else {
        mLock.unlock();
        delete mInputs[index];
        mLock.lock();
        mInputs.removeAt(index);
    }
}

status_t AudioHardware::setMode(int mode)
{
    status_t status = AudioHardwareBase::setMode(mode);
    if (status == NO_ERROR) {
        // make sure that doAudioRouteOrMute() is called by doRouting()
        // even if the new device selected is the same as current one.
        mCurSndDevice = -1;
    }
    return status;
}

bool AudioHardware::checkOutputStandby()
{
    if (mOutput)
        if (!mOutput->checkStandby())
            return false;

    return true;
}

status_t AudioHardware::setMicMute(bool state)
{
    Mutex::Autolock lock(mLock);
    return setMicMute_nosync(state);
}

// always call with mutex held
status_t AudioHardware::setMicMute_nosync(bool state)
{
    if (mMicMute != state) {
        mMicMute = state;
        return doAudioRouteOrMute(SND_DEVICE_CURRENT);
    }
    return NO_ERROR;
}

status_t AudioHardware::getMicMute(bool* state)
{
    *state = mMicMute;
    return NO_ERROR;
}

status_t AudioHardware::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 value;
    String8 key;
    const char BT_NREC_KEY[] = "bt_headset_nrec";
    const char BT_NAME_KEY[] = "bt_headset_name";
    const char BT_NREC_VALUE_ON[] = "on";
    const char FM_NAME_KEY[] = "FMRadioOn";
    const char FM_VALUE_TRUE[] = "true";
    const char FM_VALUE_FALSE[] = "false";


    LOGV("setParameters() %s", keyValuePairs.string());

    if (keyValuePairs.length() == 0) return BAD_VALUE;

    key = String8(BT_NREC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == BT_NREC_VALUE_ON) {
            mBluetoothNrec = true;
        } else {
            mBluetoothNrec = false;
            LOGI("Turning noise reduction and echo cancellation off for BT "
                 "headset");
        }
    }
    key = String8(BT_NAME_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mBluetoothId = 0;
        for (int i = 0; i < mNumSndEndpoints; i++) {
            if (!strcasecmp(value.string(), mSndEndpoints[i].name)) {
                mBluetoothId = mSndEndpoints[i].id;
                LOGI("Using custom acoustic parameters for %s", value.string());
                break;
            }
        }
        if (mBluetoothId == 0) {
            LOGI("Using default acoustic parameters "
                 "(%s not in acoustic database)", value.string());
            doRouting(NULL);
        }
    }
    key = String8(FM_NAME_KEY);
    if(param.get(key,value) == NO_ERROR)
    {
       if(value == FM_VALUE_TRUE && fmState == false) {
           fmState = true;
           doRouting(NULL);

       }
       else if(value == FM_VALUE_FALSE && fmState == true) {
           fmState = false;
           doRouting(NULL);
       }
    }
    return NO_ERROR;
}

String8 AudioHardware::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    return param.toString();
}


static unsigned calculate_audpre_table_index(unsigned index)
{
    switch (index) {
        case 48000:    return SAMP_RATE_INDX_48000;
        case 44100:    return SAMP_RATE_INDX_44100;
        case 32000:    return SAMP_RATE_INDX_32000;
        case 24000:    return SAMP_RATE_INDX_24000;
        case 22050:    return SAMP_RATE_INDX_22050;
        case 16000:    return SAMP_RATE_INDX_16000;
        case 12000:    return SAMP_RATE_INDX_12000;
        case 11025:    return SAMP_RATE_INDX_11025;
        case 8000:    return SAMP_RATE_INDX_8000;
        default:     return -1;
    }
}
size_t AudioHardware::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    if ((format != AudioSystem::PCM_16_BIT) &&
        (format != AudioSystem::AMR_NB)      &&
        (format != AudioSystem::EVRC)      &&
        (format != AudioSystem::QCELP)  &&
        (format != AudioSystem::AAC)){
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if (channelCount < 1 || channelCount > 2) {
        LOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }

    if (format == AudioSystem::AMR_NB)
       return 1280*channelCount;
    if (format == AudioSystem::EVRC)
       return 1150*channelCount;
    else if (format == AudioSystem::QCELP)
       return 1050*channelCount;
    else if (format == AudioSystem::AAC)
       return 2048;
    else
       return 2048*channelCount;
}

static status_t set_volume_rpc(uint32_t device,
                               uint32_t method,
                               uint32_t volume)
{
    LOGV("set_volume_rpc(%d, %d, %d)\n", device, method, volume);

    if (device == -1UL) return NO_ERROR;
     return NO_ERROR;
}

status_t AudioHardware::setVoiceVolume(float v)
{
    if (v < 0.0) {
        LOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        LOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int vol = lrint(v * 7.0);
    LOGD("setVoiceVolume(%f)\n", v);
    LOGI("Setting in-call volume to %d (available range is 0 to 5)\n", vol);

    Mutex::Autolock lock(mLock);
    set_volume_rpc(SND_DEVICE_CURRENT, SND_METHOD_VOICE, vol);
    return NO_ERROR;
}

status_t AudioHardware::setMasterVolume(float v)
{
    Mutex::Autolock lock(mLock);
    int vol = ceil(v * 7.0);
    LOGI("Set master volume to %d.\n", vol);

    set_volume_rpc(SND_DEVICE_HANDSET, SND_METHOD_VOICE, vol);
    set_volume_rpc(SND_DEVICE_SPEAKER, SND_METHOD_VOICE, vol);
    set_volume_rpc(SND_DEVICE_BT,      SND_METHOD_VOICE, vol);
    set_volume_rpc(SND_DEVICE_HEADSET, SND_METHOD_VOICE, vol);

    // We return an error code here to let the audioflinger do in-software
    // volume on top of the maximum volume that we set through the SND API.
    // return error - software mixer will handle it
    return -1;
}

static status_t do_route_audio_rpc(uint32_t device,
                                   bool ear_mute, bool mic_mute)
{
    LOGV("do_route_audio_rpc(%d, %d, %d)", device, ear_mute, mic_mute);

    if (device == device_list[2].dev_id/*SND_DEVICE_FMRADIO_RX*/)
    {
        LOGV("Going to enable fm radio");
        if(msm_en_device(device, 1)) {
            LOGE("msm_en_device[2],1 failed errno = %d",errno);
            return 0;
        }
        return NO_ERROR;

    }
    if (ear_mute == false) {
        if (VoiceDeviceIsEnabled == false) {
        LOGV("Going to enable RX/TX device for voice stream");
            //Enable RX device
            if(msm_en_device(device_list[0].dev_id, 1)) {
                LOGE("msm_en_device[0],1 failed errno = %d",errno);
                return 0;
            }
            //Enable TX device
            if(msm_en_device(device_list[1].dev_id, 1)) {
                LOGE("msm_en_device[1],1 failed errno = %d",errno);
                return 0;
            }
            //Route voice
            if(msm_route_voice(device_list[0].dev_id,device_list[1].dev_id,1)) {
                LOGE("msm_route_voice failed errno = %d",errno);
                return 0;
            }
            VoiceDeviceIsEnabled = true;
        }
    }
    else if (ear_mute == true) {
        if (VoiceDeviceIsEnabled == true) {
        LOGV("Going to disable RX/TX device during end of voice call");
            //Disable RX device
            if(msm_en_device(device_list[0].dev_id, 0)) {
                LOGE("msm_en_device[0],0 failed errno = %d",errno);
                return 0;
            }
            //Disable TX device
            if(msm_en_device(device_list[1].dev_id, 0)) {
                LOGE("msm_en_device[1],0 failed errno = %d",errno);
                return 0;
            }
            VoiceDeviceIsEnabled = false;
        }
        else {
            //disable fm radio
            LOGV("Going to disable fm radio");
            if(msm_en_device(device_list[2].dev_id, 0)) {
                LOGE("msm_en_device[2],0 failed errno = %d",errno);
                return 0;
            }
        }
    }
    else {
       LOGE("Unknown route combination (%d, %d, %d)\n", device, ear_mute, mic_mute);
       return -EINVAL;

    }
    return NO_ERROR;
}

// always call with mutex held
status_t AudioHardware::doAudioRouteOrMute(uint32_t device)
{
    if (device == (uint32_t)SND_DEVICE_BT || device == (uint32_t)SND_DEVICE_CARKIT) {
        if (mBluetoothId) {
            device = mBluetoothId;
        } else if (!mBluetoothNrec) {
            device = SND_DEVICE_BT_EC_OFF;
        }
    }
    LOGV("doAudioRouteOrMute() device %x, mMode %d, mMicMute %d", device, mMode, mMicMute);
    return do_route_audio_rpc(device,
                              mMode != AudioSystem::MODE_IN_CALL, mMicMute);
}

status_t AudioHardware::doRouting(AudioStreamInMSM72xx *input)
{
    Mutex::Autolock lock(mLock);
    uint32_t outputDevices = mOutput->devices();
    status_t ret = NO_ERROR;
    int audProcess = (ADRC_DISABLE | EQ_DISABLE | RX_IIR_DISABLE);
    int sndDevice = -1;

    if (input != NULL) {
        uint32_t inputDevice = input->devices();
        LOGI("do input routing device %x\n", inputDevice);
        if (inputDevice != 0) {
            if (inputDevice & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
                LOGI("Routing audio to Bluetooth PCM\n");
                sndDevice = SND_DEVICE_BT;
            } else if (inputDevice & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
                if ((outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) &&
                    (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER)) {
                    LOGI("Routing audio to Wired Headset and Speaker\n");
                    sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
                    audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
                } else {
                    LOGI("Routing audio to Wired Headset\n");
                    sndDevice = SND_DEVICE_HEADSET;
                }
            } else {
                if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                    LOGI("Routing audio to Speakerphone\n");
                    sndDevice = SND_DEVICE_SPEAKER;
                    audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
                } else {
                    LOGI("Routing audio to Handset\n");
                    sndDevice = SND_DEVICE_HANDSET;
                }
            }
        }
        // if inputDevice == 0, restore output routing
    }

    if (sndDevice == -1) {
        if (outputDevices & (outputDevices - 1)) {
            if ((outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) == 0) {
                LOGW("Hardware does not support requested route combination (%#X),"
                     " picking closest possible route...", outputDevices);
            }
        }

        if (outputDevices & AudioSystem::DEVICE_OUT_TTY) {
                LOGI("Routing audio to TTY\n");
                sndDevice = SND_DEVICE_TTY_FULL;
        } else if (outputDevices &
                   (AudioSystem::DEVICE_OUT_BLUETOOTH_SCO | AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET)) {
            LOGI("Routing audio to Bluetooth PCM\n");
            sndDevice = SND_DEVICE_BT;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT) {
            LOGI("Routing audio to Bluetooth PCM\n");
            sndDevice = SND_DEVICE_CARKIT;
        } else if ((outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) &&
                   (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER)) {
            LOGI("Routing audio to Wired Headset and Speaker\n");
            sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
            audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
        } else if (outputDevices & AudioSystem::DEVICE_OUT_FM_SPEAKER) {
            LOGI("Routing audio to FM Speakerphone (%d,%x)\n", mMode, outputDevices);
            sndDevice = SND_DEVICE_FM_SPEAKER;
            audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_DISABLE);
        } else if (outputDevices & AudioSystem::DEVICE_OUT_FM_HEADPHONE) {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to FM Headset and Speaker (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
                audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
            } else {
                LOGI("Routing audio to FM Headset (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_FM_HEADSET;
            }
        } else if (outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to No microphone Wired Headset and Speaker (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
                audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
            } else {
                LOGI("Routing audio to No microphone Wired Headset (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_NO_MIC_HEADSET;
            }
        } else if (outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) {
            LOGI("Routing audio to Wired Headset\n");
            sndDevice = SND_DEVICE_HEADSET;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
            LOGI("Routing audio to Speakerphone\n");
            sndDevice = SND_DEVICE_SPEAKER;
            audProcess = (ADRC_ENABLE | EQ_ENABLE | RX_IIR_ENABLE);
        } else if(fmState == true) {
            sndDevice = device_list[2].dev_id;
        } else {
            LOGI("Routing audio to Handset\n");
            //sndDevice = SND_DEVICE_HANDSET;
            sndDevice = device_list[0].dev_id;
        }
    }

    if (sndDevice != -1 && sndDevice != mCurSndDevice) {
        ret = doAudioRouteOrMute(sndDevice);
        mCurSndDevice = sndDevice;
    }

    return ret;
}

status_t AudioHardware::checkMicMute()
{
    Mutex::Autolock lock(mLock);
    if (mMode != AudioSystem::MODE_IN_CALL) {
        setMicMute_nosync(true);
    }

    return NO_ERROR;
}

status_t AudioHardware::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioHardware::dumpInternals\n");
    snprintf(buffer, SIZE, "\tmInit: %s\n", mInit? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmMicMute: %s\n", mMicMute? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothNrec: %s\n", mBluetoothNrec? "true": "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothId: %d\n", mBluetoothId);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioHardware::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    for (size_t index = 0; index < mInputs.size(); index++) {
        mInputs[index]->dump(fd, args);
    }

    if (mOutput) {
        mOutput->dump(fd, args);
    }
    return NO_ERROR;
}

uint32_t AudioHardware::getInputSampleRate(uint32_t sampleRate)
{
    uint32_t i;
    uint32_t prevDelta;
    uint32_t delta;

    for (i = 0, prevDelta = 0xFFFFFFFF; i < sizeof(inputSamplingRates)/sizeof(uint32_t); i++, prevDelta = delta) {
        delta = abs(sampleRate - inputSamplingRates[i]);
        if (delta > prevDelta) break;
    }
    // i is always > 0 here
    return inputSamplingRates[i-1];
}

// ----------------------------------------------------------------------------

AudioHardware::AudioStreamOutMSM72xx::AudioStreamOutMSM72xx() :
    mHardware(0), mFd(-1), mStartCount(0), mRetryCount(0), mStandby(true), mDevices(0)
{
}

status_t AudioHardware::AudioStreamOutMSM72xx::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate)
{
    int lFormat = pFormat ? *pFormat : 0;
    uint32_t lChannels = pChannels ? *pChannels : 0;
    uint32_t lRate = pRate ? *pRate : 0;

    mHardware = hw;

    // fix up defaults
    if (lFormat == 0) lFormat = format();
    if (lChannels == 0) lChannels = channels();
    if (lRate == 0) lRate = sampleRate();

    // check values
    if ((lFormat != format()) ||
        (lChannels != channels()) ||
        (lRate != sampleRate())) {
        if (pFormat) *pFormat = format();
        if (pChannels) *pChannels = channels();
        if (pRate) *pRate = sampleRate();
        return BAD_VALUE;
    }

    if (pFormat) *pFormat = lFormat;
    if (pChannels) *pChannels = lChannels;
    if (pRate) *pRate = lRate;

    mDevices = devices;

    return NO_ERROR;
}

AudioHardware::AudioStreamOutMSM72xx::~AudioStreamOutMSM72xx()
{
    if (mFd >= 0) close(mFd);
}

ssize_t AudioHardware::AudioStreamOutMSM72xx::write(const void* buffer, size_t bytes)
{
    // LOGD("AudioStreamOutMSM72xx::write(%p, %u)", buffer, bytes);
    status_t status = NO_INIT;
    size_t count = bytes;
    const uint8_t* p = static_cast<const uint8_t*>(buffer);

    if (mStandby) {

        // open driver
        LOGV("open driver");
        status = ::open("/dev/msm_pcm_out", O_WRONLY/*O_RDWR*/);
        if (status < 0) {
            LOGE("Cannot open /dev/msm_pcm_out errno: %d", errno);
            goto Error;
        }
        mFd = status;

        // configuration
        LOGV("get config");
        struct msm_audio_config config;
        status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot read config");
            goto Error;
        }

        LOGV("set config");
        config.channel_count = AudioSystem::popCount(channels());
        config.sample_rate = sampleRate();
        config.buffer_size = bufferSize();
        config.buffer_count = AUDIO_HW_NUM_OUT_BUF;
        config.codec_type = CODEC_TYPE_PCM;
        status = ioctl(mFd, AUDIO_SET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot set config");
            goto Error;
        }

        LOGV("buffer_size: %u", config.buffer_size);
        LOGV("buffer_count: %u", config.buffer_count);
        LOGV("channel_count: %u", config.channel_count);
        LOGV("sample_rate: %u", config.sample_rate);

        // fill 2 buffers before AUDIO_START
        mStartCount = AUDIO_HW_NUM_OUT_BUF;
        mStandby = false;
    }

    while (count) {
        ssize_t written = ::write(mFd, p, count);
        if (written >= 0) {
            count -= written;
            p += written;
        } else {
            if (errno != EAGAIN) return written;
            mRetryCount++;
            LOGW("EAGAIN - retry");
        }
    }

    // start audio after we fill 2 buffers
    if (mStartCount) {
        if (--mStartCount == 0) {
            if(ioctl(mFd, AUDIO_GET_SESSION_ID, &dec_id)) {
                LOGE("AUDIO_GET_SESSION_ID failed*********");
                return 0;
            }
	    LOGV("dec_id = %d\n",dec_id);
	    if(msm_en_device(device_list[0].dev_id, 1)) {
	        LOGE("msm_en_device failed");
	        return 0;
	    }
            if(msm_route_stream(1, dec_id, device_list[0].dev_id, 1)) {
                LOGE("msm_route_stream failed");
                return 0;
            }
            ioctl(mFd, AUDIO_START, 0);
        }
    }
    return bytes;

Error:
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    // Simulate audio output timing in case of error
    usleep(bytes * 1000000 / frameSize() / sampleRate());

    return status;
}

status_t AudioHardware::AudioStreamOutMSM72xx::standby()
{
    LOGV("Out::standby()");
    status_t status = NO_ERROR;
    if (!mStandby && mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    LOGV("Deroute pcm stream");
    if(msm_route_stream(1, dec_id, device_list[0].dev_id, 0)) {
        LOGE("could not set stream routing\n");
        return -1;
    }
    // Disable Rx device if only not in voice call
    if (VoiceDeviceIsEnabled == false) {
    // Disable the Eapiece device, if there is nothing to render.
       if(dec_id != 65535) {
         LOGV("Disable device");
         if(msm_en_device(device_list[0].dev_id, 0)) {
             LOGE("could not enable device\n");
             return -1;
         }
         dec_id = 65535;
       }
    }
    mStandby = true;
    return status;
}

status_t AudioHardware::AudioStreamOutMSM72xx::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamOutMSM72xx::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmStartCount: %d\n", mStartCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmStandby: %s\n", mStandby? "true": "false");
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

bool AudioHardware::AudioStreamOutMSM72xx::checkStandby()
{
    return mStandby;
}


status_t AudioHardware::AudioStreamOutMSM72xx::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("AudioStreamOutMSM72xx::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        mDevices = device;
        LOGV("set output routing %x", mDevices);
        status = mHardware->doRouting(NULL);
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardware::AudioStreamOutMSM72xx::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        LOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    LOGV("AudioStreamOutMSM72xx::getParameters() %s", param.toString().string());
    return param.toString();
}


// ----------------------------------------------------------------------------

AudioHardware::AudioStreamInMSM72xx::AudioStreamInMSM72xx() :
    mHardware(0), mFd(-1), mState(AUDIO_INPUT_CLOSED), mRetryCount(0),
    mFormat(AUDIO_HW_IN_FORMAT), mChannels(AUDIO_HW_IN_CHANNELS),
    mSampleRate(AUDIO_HW_IN_SAMPLERATE), mBufferSize(AUDIO_HW_IN_BUFFERSIZE),
    mAcoustics((AudioSystem::audio_in_acoustics)0), mDevices(0)
{
}

status_t AudioHardware::AudioStreamInMSM72xx::set(
        AudioHardware* hw, uint32_t devices, int *pFormat, uint32_t *pChannels, uint32_t *pRate,
        AudioSystem::audio_in_acoustics acoustic_flags)
{
    if ((pFormat == 0) ||
        ((*pFormat != AUDIO_HW_IN_FORMAT) &&
         (*pFormat != AudioSystem::AMR_NB) &&
         (*pFormat != AudioSystem::EVRC) &&
         (*pFormat != AudioSystem::QCELP) &&
         (*pFormat != AudioSystem::AAC)))
    {
        *pFormat = AUDIO_HW_IN_FORMAT;
        return BAD_VALUE;
    }
    if (pRate == 0) {
        return BAD_VALUE;
    }
    uint32_t rate = hw->getInputSampleRate(*pRate);
    if (rate != *pRate) {
        *pRate = rate;
        return BAD_VALUE;
    }

    if (pChannels == 0 || (*pChannels != AudioSystem::CHANNEL_IN_MONO &&
        *pChannels != AudioSystem::CHANNEL_IN_STEREO)) {
        *pChannels = AUDIO_HW_IN_CHANNELS;
        return BAD_VALUE;
    }

    mHardware = hw;

    LOGV("AudioStreamInMSM72xx::set(%d, %d, %u)", *pFormat, *pChannels, *pRate);
    if (mFd >= 0) {
        LOGE("Audio record already open");
        return -EPERM;
    }
    status_t status =0;
    if(*pFormat == AUDIO_HW_IN_FORMAT)
    {
        // open audio input device
        status = ::open("/dev/msm_pcm_in", O_RDWR);
        if (status < 0) {
            LOGE("Cannot open /dev/msm_pcm_in errno: %d", errno);
            goto Error;
        }
        mFd = status;

        // configuration
        LOGV("get config");
        struct msm_audio_config config;
        status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot read config");
            goto Error;
        }

        LOGV("set config");
        config.channel_count = AudioSystem::popCount(*pChannels);
        config.sample_rate = *pRate;
        config.buffer_size = bufferSize();
        config.buffer_count = 2;
        config.codec_type = CODEC_TYPE_PCM;
        status = ioctl(mFd, AUDIO_SET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot set config");
            if (ioctl(mFd, AUDIO_GET_CONFIG, &config) == 0) {
                if (config.channel_count == 1) {
                    *pChannels = AudioSystem::CHANNEL_IN_MONO;
                } else {
                    *pChannels = AudioSystem::CHANNEL_IN_STEREO;
                }
                *pRate = config.sample_rate;
            }
            goto Error;
        }

        LOGV("confirm config");
        status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot read config");
            goto Error;
        }
        LOGV("buffer_size: %u", config.buffer_size);
        LOGV("buffer_count: %u", config.buffer_count);
        LOGV("channel_count: %u", config.channel_count);
        LOGV("sample_rate: %u", config.sample_rate);

        mDevices = devices;
        mFormat = AUDIO_HW_IN_FORMAT;
        mChannels = *pChannels;
        mSampleRate = config.sample_rate;
        mBufferSize = config.buffer_size;
    }
    else if (*pFormat == AudioSystem::EVRC)
    {
        // open evrc input device
          status = ::open(EVRC_DEVICE_IN, O_RDWR);
          if (status < 0) {
              LOGE("Cannot open evrc device for read");
              goto Error;
          }
          mFd = status;

          /* Config param */
          struct msm_audio_stream_config config;
          if(ioctl(mFd, AUDIO_GET_STREAM_CONFIG, &config))
          {
            LOGE(" Error getting buf config param AUDIO_GET_STREAM_CONFIG \n");
            goto  Error;
          }

          LOGE("The Config buffer size is %d", config.buffer_size);
          LOGE("The Config buffer count is %d", config.buffer_count);

          mDevices = devices;
          mChannels = AudioSystem::CHANNEL_IN_MONO;
          mSampleRate =8000;
          mFormat = *pFormat;
          mBufferSize = 1150;
          struct msm_audio_evrc_enc_config evrc_enc_cfg;

          if (ioctl(mFd, AUDIO_GET_EVRC_ENC_CONFIG, &evrc_enc_cfg))
          {
            LOGE("Error: AUDIO_GET_EVRC_ENC_CONFIG failed\n");
            goto  Error;
          }

          LOGV("The Config cdma_rate is %d", evrc_enc_cfg.cdma_rate);
          LOGV("The Config min_bit_rate is %d", evrc_enc_cfg.min_bit_rate);
          LOGV("The Config max_bit_rate is %d", evrc_enc_cfg.max_bit_rate);

          evrc_enc_cfg.min_bit_rate = 4;
          evrc_enc_cfg.max_bit_rate = 4;

          if (ioctl(mFd, AUDIO_SET_EVRC_ENC_CONFIG, &evrc_enc_cfg))
          {
            LOGE("Error: AUDIO_SET_EVRC_ENC_CONFIG failed\n");
            goto  Error;
          }
    }
    else if (*pFormat == AudioSystem::QCELP)
    {
        // open qcelp input device
          status = ::open(QCELP_DEVICE_IN, O_RDWR);
          if (status < 0) {
              LOGE("Cannot open qcelp device for read");
              goto Error;
          }
          mFd = status;

          /* Config param */
          struct msm_audio_stream_config config;
          if(ioctl(mFd, AUDIO_GET_STREAM_CONFIG, &config))
          {
            LOGE(" Error getting buf config param AUDIO_GET_STREAM_CONFIG \n");
            goto  Error;
          }

          LOGE("The Config buffer size is %d", config.buffer_size);
          LOGE("The Config buffer count is %d", config.buffer_count);

          mDevices = devices;
          mChannels = AudioSystem::CHANNEL_IN_MONO;
          mSampleRate =8000;
          mFormat = *pFormat;
          mBufferSize = 1050;

          struct msm_audio_qcelp_enc_config qcelp_enc_cfg;

          if (ioctl(mFd, AUDIO_GET_QCELP_ENC_CONFIG, &qcelp_enc_cfg))
          {
            LOGE("Error: AUDIO_GET_QCELP_ENC_CONFIG failed\n");
            goto  Error;
          }

          LOGV("The Config cdma_rate is %d", qcelp_enc_cfg.cdma_rate);
          LOGV("The Config min_bit_rate is %d", qcelp_enc_cfg.min_bit_rate);
          LOGV("The Config max_bit_rate is %d", qcelp_enc_cfg.max_bit_rate);

          qcelp_enc_cfg.min_bit_rate = 4;
          qcelp_enc_cfg.max_bit_rate = 4;

          if (ioctl(mFd, AUDIO_SET_QCELP_ENC_CONFIG, &qcelp_enc_cfg))
          {
            LOGE("Error: AUDIO_SET_QCELP_ENC_CONFIG failed\n");
            goto  Error;
          }
    }
    else if (*pFormat == AudioSystem::AMR_NB)
    {
        // open amr_nb input device
          status = ::open(AMRNB_DEVICE_IN, O_RDWR);
          if (status < 0) {
              LOGE("Cannot open amr_nb device for read");
              goto Error;
          }
          mFd = status;

          /* Config param */
          struct msm_audio_stream_config config;
          if(ioctl(mFd, AUDIO_GET_STREAM_CONFIG, &config))
          {
            LOGE(" Error getting buf config param AUDIO_GET_STREAM_CONFIG \n");
            goto  Error;
          }

          LOGE("The Config buffer size is %d", config.buffer_size);
          LOGE("The Config buffer count is %d", config.buffer_count);

          mDevices = devices;
          mChannels = AudioSystem::CHANNEL_IN_MONO;
          mSampleRate =8000;
          mFormat = *pFormat;
          mBufferSize = 1280;
          struct msm_audio_amrnb_enc_config_v2 amr_nb_cfg;

          if (ioctl(mFd, AUDIO_GET_AMRNB_ENC_CONFIG_V2, &amr_nb_cfg))
          {
            LOGE("Error: AUDIO_GET_AMRNB_ENC_CONFIG_V2 failed\n");
            goto  Error;
          }

          LOGV("The Config band_mode is %d", amr_nb_cfg.band_mode);
          LOGV("The Config dtx_enable is %d", amr_nb_cfg.dtx_enable);
          LOGV("The Config frame_format is %d", amr_nb_cfg.frame_format);

          amr_nb_cfg.band_mode = 7; /* Bit Rate 12.2 kbps MR122 */
          amr_nb_cfg.dtx_enable= -1;
          amr_nb_cfg.frame_format = 0; /* IF1 */

          if (ioctl(mFd, AUDIO_SET_AMRNB_ENC_CONFIG_V2, &amr_nb_cfg))
          {
            LOGE("Error: AUDIO_SET_AMRNB_ENC_CONFIG_V2 failed\n");
            goto  Error;
          }
    }
    else if (*pFormat == AudioSystem::AAC)
    {
        // open aac input device
          status = ::open(AAC_DEVICE_IN, O_RDWR);
          if (status < 0) {
              LOGE("Cannot open aac device for read");
              goto Error;
          }
          mFd = status;

          struct msm_audio_stream_config config;
          if(ioctl(mFd, AUDIO_GET_STREAM_CONFIG, &config))
          {
            LOGE(" Error getting buf config param AUDIO_GET_STREAM_CONFIG \n");
            goto  Error;
          }

          LOGE("The Config buffer size is %d", config.buffer_size);
          LOGE("The Config buffer count is %d", config.buffer_count);


          struct msm_audio_aac_enc_config aac_enc_cfg;
          if (ioctl(mFd, AUDIO_GET_AAC_ENC_CONFIG, &aac_enc_cfg))
          {
            LOGE("Error: AUDIO_GET_AAC_ENC_CONFIG failed\n");
            goto  Error;
          }

          LOGV("The Config channels is %d", aac_enc_cfg.channels);
          LOGV("The Config sample_rate is %d", aac_enc_cfg.sample_rate);
          LOGV("The Config bit_rate is %d", aac_enc_cfg.bit_rate);
          LOGV("The Config stream_format is %d", aac_enc_cfg.stream_format);

          mDevices = devices;
          mChannels = *pChannels;
          mSampleRate = *pRate;
          mFormat = *pFormat;
          mBufferSize = 2048;

          if (ioctl(mFd, AUDIO_SET_AAC_ENC_CONFIG, &aac_enc_cfg))
          {
            LOGE("Error: AUDIO_SET_AAC_ENC_CONFIG failed\n");
            goto  Error;
          }
    }
    //mHardware->setMicMute_nosync(false);
    mState = AUDIO_INPUT_OPENED;

    if (!acoustic)
        return NO_ERROR;

    audpre_index = calculate_audpre_table_index(mSampleRate);
    tx_iir_index = (audpre_index * 2) + (hw->checkOutputStandby() ? 0 : 1);
    LOGD("audpre_index = %d, tx_iir_index = %d\n", audpre_index, tx_iir_index);

    /**
     * If audio-preprocessing failed, we should not block record.
     */
    int (*msm72xx_set_audpre_params)(int, int);
    msm72xx_set_audpre_params = (int (*)(int, int))::dlsym(acoustic, "msm72xx_set_audpre_params");
    status = msm72xx_set_audpre_params(audpre_index, tx_iir_index);
    if (status < 0)
        LOGE("Cannot set audpre parameters");

    int (*msm72xx_enable_audpre)(int, int, int);
    msm72xx_enable_audpre = (int (*)(int, int, int))::dlsym(acoustic, "msm72xx_enable_audpre");
    mAcoustics = acoustic_flags;
    status = msm72xx_enable_audpre((int)acoustic_flags, audpre_index, tx_iir_index);
    if (status < 0)
        LOGE("Cannot enable audpre");

    return NO_ERROR;

Error:
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    return status;
}

AudioHardware::AudioStreamInMSM72xx::~AudioStreamInMSM72xx()
{
    LOGV("AudioStreamInMSM72xx destructor");
    standby();
}

ssize_t AudioHardware::AudioStreamInMSM72xx::read( void* buffer, ssize_t bytes)
{
    LOGV("AudioStreamInMSM72xx::read(%p, %ld)", buffer, bytes);
    if (!mHardware) return -1;

    size_t count = bytes;
    size_t  aac_framesize= bytes;
    uint8_t* p = static_cast<uint8_t*>(buffer);
    uint32_t* recogPtr = (uint32_t *)p;
    uint16_t* frameCountPtr;
    uint16_t* frameSizePtr;

    if (mState < AUDIO_INPUT_OPENED) {
        AudioHardware *hw = mHardware;
        hw->mLock.lock();
        status_t status = set(hw, mDevices, &mFormat, &mChannels, &mSampleRate, mAcoustics);
        hw->mLock.unlock();
        if (status != NO_ERROR) {
            return -1;
        }
    }
    if(ioctl(mFd, AUDIO_GET_SESSION_ID, &in_dec_id)) {
        LOGE("AUDIO_GET_SESSION_ID failed*********");
        return 0;
    }
    LOGV("in_dec_id = %d\n",in_dec_id);
    if(msm_en_device(device_list[1].dev_id, 1)) {
        LOGE("msm_en_device failed");
        return 0;
    }
    if(msm_route_stream(2, in_dec_id, device_list[1].dev_id, 1)) {
        LOGE("msm_route_stream failed");
        return 0;
    }

    if (mState < AUDIO_INPUT_STARTED) {
        if (ioctl(mFd, AUDIO_START, 0)) {
            LOGE("Error starting record");
            return -1;
        }
        mState = AUDIO_INPUT_STARTED;
    }

    bytes = 0;
    if(mFormat == AUDIO_HW_IN_FORMAT)
    {
        while (count) {
            ssize_t bytesRead = ::read(mFd, buffer, count);
            if (bytesRead >= 0) {
                count -= bytesRead;
                p += bytesRead;
            } else {
                if (errno != EAGAIN) return bytesRead;
                mRetryCount++;
                LOGW("EAGAIN - retrying");
            }
        }
    }
    else if ((mFormat == AudioSystem::EVRC) || (mFormat == AudioSystem::QCELP) || (mFormat == AudioSystem::AMR_NB))
    {
        uint8_t readBuf[36];
        uint8_t *dataPtr;
        while (count) {
            dataPtr = readBuf;
            ssize_t bytesRead = ::read(mFd, readBuf, 36);
            if (bytesRead >= 0) {
                if (mFormat == AudioSystem::AMR_NB){
                   amr_transcode(dataPtr,p);
                   p += AMRNB_FRAME_SIZE;
                   count -= AMRNB_FRAME_SIZE;
                   bytes += AMRNB_FRAME_SIZE;
                }
                else {
                    dataPtr++;
                    if (mFormat == AudioSystem::EVRC){
                       memcpy(p, dataPtr, EVRC_FRAME_SIZE);
                       p += EVRC_FRAME_SIZE;
                       count -= EVRC_FRAME_SIZE;
                       bytes += EVRC_FRAME_SIZE;
                    }
                    else if (mFormat == AudioSystem::QCELP){
                       memcpy(p, dataPtr, QCELP_FRAME_SIZE);
                       p += QCELP_FRAME_SIZE;
                       count -= QCELP_FRAME_SIZE;
                       bytes += QCELP_FRAME_SIZE;
                    }
                }

            } else {
                if (errno != EAGAIN) return bytesRead;
                mRetryCount++;
                LOGW("EAGAIN - retrying");
            }
        }
    }
    else if (mFormat == AudioSystem::AAC)
    {
        *((uint32_t*)recogPtr) = 0x51434F4D ;// ('Q','C','O', 'M') Number to identify format as AAC by higher layers
        recogPtr++;
        frameCountPtr = (uint16_t*)recogPtr;
        *frameCountPtr = 0;
        p += 3*sizeof(uint16_t);
        count -= 3*sizeof(uint16_t);

        while (count > 0) {
            frameSizePtr = (uint16_t *)p;
            p += sizeof(uint16_t);
            if(!(count > 2)) break;
            count -= sizeof(uint16_t);

            ssize_t bytesRead = ::read(mFd, p, count);
            if (bytesRead > 0) {
                LOGV("Number of Bytes read = %d", bytesRead);
                count -= bytesRead;
                p += bytesRead;
                bytes += bytesRead;
                LOGV("Total Number of Bytes read = %d", bytes);

                *frameSizePtr =  bytesRead;
                (*frameCountPtr)++;
            }
            else if(bytesRead == 0)
            {
             LOGI("Bytes Read = %d ,Buffer no longer sufficient",bytesRead);
             break;
            } else {
                if (errno != EAGAIN) return bytesRead;
                mRetryCount++;
                LOGW("EAGAIN - retrying");
            }
        }
    }

    if (mFormat == AudioSystem::AAC)
         return aac_framesize;

        return bytes;
}

status_t AudioHardware::AudioStreamInMSM72xx::standby()
{
    if (!mHardware) return -1;
    if (mState > AUDIO_INPUT_CLOSED) {
        if (mFd >= 0) {
            ::close(mFd);
            mFd = -1;
        }
        //mHardware->checkMicMute();
        mState = AUDIO_INPUT_CLOSED;
    }

    LOGV("Deroute pcm stream");
    if(msm_route_stream(2, in_dec_id, device_list[1].dev_id, 0)) {
        LOGE("could not set stream routing\n");
        return -1;
    }
    // Disable the Eapiece device, if there is nothing to render.
    if(in_dec_id != 65535) {
        LOGV("Disable device");
        if(msm_en_device(device_list[1].dev_id, 0)) {
            LOGE("could not enable device\n");
            return -1;
        }
        in_dec_id = 65535;
    }
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInMSM72xx::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    result.append("AudioStreamInMSM72xx::dump\n");
    snprintf(buffer, SIZE, "\tsample rate: %d\n", sampleRate());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tbuffer size: %d\n", bufferSize());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tchannels: %d\n", channels());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tformat: %d\n", format());
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmHardware: %p\n", mHardware);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmFd count: %d\n", mFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmState: %d\n", mState);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmRetryCount: %d\n", mRetryCount);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioHardware::AudioStreamInMSM72xx::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("AudioStreamInMSM72xx::setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        LOGV("set input routing %x", device);
        if (device & (device - 1)) {
            status = BAD_VALUE;
        } else {
            mDevices = device;
            status = mHardware->doRouting(this);
        }
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardware::AudioStreamInMSM72xx::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        LOGV("get routing %x", mDevices);
        param.addInt(key, (int)mDevices);
    }

    LOGV("AudioStreamInMSM72xx::getParameters() %s", param.toString().string());
    return param.toString();
}

// ----------------------------------------------------------------------------

extern "C" AudioHardwareInterface* createAudioHardware(void) {
    return new AudioHardware();
}

/*===========================================================================

FUNCTION amrsup_frame_len

DESCRIPTION
  This function will determine number of bytes of AMR vocoder frame length
based on the frame type and frame rate.

DEPENDENCIES
  None.

RETURN VALUE
  number of bytes of AMR frame

SIDE EFFECTS
  None.

===========================================================================*/
int amrsup_frame_len_bits(
  amrsup_frame_type frame_type,
  amrsup_mode_type amr_mode
)
{
  int frame_len=0;


  switch (frame_type)
  {
    case AMRSUP_SPEECH_GOOD :
    case AMRSUP_SPEECH_DEGRADED :
    case AMRSUP_ONSET :
    case AMRSUP_SPEECH_BAD :
      if (amr_mode >= AMRSUP_MODE_MAX)
      {
        frame_len = 0;
      }
      else
      {
        frame_len = amrsup_122_framing.len_a
                    + amrsup_122_framing.len_b
                    + amrsup_122_framing.len_c;
      }
      break;

    case AMRSUP_SID_FIRST :
    case AMRSUP_SID_UPDATE :
    case AMRSUP_SID_BAD :
      frame_len = AMR_CLASS_A_BITS_SID;
      break;

    case AMRSUP_NO_DATA :
    case AMRSUP_SPEECH_LOST :
    default :
      frame_len = 0;
  }

  return frame_len;
}

/*===========================================================================

FUNCTION amrsup_frame_len

DESCRIPTION
  This function will determine number of bytes of AMR vocoder frame length
based on the frame type and frame rate.

DEPENDENCIES
  None.

RETURN VALUE
  number of bytes of AMR frame

SIDE EFFECTS
  None.

===========================================================================*/
int amrsup_frame_len(
  amrsup_frame_type frame_type,
  amrsup_mode_type amr_mode
)
{
  int frame_len = amrsup_frame_len_bits(frame_type, amr_mode);

  frame_len = (frame_len + 7) / 8;
  return frame_len;
}

/*===========================================================================

FUNCTION amrsup_tx_order

DESCRIPTION
  Use a bit ordering table to order bits from their original sequence.

DEPENDENCIES
  None.

RETURN VALUE
  None.

SIDE EFFECTS
  None.

===========================================================================*/
void amrsup_tx_order(
  unsigned char *dst_frame,
  int         *dst_bit_index,
  unsigned char *src,
  int         num_bits,
  const unsigned short *order
)
{
  unsigned long dst_mask = 0x00000080 >> ((*dst_bit_index) & 0x7);
  unsigned char *dst = &dst_frame[((unsigned int) *dst_bit_index) >> 3];
  unsigned long src_bit, src_mask;

  /* Prepare to process all bits
  */
  *dst_bit_index += num_bits;
  num_bits++;

  while(--num_bits) {
    /* Get the location of the bit in the input buffer */
    src_bit  = (unsigned long ) *order++;
    src_mask = 0x00000080 >> (src_bit & 0x7);

    /* Set the value of the output bit equal to the input bit */
    if (src[src_bit >> 3] & src_mask) {
      *dst |= (unsigned char ) dst_mask;
    }

    /* Set the destination bit mask and increment pointer if necessary */
    dst_mask >>= 1;
    if (dst_mask == 0) {
      dst_mask = 0x00000080;
      dst++;
    }
  }
} /* amrsup_tx_order */

/*===========================================================================

FUNCTION amrsup_if1_framing

DESCRIPTION
  Performs the transmit side framing function.  Generates AMR IF1 ordered data
  from the vocoder packet and frame type.

DEPENDENCIES
  None.

RETURN VALUE
  number of bytes of encoded frame.
  if1_frame : IF1-encoded frame.
  if1_frame_info : holds frame information of IF1-encoded frame.

SIDE EFFECTS
  None.

===========================================================================*/
static int amrsup_if1_framing(
  unsigned char              *vocoder_packet,
  amrsup_frame_type          frame_type,
  amrsup_mode_type           amr_mode,
  unsigned char              *if1_frame,
  amrsup_if1_frame_info_type *if1_frame_info
)
{
  amrsup_frame_order_type *ordering_table;
  int frame_len = 0;
  int i;

  if(amr_mode >= AMRSUP_MODE_MAX)
  {
    LOGE("Invalid AMR_Mode : %d",amr_mode);
    return 0;
  }

  /* Initialize IF1 frame data and info */
  if1_frame_info->fqi = true;

  if1_frame_info->amr_type = AMRSUP_CODEC_AMR_NB;

  memset(if1_frame, 0,
           amrsup_frame_len(AMRSUP_SPEECH_GOOD, AMRSUP_MODE_1220));


  switch (frame_type)
  {
    case AMRSUP_SID_BAD:
      if1_frame_info->fqi = false;
      /* fall thru */

    case AMRSUP_SID_FIRST:
    case AMRSUP_SID_UPDATE:
      /* Set frame type index */
      if1_frame_info->frame_type_index
      = AMRSUP_FRAME_TYPE_INDEX_AMR_SID;


      /* ===== Encoding SID frame ===== */
      /* copy the sid frame to class_a data */
      for (i=0; i<5; i++)
      {
        if1_frame[i] = vocoder_packet[i];
      }

      /* Set the SID type : SID_FIRST: Bit 35 = 0, SID_UPDATE : Bit 35 = 1 */
      if (frame_type == AMRSUP_SID_FIRST)
      {
        if1_frame[4] &= ~0x10;
      }

      if (frame_type == AMRSUP_SID_UPDATE)
      {
        if1_frame[4] |= 0x10;
      }
      else
      {
      /* Set the mode (Bit 36 - 38 = amr_mode with bits swapped)
      */
      if1_frame[4] |= (((unsigned char)amr_mode << 3) & 0x08)
        | (((unsigned char)amr_mode << 1) & 0x04) | (((unsigned char)amr_mode >> 1) & 0x02);

      frame_len = AMR_CLASS_A_BITS_SID;
      }

      break;


    case AMRSUP_SPEECH_BAD:
      if1_frame_info->fqi = false;
      /* fall thru */

    case AMRSUP_SPEECH_GOOD:
      /* Set frame type index */

        if1_frame_info->frame_type_index
        = (amrsup_frame_type_index_type)(amr_mode);

      /* ===== Encoding Speech frame ===== */
      /* Clear num bits in frame */
      frame_len = 0;

      /* Select ordering table */
      ordering_table =
      (amrsup_frame_order_type*)&amrsup_122_framing;

      amrsup_tx_order(
        if1_frame,
        &frame_len,
        vocoder_packet,
        ordering_table->len_a,
        ordering_table->class_a
      );

      amrsup_tx_order(
        if1_frame,
        &frame_len,
        vocoder_packet,
        ordering_table->len_b,
        ordering_table->class_b
      );

      amrsup_tx_order(
        if1_frame,
        &frame_len,
        vocoder_packet,
        ordering_table->len_c,
        ordering_table->class_c
      );


      /* frame_len already updated with correct number of bits */
      break;



    default:
      LOGE("Unsupported frame type %d", frame_type);
      /* fall thru */

    case AMRSUP_NO_DATA:
      /* Set frame type index */
      if1_frame_info->frame_type_index = AMRSUP_FRAME_TYPE_INDEX_NO_DATA;

      frame_len = 0;

      break;
  }  /* end switch */


  /* convert bit length to byte length */
  frame_len = (frame_len + 7) / 8;

  return frame_len;
}

static void amr_transcode(unsigned char *src, unsigned char *dst)
{
   amrsup_frame_type frame_type_in = (amrsup_frame_type) *(src++);
   amrsup_mode_type frame_rate_in = (amrsup_mode_type) *(src++);
   amrsup_if1_frame_info_type frame_info_out;
   unsigned char frameheader;

   amrsup_if1_framing(src, frame_type_in, frame_rate_in, dst+1, &frame_info_out);
   frameheader = (frame_info_out.frame_type_index << 3) + (frame_info_out.fqi << 2);
   *dst = frameheader;

   return;
}

}; // namespace android
