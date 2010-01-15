/*
** Copyright 2008, Google Inc.
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
#define LOG_TAG "AudioHardwareQSD"
#include <utils/Log.h>
#include <utils/String8.h>
#include <hardware_legacy/power.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>

#include <cutils/properties.h> // for property_get for the voice recognition mode switch

// hardware specific functions

#include "AudioHardware.h"
#include <media/AudioRecord.h>

#define DUALMIC_KEY "dualmic_enabled"
#define TTY_MODE_KEY "tty_mode"

#define LOG_SND_RPC 0  // Set to 1 to log sound RPC's
#define TX_PATH (1)
#define EVRC_DEVICE_IN "/dev/msm_evrc_in"
#define QCELP_DEVICE_IN "/dev/msm_qcelp_in"
#define AAC_DEVICE_IN "/dev/msm_aac_in"
#define EVRC_FRAME_SIZE 23
#define QCELP_FRAME_SIZE 35

static const uint32_t SND_DEVICE_CURRENT = 256;
static const uint32_t SND_DEVICE_HANDSET = 0;
static const uint32_t SND_DEVICE_SPEAKER = 1;
static const uint32_t SND_DEVICE_BT = 3;
static const uint32_t SND_DEVICE_CARKIT = 4;
static const uint32_t SND_DEVICE_BT_EC_OFF = 45;
static const uint32_t SND_DEVICE_HEADSET = 2;
static const uint32_t SND_DEVICE_HEADSET_AND_SPEAKER = 10;
static const uint32_t SND_DEVICE_FM_HEADSET = 9;
static const uint32_t SND_DEVICE_FM_SPEAKER = 11;
static const uint32_t SND_DEVICE_NO_MIC_HEADSET = 8;
static const uint32_t SND_DEVICE_TTY_FULL = 5;
static const uint32_t SND_DEVICE_TTY_VCO = 6;
static const uint32_t SND_DEVICE_TTY_HCO = 7;
static const uint32_t SND_DEVICE_HANDSET_BACK_MIC = 20;
static const uint32_t SND_DEVICE_SPEAKER_BACK_MIC = 21;
static const uint32_t SND_DEVICE_NO_MIC_HEADSET_BACK_MIC = 28;
static const uint32_t SND_DEVICE_HANDSET_DUAL_MIC = 30;
static const uint32_t SND_DEVICE_SPEAKER_DUAL_MIC = 31;
namespace android {
static int old_pathid = -1;
static int new_pathid = -1;
static int curr_out_device = -1;
static int curr_mic_device = -1;
static int voice_started = 0;
static int fd_fm_device = -1;
static int stream_volume = -300;

int errCount = 0;
const uint32_t AudioHardware::inputSamplingRates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

// ID string for audio wakelock
static const char kOutputWakelockStr[] = "AudioHardwareQSD";

// ----------------------------------------------------------------------------

AudioHardware::AudioHardware() :
    mInit(false), mMicMute(true),
    mBluetoothNrec(true), mBluetoothIdTx(0),
    mBluetoothIdRx(0), mOutput(0),
    mDualMicEnabled(false), mTtyMode(TTY_OFF)
{
    // reset voice mode in case media_server crashed and restarted while in call
    int fd = open("/dev/msm_audio_ctl", O_RDWR);
    if (fd >= 0) {
        ioctl(fd, AUDIO_STOP_VOICE, NULL);
        close(fd);
    }
    mInit = true;

}
AudioHardware::~AudioHardware()
{
    for (size_t index = 0; index < mInputs.size(); index++) {
        closeInputStream((AudioStreamIn*)mInputs[index]);
    }
    mInputs.clear();
    closeOutputStream((AudioStreamOut*)mOutput);
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

    // This is to ensure that the concurrency scenario is handled. Right now on
    // 8K voice call and pcm playback concurrency is not supported.
    if (mOutput)
    {
        mOutput->setPhonestate(mode);
        mOutput->closedecoder();
    }

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
static status_t set_mic_mute(bool _mute)
{
    uint32_t mute = _mute;
    int fd = -1;
    fd = open("/dev/msm_audio_ctl", O_RDWR);
    if (fd < 0) {
        LOGE("Cannot open msm_audio_ctl device\n");
        return -1;
    }
    LOGD("Setting mic mute to %d\n", mute);
    if (ioctl(fd, AUDIO_SET_MUTE, &mute)) {
        LOGE("Cannot set mic mute on current device\n");
        close(fd);
        return -1;
    }
    close(fd);
    return NO_ERROR;
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
        return set_mic_mute(mMicMute); //always set current TX device
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
        mBluetoothIdTx = 0;
        mBluetoothIdRx = 0;
        for (int i = 0; i < mNumBTEndpoints; i++) {
            if (!strcasecmp(value.string(), mBTEndpoints[i].name)) {
                mBluetoothIdTx = mBTEndpoints[i].tx;
                mBluetoothIdRx = mBTEndpoints[i].rx;
                LOGI("Using custom acoustic parameters for %s", value.string());
                break;
            }
        }
        if (mBluetoothIdTx == 0) {
            LOGI("Using default acoustic parameters "
                 "(%s not in acoustic database)", value.string());
        }
        doRouting(NULL);
    }

    key = String8(DUALMIC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            mDualMicEnabled = true;
            LOGI("DualMike feature Enabled");
        } else {
            mDualMicEnabled = false;
            LOGI("DualMike feature Disabled");
        }
        doRouting(NULL);
    }

    key = String8(TTY_MODE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "full") {
            mTtyMode = TTY_FULL;
        } else if (value == "hco") {
            mTtyMode = TTY_HCO;
        } else if (value == "vco") {
            mTtyMode = TTY_VCO;
        } else {
            mTtyMode = TTY_OFF;
        }
        doRouting(NULL);
    }

    return NO_ERROR;
}

String8 AudioHardware::getParameters(const String8& keys)
{
    AudioParameter request = AudioParameter(keys);
    AudioParameter reply = AudioParameter();
    String8 value;
    String8 key = String8(DUALMIC_KEY);

    if (request.get(key, value) == NO_ERROR) {
        value = String8(mDualMicEnabled ? "true" : "false");
        reply.add(key, value);
    }

    LOGV("getParameters() %s", keys.string());

    return reply.toString();
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
        (format != AudioSystem::EVRC) &&
         (format != AudioSystem::QCELP) &&
         (format != AudioSystem::AAC)){
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if (channelCount < 1 || channelCount > 2) {
        LOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }
    if (format == AudioSystem::EVRC)
       return 1150*channelCount;
    else if (format == AudioSystem::QCELP)
       return 1050*channelCount;
    else if (format == AudioSystem::AAC)
       return 2048;
    else
    return AUDIO_HW_IN_BUFFERSIZE*channelCount;
}

static status_t set_volume_rpc(uint32_t volume)
{
    int fd = -1;
    fd = open("/dev/msm_audio_ctl", O_RDWR);
    if (fd < 0) {
        LOGE("Cannot open msm_audio_ctl device\n");
        return -1;
    }
    volume *= 20; //percentage
    LOGD("Setting in-call volume to %d\n", volume);
    if (ioctl(fd, AUDIO_SET_VOLUME, &volume)) {
        LOGW("Cannot set volume on current device\n");
    }
    close(fd);
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

    int vol = lrint(v * 5.0);
    LOGD("setVoiceVolume(%f)\n", v);
    LOGI("Setting in-call volume to %d (available range is 0 to 5)\n", vol);

    Mutex::Autolock lock(mLock);
    set_volume_rpc(vol); //always set current device
    return NO_ERROR;
}

status_t AudioHardware::setMasterVolume(float v)
{
    Mutex::Autolock lock(mLock);
    int vol = ceil(v * 5.0);
    LOGI("Set master volume to %d.\n", vol);
    // We return an error code here to let the audioflinger do in-software
    // volume on top of the maximum volume that we set through the SND API.
    // return error - software mixer will handle it
    return -1;
}

static status_t do_route_audio_dev_ctrl(uint32_t device, bool inCall)
{
    int out_device = 0, mic_device = 0;
    int fd = 0;

    if (device == SND_DEVICE_CURRENT)
        goto Incall;

    // hack -- kernel needs to put these in include file
    LOGD("Switching audio device to ");
    if (device == SND_DEVICE_HANDSET) {
           out_device = HANDSET_SPKR;
           mic_device = HANDSET_MIC;
           LOGD("Handset");
    } else if ((device  == SND_DEVICE_BT) || (device == SND_DEVICE_BT_EC_OFF)) {
           out_device = BT_SCO_SPKR;
           mic_device = BT_SCO_MIC;
           LOGD("BT Headset");
    } else if (device == SND_DEVICE_SPEAKER ||
               device == SND_DEVICE_SPEAKER_BACK_MIC) {
           out_device = SPKR_PHONE_MONO;
           mic_device = SPKR_PHONE_MIC;
           LOGD("Speakerphone");
    } else if (device == SND_DEVICE_HEADSET) {
           out_device = HEADSET_SPKR_STEREO;
           mic_device = HEADSET_MIC;
           LOGD("Stereo Headset");
    } else if (device == SND_DEVICE_HEADSET_AND_SPEAKER) {
           out_device = SPKR_PHONE_HEADSET_STEREO;
           mic_device = HEADSET_MIC;
           LOGD("Stereo Headset + Speaker");
    } else if (device == SND_DEVICE_NO_MIC_HEADSET) {
           out_device = HEADSET_SPKR_STEREO;
           mic_device = HANDSET_MIC;
           LOGD("No microphone Wired Headset");
    } else if (device == SND_DEVICE_NO_MIC_HEADSET_BACK_MIC) {
           out_device = HEADSET_SPKR_STEREO;
           mic_device = SPKR_PHONE_MIC;
           LOGD("No microphone Wired Headset and back mic");
    } else if (device == SND_DEVICE_HANDSET_BACK_MIC) {
           out_device = HANDSET_SPKR;
           mic_device = SPKR_PHONE_MIC;
           LOGD("Handset and back mic");
    } else if (device == SND_DEVICE_FM_HEADSET) {
           out_device = FM_HEADSET;
           mic_device = HEADSET_MIC;
           LOGD("Stereo FM headset");
    } else if (device == SND_DEVICE_FM_SPEAKER) {
           out_device = FM_SPKR;
           mic_device = HEADSET_MIC;
           LOGD("Stereo FM speaker");
    } else if (device == SND_DEVICE_TTY_FULL) {
           out_device = TTY_HEADSET_SPKR;
           mic_device = TTY_HEADSET_MIC;
           LOGD("TTY headset in FULL mode\n");
    } else if (device == SND_DEVICE_TTY_VCO) {
           out_device = TTY_HEADSET_SPKR;
           mic_device = HANDSET_MIC;
           LOGD("TTY headset in VCO mode\n");
    } else if (device == SND_DEVICE_TTY_HCO) {
           out_device = HANDSET_SPKR;
           mic_device = TTY_HEADSET_MIC;
           LOGD("TTY headset in HCO mode\n");
    } else if (device == SND_DEVICE_HANDSET_DUAL_MIC) {
           out_device = HANDSET_SPKR;
           mic_device = HANDSET_DUALMIC;
           LOGD("Handset with DualMike");
    } else if (device == SND_DEVICE_SPEAKER_DUAL_MIC) {
           out_device = SPKR_PHONE_MONO;
           mic_device = SPKR_DUALMIC;
           LOGD("Speakerphone with DualMike");
    } else {
           LOGE("unknown device %d", device);
           return -1;
    }

    fd = open("/dev/msm_audio_ctl", O_RDWR);
    if (fd < 0)        {
       LOGE("Cannot open msm_audio_ctl");
       return -1;
    }
    if (ioctl(fd, AUDIO_SWITCH_DEVICE, &out_device)) {
       LOGE("Cannot switch audio device");
       close(fd);
       return -1;
    }
    if (ioctl(fd, AUDIO_SWITCH_DEVICE, &mic_device)) {
       LOGE("Cannot switch mic device");
       close(fd);
       return -1;
    }
    curr_out_device = out_device;
    curr_mic_device = mic_device;

Incall:
    if (inCall == true && !voice_started) {
	if (fd < 0) {
            fd = open("/dev/msm_audio_ctl", O_RDWR);

            if (fd < 0) {
                LOGE("Cannot open msm_audio_ctl");
                return -1;
            }
        }
        if (ioctl(fd, AUDIO_START_VOICE, NULL)) {
               LOGE("Cannot start voice");
               close(fd);
               return -1;
        }
        LOGD("Voice Started!!");
        voice_started = 1;
    }
    else if (inCall == false && voice_started) {
        if (fd < 0) {
            fd = open("/dev/msm_audio_ctl", O_RDWR);

            if (fd < 0) {
                LOGE("Cannot open msm_audio_ctl");
                return -1;
            }
        }
        if (ioctl(fd, AUDIO_STOP_VOICE, NULL)) {
               LOGE("Cannot stop voice");
               close(fd);
               return -1;
        }
        LOGD("Voice Stopped!!");
        voice_started = 0;
    }

    close(fd);
    return NO_ERROR;
}


// always call with mutex held
status_t AudioHardware::doAudioRouteOrMute(uint32_t device)
{
    if (device == (uint32_t)SND_DEVICE_BT || device == (uint32_t)SND_DEVICE_CARKIT) {
        if (!mBluetoothNrec) {
            device = SND_DEVICE_BT_EC_OFF;
        }
    }
    return do_route_audio_dev_ctrl(device, mMode == AudioSystem::MODE_IN_CALL);
}

status_t AudioHardware::get_mMode(void)
{
    return mMode;
}

status_t AudioHardware::get_mRoutes(void)
{
    return mRoutes[mMode];
}

status_t AudioHardware::set_mRecordState(bool onoff)
{
    mRecordState = onoff;
    return 0;
}

status_t AudioHardware::get_snd_dev(void)
{
    Mutex::Autolock lock(mLock);
    return mCurSndDevice;
}

// always call with mutex held
status_t AudioHardware::updateACDB(void)
{

    int fd = 0;
    int acdb_id = -1;
    uint32_t id[2];

    fd = open("/dev/msm_audio_ctl", O_RDWR);
    if (fd < 0)        {
       LOGE("Cannot open msm_audio_ctl");
       return -1;
    }

    if (mMode == AudioSystem::MODE_IN_CALL) {
        LOGD("skip update ACDB due to in-call");
        close(fd);
        return 0;
    }

    //update RX acdb parameters.
    if (!checkOutputStandby()) {
        switch (mCurSndDevice) {
            case SND_DEVICE_HEADSET:
            case SND_DEVICE_NO_MIC_HEADSET:
            case SND_DEVICE_NO_MIC_HEADSET_BACK_MIC:
            case SND_DEVICE_FM_HEADSET:
                acdb_id = ACDB_ID_HEADSET_PLAYBACK;
                break;
            case SND_DEVICE_SPEAKER:
            case SND_DEVICE_FM_SPEAKER:
            case SND_DEVICE_SPEAKER_BACK_MIC:
                acdb_id = ACDB_ID_SPKR_PLAYBACK;
                break;
            case SND_DEVICE_HEADSET_AND_SPEAKER:
                acdb_id = ACDB_ID_HEADSET_RINGTONE_PLAYBACK;
                break;
            default:
                break;
        }
    }
    if (acdb_id != -1) {
        id[0] = acdb_id;
        id[1] = curr_out_device;
        if (ioctl(fd, AUDIO_UPDATE_ACDB, &id)) {
           LOGE("Cannot update RX ACDB %d", acdb_id);
           close(fd);
           return -1;
        } else
           LOGD("update RX ACDB %d success", acdb_id);
    }

    //update TX acdb parameters.
    acdb_id = -1;
    if (mRecordState) {
        switch (mCurSndDevice) {
            case SND_DEVICE_HEADSET:
            case SND_DEVICE_FM_HEADSET:
            case SND_DEVICE_FM_SPEAKER:
                acdb_id = ACDB_ID_EXT_MIC_REC;
                break;
            case SND_DEVICE_HANDSET:
            case SND_DEVICE_NO_MIC_HEADSET:
            case SND_DEVICE_SPEAKER:
                /*if (vr_mode_enabled == 0) {
                    acdb_id = ACDB_ID_INT_MIC_REC;
                } else {
                    acdb_id = ACDB_ID_INT_MIC_VR;
                }
                break;*/
            case SND_DEVICE_SPEAKER_BACK_MIC:
            case SND_DEVICE_NO_MIC_HEADSET_BACK_MIC:
            case SND_DEVICE_HANDSET_BACK_MIC:
                acdb_id = ACDB_ID_CAMCORDER;
                break;
            default:
                break;
        }
    }
    if (acdb_id != -1) {
        id[0] = acdb_id;
        id[1] = curr_mic_device;
        if (ioctl(fd, AUDIO_UPDATE_ACDB, &id)) {
           LOGE("Cannot update TX ACDB %d", acdb_id);
           close(fd);
           return -1;
        } else
           LOGD("update TX ACDB %d success", acdb_id);
    }
    close(fd);
    return 0;
}

status_t AudioHardware::doRouting(AudioStreamInMSM72xx *input)
{
    Mutex::Autolock lock(mLock);
    uint32_t outputDevices = mOutput->devices();
    status_t ret = NO_ERROR;
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
                } else {
                    LOGI("Routing audio to Wired Headset\n");
                    sndDevice = SND_DEVICE_HEADSET;
                }
            } else if (inputDevice & AudioSystem::DEVICE_IN_BACK_MIC) {
                if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                    LOGI("Routing audio to Speakerphone with back mic\n");
                    sndDevice = SND_DEVICE_SPEAKER_BACK_MIC;
                } else if (outputDevices == AudioSystem::DEVICE_OUT_EARPIECE) {
                    LOGI("Routing audio to Handset with back mic\n");
                    sndDevice = SND_DEVICE_HANDSET_BACK_MIC;
                } else {
                    LOGI("Routing audio to Headset with back mic\n");
                    sndDevice = SND_DEVICE_NO_MIC_HEADSET_BACK_MIC;
                }
            } else {
                if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                    LOGI("Routing audio to Speakerphone\n");
                    sndDevice = SND_DEVICE_SPEAKER;
                } else if (outputDevices == AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
                    LOGI("Routing audio to Speakerphone\n");
                    sndDevice = SND_DEVICE_NO_MIC_HEADSET;
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

        if ((mTtyMode != TTY_OFF) && (mMode == AudioSystem::MODE_IN_CALL) &&
                (outputDevices & (AudioSystem::DEVICE_OUT_TTY | AudioSystem::DEVICE_OUT_WIRED_HEADSET))) {
            if (mTtyMode == TTY_FULL) {
                LOGI("Routing audio to TTY FULL Mode\n");
                sndDevice = SND_DEVICE_TTY_FULL;
            } else if (mTtyMode == TTY_VCO) {
                LOGI("Routing audio to TTY VCO Mode\n");
                sndDevice = SND_DEVICE_TTY_VCO;
            } else if (mTtyMode == TTY_HCO) {
                LOGI("Routing audio to TTY HCO Mode\n");
                sndDevice = SND_DEVICE_TTY_HCO;
            }
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
        } else if (outputDevices & AudioSystem::DEVICE_OUT_FM_SPEAKER) {
            LOGI("Routing audio to FM Speakerphone (%d,%x)\n", mMode, outputDevices);
            sndDevice = SND_DEVICE_FM_SPEAKER;
        } else if (outputDevices & AudioSystem::DEVICE_OUT_FM_HEADPHONE) {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to FM Headset and Speaker (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
            } else {
                LOGI("Routing audio to FM Headset (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_FM_HEADSET;
            }
        } else if (outputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            if (outputDevices & AudioSystem::DEVICE_OUT_SPEAKER) {
                LOGI("Routing audio to No microphone Wired Headset and Speaker (%d,%x)\n", mMode, outputDevices);
                sndDevice = SND_DEVICE_HEADSET_AND_SPEAKER;
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
        } else {
            LOGI("Routing audio to Handset\n");
            sndDevice = SND_DEVICE_HANDSET;
        }
    }

    if (mDualMicEnabled && mMode == AudioSystem::MODE_IN_CALL) {
        if (sndDevice == SND_DEVICE_HANDSET) {
            LOGI("Routing audio to handset with DualMike enabled\n");
            sndDevice = SND_DEVICE_HANDSET_DUAL_MIC;
        } else if (sndDevice == SND_DEVICE_SPEAKER) {
            LOGI("Routing audio to speakerphone with DualMike enabled\n");
            sndDevice = SND_DEVICE_SPEAKER_DUAL_MIC;
        }
    }

    if ((sndDevice != -1 && sndDevice != mCurSndDevice)) {
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
    snprintf(buffer, SIZE, "\tmBluetoothIdtx: %d\n", mBluetoothIdTx);
    result.append(buffer);
    snprintf(buffer, SIZE, "\tmBluetoothIdrx: %d\n", mBluetoothIdRx);
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
        LOGV("open pcm_out driver");
        status = ::open("/dev/msm_pcm_out", O_RDWR);
        if (status < 0) {
            if (errCount++ < 10) {
                LOGE("Cannot open /dev/msm_pcm_out errno: %d", errno);
            }
            goto Error;
        }
        mFd = status;

        // configuration
        LOGV("get config");
        struct msm_audio_config config;
        status = ioctl(mFd, AUDIO_GET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot read pcm_out config");
            goto Error;
        }

        LOGV("set pcm_out config");
        config.channel_count = AudioSystem::popCount(channels());
        config.sample_rate = sampleRate();
        config.buffer_size = bufferSize();
        config.buffer_count = AUDIO_HW_NUM_OUT_BUF;
        config.type = CODEC_TYPE_PCM;
        status = ioctl(mFd, AUDIO_SET_CONFIG, &config);
        if (status < 0) {
            LOGE("Cannot set config");
            goto Error;
        }

        LOGV("buffer_size: %u", config.buffer_size);
        LOGV("buffer_count: %u", config.buffer_count);
        LOGV("channel_count: %u", config.channel_count);
        LOGV("sample_rate: %u", config.sample_rate);

        status = ioctl(mFd, AUDIO_START, 0);
        if (status < 0) {
            LOGE("Cannot start pcm playback");
            goto Error;
        }

        LOGV("acquire wakelock");
        acquire_wake_lock(PARTIAL_WAKE_LOCK, kOutputWakelockStr);
        mStandby = false;
        Mutex::Autolock lock(mHardware->mLock);
        mHardware->updateACDB();
    }

    if (mPhonestate == AudioSystem::MODE_IN_CALL)
    {
        if (mFd >= 0)
        {
            ::close(mFd);
            mFd = -1;
        }

        return bytes;
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
    status_t status = NO_ERROR;
    if (!mStandby && mFd >= 0) {
        ::close(mFd);
        mFd = -1;
        LOGV("release wakelock");
        release_wake_lock(kOutputWakelockStr);
    }
    mStandby = true;
    LOGI("AudioHardware pcm playback is going to standby.");
    return status;
}

void AudioHardware::AudioStreamOutMSM72xx::closedecoder()
{
    if ( (mFd >=0) && (mPhonestate == AudioSystem::MODE_IN_CALL) )
    {
        ::close(mFd);
        mFd = -1;
        LOGE("Close PCM decoder, since Voice call is about to get active");
    }
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
    status_t status = NO_ERROR;

    if ((pFormat == 0) ||
        ((*pFormat != AUDIO_HW_IN_FORMAT) &&
        (*pFormat != AudioSystem::EVRC) &&
        (*pFormat != AudioSystem::QCELP) &&
        (*pFormat != AudioSystem::AAC)))
    {
        *pFormat = AUDIO_HW_IN_FORMAT;
        return BAD_VALUE;
    }

    if((*pFormat == AudioSystem::AAC) && (*pChannels & (AudioSystem::CHANNEL_IN_VOICE_DNLINK |  AudioSystem::CHANNEL_IN_VOICE_UPLINK))) {
        LOGE("voice call recording in AAC format does not support");
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

    if (pChannels == 0 || (*pChannels & (AudioSystem::CHANNEL_IN_MONO | AudioSystem::CHANNEL_IN_STEREO)) == 0) {
        *pChannels = AUDIO_HW_IN_CHANNELS;
        return BAD_VALUE;
    }

    mHardware = hw;

    LOGV("AudioStreamInMSM72xx::set(%d, %x, %u)", *pFormat, *pChannels, *pRate);
    if (mFd >= 0) {
        LOGE("Audio record already open");
        return -EPERM;
    }
    if(*pFormat == AUDIO_HW_IN_FORMAT)
    {
    // open audio input device
    status_t status = ::open("/dev/msm_pcm_in", O_RDWR);
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
    config.type = CODEC_TYPE_PCM;
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
          LOGI("Recording format: EVRC");
          // open evrc input device
          status = ::open(EVRC_DEVICE_IN, O_RDWR);
          if (status < 0) {
              LOGE("Cannot open evrc device for read");
              goto Error;
          }
          mFd = status;
          mDevices = devices;
          mChannels = *pChannels;
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

          evrc_enc_cfg.min_bit_rate = 1; //CDMA Eighth rate
          evrc_enc_cfg.max_bit_rate = 4; //CDMA Full rate

          if (ioctl(mFd, AUDIO_SET_EVRC_ENC_CONFIG, &evrc_enc_cfg))
          {
            LOGE("Error: AUDIO_SET_EVRC_ENC_CONFIG failed\n");
            goto  Error;
          }
    }
    else if (*pFormat == AudioSystem::QCELP)
    {
          LOGI("Recording format: QCELP");
          // open qcelp input device
          status = ::open(QCELP_DEVICE_IN, O_RDWR);
          if (status < 0) {
              LOGE("Cannot open qcelp device for read");
              goto Error;
          }
          mFd = status;
          mDevices = devices;
          mChannels = *pChannels;
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

          qcelp_enc_cfg.min_bit_rate = 1; //CDMA Eighth rate
          qcelp_enc_cfg.max_bit_rate = 4; //CDMA Full rate

          if (ioctl(mFd, AUDIO_SET_QCELP_ENC_CONFIG, &qcelp_enc_cfg))
          {
            LOGE("Error: AUDIO_SET_QCELP_ENC_CONFIG failed\n");
            goto  Error;
          }
    }
    else if(*pFormat == AudioSystem::AAC)
    {
        // open AAC input device
        status = ::open(AAC_DEVICE_IN, O_RDWR);
            if (status < 0) {
                LOGE("Cannot open AAC input  device for read");
                goto Error;
            }
            mFd = status;

         /* Config param */
         struct msm_audio_aac_enc_config config;
         if(ioctl(mFd, AUDIO_GET_AAC_ENC_CONFIG, &config))
         {
                LOGE(" Error getting buf config param AUDIO_GET_CONFIG \n");
                goto  Error;
         }

        LOGV("The Config Sample rate is %d", config.sample_rate);

        mDevices = devices;
        mChannels = *pChannels;
        mSampleRate = *pRate;
        mBufferSize = 2048;
        mFormat = *pFormat;

        config.sample_rate = *pRate;
        config.stream_format = AUDIO_AAC_FORMAT_RAW;

        if (ioctl(mFd, AUDIO_SET_AAC_ENC_CONFIG, &config)) {
            LOGE(" Error in setting config of msm_pcm_in device \n");
			goto Error;
        }
    }

    //mHardware->setMicMute_nosync(false);
    mState = AUDIO_INPUT_OPENED;

    // Need to start AUDIO before close is called, even if we are not ready to
    // read. This is the due to our hardware requirement (Cannot Open -> Close,
    // have to follow Open -> Start -> Close).
    mHardware->set_mRecordState(1);
    if (ioctl(mFd, AUDIO_START, 0)) {
        LOGE("Error starting record");
        return -1;
    }

    mState = AUDIO_INPUT_STARTED;
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

    if (mState < AUDIO_INPUT_STARTED) {
        mHardware->set_mRecordState(1);
        if (ioctl(mFd, AUDIO_START, 0)) {
            LOGE("Error starting record");
            return -1;
        }
        LOGI("AUDIO_START: start kernel pcm_in driver.");
        mState = AUDIO_INPUT_STARTED;
        Mutex::Autolock lock(mHardware->mLock);
        mHardware->updateACDB();
    }

    if (mFormat == AudioSystem::AAC) {
       if (bytes < 512) {
        LOGE("Error, the buffer size passed is not compatible %d", bytes);
        return -1;
       }
    }
    bytes = 0;
    if (mFormat == AudioSystem::AAC)
    {
        *((uint32_t*)recogPtr) = 0x51434F4D ;// ('Q','C','O', 'M') Number to identify format as AAC by higher layers
        recogPtr++;
        frameCountPtr = (uint16_t*)recogPtr;
        *frameCountPtr = 0;
        p += 3*sizeof(uint16_t);
        count -= 3*sizeof(uint16_t);
    }
    if (mFormat == AudioSystem::AAC)
    {
	while (count > 0) {
          frameSizePtr = (uint16_t *)p;
          p += sizeof(uint16_t);
          if(!(count > 2)) break;
          count -= sizeof(uint16_t);
          if (count < 512) break;
          ssize_t bytesRead = ::read(mFd, p, 512);// AAC has fixed frame size of 512 bytes
              if (bytesRead > 0) {
                  LOGV("Number of Bytes read = %d", bytesRead);
                  count -= bytesRead;
                  p += bytesRead;
                  bytes += bytesRead;
                  LOGV("Total Number of Bytes read = %d", bytes);
		  *frameSizePtr =  bytesRead;
		  (*frameCountPtr)++;
	      }
        }
    }
    if(mFormat == AUDIO_HW_IN_FORMAT)
    {
        while (count) {
            ssize_t bytesRead = ::read(mFd, buffer, count);
            if (bytesRead >= 0) {
                count -= bytesRead;
                p += bytesRead;
                bytes += bytesRead;
            }
            else
            {
                if (errno != EAGAIN) return bytesRead;
                mRetryCount++;
                LOGW("EAGAIN - retrying");
            }
        }
    }
    else if (mFormat == AudioSystem::EVRC)
    {
       while (count) {
            ssize_t bytesRead = ::read(mFd, buffer, EVRC_FRAME_SIZE);
            if (bytesRead >= 0) {
                p += EVRC_FRAME_SIZE;
                count -= EVRC_FRAME_SIZE;
                bytes += EVRC_FRAME_SIZE;
                buffer += EVRC_FRAME_SIZE;
            }
            else {
                if (errno != EAGAIN) return bytesRead;
                mRetryCount++;
                LOGW("EAGAIN - retrying");
            }
        }
    }
    else if (mFormat == AudioSystem::QCELP)
    {
       while (count) {
            ssize_t bytesRead = ::read(mFd, buffer, QCELP_FRAME_SIZE);
            if (bytesRead >= 0) {
                p += QCELP_FRAME_SIZE;
                count -= QCELP_FRAME_SIZE;
                bytes += QCELP_FRAME_SIZE;
                buffer += QCELP_FRAME_SIZE;
            }
            else {
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
        if (mHardware) {
        mHardware->set_mRecordState(0);
    }

    if (!mHardware) return -1;
    if (mState > AUDIO_INPUT_CLOSED) {
        if (mFd >= 0) {
            ::close(mFd);
            mFd = -1;
        }
        //mHardware->checkMicMute();
        mState = AUDIO_INPUT_CLOSED;
    }
    LOGI("AudioHardware PCM record is going to standby.");
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
    status_t status = NO_ERROR;
    int device;
    String8 key = String8(AudioParameter::keyRouting);
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

}; // namespace android
