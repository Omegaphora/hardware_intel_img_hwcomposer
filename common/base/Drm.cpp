/*
 * Copyright © 2012 Intel Corporation
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jackie Li <yaodong.li@intel.com>
 *
 */
#include <fcntl.h>
#include <errno.h>
#include <HwcTrace.h>
#include <IDisplayDevice.h>
#include <DrmConfig.h>
#include <Drm.h>
#include <Hwcomposer.h>

namespace android {
namespace intel {

Drm::Drm()
    : mDrmFd(0),
      mLock(),
      mInitialized(false)
{
    memset(&mOutputs, 0, sizeof(mOutputs));
}

Drm::~Drm()
{
    WARN_IF_NOT_DEINIT();
}

bool Drm::initialize()
{
    if (mInitialized) {
        WTRACE("Drm object has been initialized");
        return true;
    }

    const char *path = DrmConfig::getDrmPath();
    mDrmFd = open(path, O_RDWR, 0);
    if (mDrmFd < 0) {
        ETRACE("failed to open Drm, error: %s", strerror(errno));
        return false;
    }
    DTRACE("mDrmFd = %d", mDrmFd);

    memset(&mOutputs, 0, sizeof(mOutputs));
    mInitialized = true;
    return true;
}

void Drm::deinitialize()
{
    for (int i = 0; i < OUTPUT_MAX; i++) {
        resetOutput(i);
    }

    if (mDrmFd) {
        close(mDrmFd);
        mDrmFd = 0;
    }
    mInitialized = false;
}

bool Drm::detect(int device)
{
    RETURN_FALSE_IF_NOT_INIT();

    Mutex::Autolock _l(mLock);
    int outputIndex = getOutputIndex(device);
    if (outputIndex < 0 ) {
        return false;
    }

    resetOutput(outputIndex);

    // get drm resources
    drmModeResPtr resources = drmModeGetResources(mDrmFd);
    if (!resources) {
        ETRACE("fail to get drm resources, error: %s", strerror(errno));
        return false;
    }

    drmModeConnectorPtr connector = NULL;
    DrmOutput *output = &mOutputs[outputIndex];
    bool ret = false;

    // find connector for the given device
    for (int i = 0; i < resources->count_connectors; i++) {
        if (!resources->connectors || !resources->connectors[i]) {
            ETRACE("fail to get drm resources connectors, error: %s", strerror(errno));
            continue;
        }

        connector = drmModeGetConnector(mDrmFd, resources->connectors[i]);
        if (!connector) {
            ETRACE("drmModeGetConnector failed");
            continue;
        }

        if (connector->connector_type != DrmConfig::getDrmConnector(device)) {
            drmModeFreeConnector(connector);
            continue;
        }

        if (connector->connection != DRM_MODE_CONNECTED) {
            ITRACE("device %d is not connected", device);
            drmModeFreeConnector(connector);
            ret = true;
            break;
        }

        output->connector = connector;
        output->connected = true;

        // get proper encoder for the given connector
        if (connector->encoder_id) {
            ITRACE("Drm connector has encoder attached on device %d", device);
            output->encoder = drmModeGetEncoder(mDrmFd, connector->encoder_id);
            if (!output->encoder) {
                ETRACE("failed to get encoder from a known encoder id");
                // fall through to get an encoder
            }
        }
        if (!output->encoder) {
            ITRACE("getting encoder for device %d", device);
            drmModeEncoderPtr encoder;
            for (int j = 0; j < resources->count_encoders; j++) {
                if (!resources->encoders || !resources->encoders[j]) {
                    ETRACE("fail to get drm resources encoders, error: %s", strerror(errno));
                    continue;
                }

                encoder = drmModeGetEncoder(mDrmFd, resources->encoders[i]);
                if (!encoder) {
                    ETRACE("drmModeGetEncoder failed");
                    continue;
                }
                if (encoder->encoder_type == DrmConfig::getDrmEncoder(device)) {
                    output->encoder = encoder;
                    break;
                }
                drmModeFreeEncoder(encoder);
                encoder = NULL;
            }
        }
        if (!output->encoder) {
            ETRACE("failed to get drm encoder");
            break;
        }

        // get an attached crtc or spare crtc
        if (output->encoder->crtc_id) {
            ITRACE("Drm encoder has crtc attached on device %d", device);
            output->crtc = drmModeGetCrtc(mDrmFd, output->encoder->crtc_id);
            if (!output->crtc) {
                ETRACE("failed to get crtc from a known crtc id");
                // fall through to get a spare crtc
            }
        }
        if (!output->crtc) {
            ITRACE("getting crtc for device %d", device);
            drmModeCrtcPtr crtc;
            for (int j = 0; j < resources->count_crtcs; j++) {
                if (!resources->crtcs || !resources->crtcs[j]) {
                    ETRACE("fail to get drm resources crtcs, error: %s", strerror(errno));
                    continue;
                }

                crtc = drmModeGetCrtc(mDrmFd, resources->crtcs[j]);
                if (!crtc) {
                    ETRACE("drmModeGetCrtc failed");
                    continue;
                }
                if (crtc->buffer_id == 0) {
                    output->crtc = crtc;
                    break;
                }
                drmModeFreeCrtc(crtc);
            }
        }
        if (!output->crtc) {
            ETRACE("failed to get drm crtc");
            break;
        }

        // current mode
        if (output->crtc->mode_valid) {
            ITRACE("mode is valid, kernel mode settings");
            memcpy(&output->mode, &output->crtc->mode, sizeof(drmModeModeInfo));
            ret = true;
        } else {
            ITRACE("mode is invalid, setting preferred mode");
            ret = initDrmMode(outputIndex);
        }
        break;
    }

    if (!ret) {
        if (output->connector == NULL && outputIndex != OUTPUT_PRIMARY) {
            // a fatal failure on primary device
            // non fatal on secondary device
            WTRACE("device %d is disabled?", device);
            ret = true;
        }
         resetOutput(outputIndex);
    } else if (output->connected) {
        ITRACE("mode is: %dx%d@%dHz", output->mode.hdisplay, output->mode.vdisplay, output->mode.vrefresh);
    }

    drmModeFreeResources(resources);
    return ret;
}

bool Drm::isSameDrmMode(drmModeModeInfoPtr value,
        drmModeModeInfoPtr base) const
{
    if (base->hdisplay == value->hdisplay &&
        base->vdisplay == value->vdisplay &&
        base->vrefresh == value->vrefresh &&
        (base->flags & value->flags) == value->flags) {
        VTRACE("Drm mode is not changed");
        return true;
    }

    return false;
}

bool Drm::setDrmMode(int device, drmModeModeInfo& value)
{
    RETURN_FALSE_IF_NOT_INIT();
    Mutex::Autolock _l(mLock);

    if (device != IDisplayDevice::DEVICE_EXTERNAL) {
        WTRACE("Setting mode on invalid device %d", device);
        return false;
    }

    int outputIndex = getOutputIndex(device);
    if (outputIndex < 0 ) {
        ETRACE("invalid device");
        return false;
    }

    DrmOutput *output= &mOutputs[outputIndex];
    if (!output->connected) {
        ETRACE("device is not connected");
        return false;
    }

    if (output->connector->count_modes <= 0) {
        ETRACE("invalid count of modes");
        return false;
    }

    drmModeModeInfoPtr mode;
    int index = 0;
    for (int i = 0; i < output->connector->count_modes; i++) {
        mode = &output->connector->modes[i];
        if (mode->type & DRM_MODE_TYPE_PREFERRED) {
            index = i;
        }
        if (isSameDrmMode(&value, mode)) {
            index = i;
            break;
        }
    }

    mode = &output->connector->modes[index];
    return setDrmMode(outputIndex, mode);
}

bool Drm::setRefreshRate(int device, int hz)
{
    RETURN_FALSE_IF_NOT_INIT();
    Mutex::Autolock _l(mLock);

    if (device != IDisplayDevice::DEVICE_EXTERNAL) {
        WTRACE("Setting mode on invalid device %d", device);
        return false;
    }

    int outputIndex = getOutputIndex(device);
    if (outputIndex < 0 ) {
        ETRACE("invalid device");
        return false;
    }

    DrmOutput *output= &mOutputs[outputIndex];
    if (!output->connected) {
        ETRACE("device is not connected");
        return false;
    }

    if (output->connector->count_modes <= 0) {
        ETRACE("invalid count of modes");
        return false;
    }

    drmModeModeInfoPtr mode;
    int index = 0;
    for (int i = 0; i < output->connector->count_modes; i++) {
        mode = &output->connector->modes[i];
        if (mode->type & DRM_MODE_TYPE_PREFERRED) {
            index = i;
        }
        if (mode->hdisplay == output->mode.hdisplay &&
            mode->vdisplay == output->mode.vdisplay &&
            mode->vrefresh == (uint32_t)hz) {
            index = i;
            break;
        }
    }

    mode = &output->connector->modes[index];
    return setDrmMode(outputIndex, mode);
}

bool Drm::writeReadIoctl(unsigned long cmd, void *data,
                           unsigned long size)
{
    int err;

    if (mDrmFd <= 0) {
        ETRACE("drm is not initialized");
        return false;
    }

    if (!data || !size) {
        ETRACE("invalid parameters");
        return false;
    }

    err = drmCommandWriteRead(mDrmFd, cmd, data, size);
    if (err) {
        WTRACE("failed to call %ld ioctl with failure %d", cmd, err);
        return false;
    }

    return true;
}

bool Drm::writeIoctl(unsigned long cmd, void *data,
                       unsigned long size)
{
    int err;

    if (mDrmFd <= 0) {
        ETRACE("drm is not initialized");
        return false;
    }

    if (!data || !size) {
        ETRACE("invalid parameters");
        return false;
    }

    err = drmCommandWrite(mDrmFd, cmd, data, size);
    if (err) {
        WTRACE("failed to call %ld ioctl with failure %d", cmd, err);
        return false;
    }

    return true;
}

int Drm::getDrmFd() const
{
    return mDrmFd;
}

bool Drm::getModeInfo(int device, drmModeModeInfo& mode)
{
    Mutex::Autolock _l(mLock);

    int outputIndex = getOutputIndex(device);
    if (outputIndex < 0 ) {
        return false;
    }

    DrmOutput *output= &mOutputs[outputIndex];
    if (output->connected == false) {
        ETRACE("device is not connected");
        return false;
    }

    if (output->mode.hdisplay == 0 || output->mode.vdisplay == 0) {
        ETRACE("invalid width or height");
        return false;
    }

    memcpy(&mode, &output->mode, sizeof(drmModeModeInfo));
    return true;
}

bool Drm::getPhysicalSize(int device, uint32_t& width, uint32_t& height)
{
    Mutex::Autolock _l(mLock);

    int outputIndex = getOutputIndex(device);
    if (outputIndex < 0 ) {
        return false;
    }

    DrmOutput *output= &mOutputs[outputIndex];
    if (output->connected == false) {
        ETRACE("device is not connected");
        return false;
    }

    width = output->connector->mmWidth;
    height = output->connector->mmHeight;
    return true;
}

bool Drm::isConnected(int device)
{
    Mutex::Autolock _l(mLock);

    int output = getOutputIndex(device);
    if (output < 0 ) {
        return false;
    }

    return mOutputs[output].connected;
}

bool Drm::setDpmsMode(int device, int mode)
{
    WTRACE("DPMS ignored");
    return false;
    Mutex::Autolock _l(mLock);

    int output = getOutputIndex(device);
    if (output < 0 ) {
        return false;
    }

    if (mode != IDisplayDevice::DEVICE_DISPLAY_OFF &&
        mode != IDisplayDevice::DEVICE_DISPLAY_ON) {
        ETRACE("invalid mode %d", mode);
        return false;
    }

    DrmOutput *out = &mOutputs[output];
    if (!out->connected) {
        ETRACE("device is not connected");
        return false;
    }

    drmModePropertyPtr props;
    for (int i = 0; i < out->connector->count_props; i++) {
        props = drmModeGetProperty(mDrmFd, out->connector->props[i]);
        if (!props) {
            continue;
        }

        if (strcmp(props->name, "DPMS") == 0) {
            int ret = drmModeConnectorSetProperty(
                mDrmFd,
                out->connector->connector_id,
                props->prop_id,
                (mode == IDisplayDevice::DEVICE_DISPLAY_ON) ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
            drmModeFreeProperty(props);
            if (ret != 0) {
                ETRACE("unable to set DPMS %d", mode);
                return false;
            } else {
                return true;
            }
        }
        drmModeFreeProperty(props);
    }
    return false;
}

void Drm::resetOutput(int index)
{
    DrmOutput *output = &mOutputs[index];

    output->connected = false;
    memset(&output->mode, 0, sizeof(drmModeModeInfo));

    if (output->connector) {
        drmModeFreeConnector(output->connector);
        output->connector = 0;
    }
    if (output->encoder) {
        drmModeFreeEncoder(output->encoder);
        output->encoder = 0;
    }
    if (output->crtc) {
        drmModeFreeCrtc(output->crtc);
        output->crtc = 0;
    }
    if (output->fbId) {
        drmModeRmFB(mDrmFd, output->fbId);
        output->fbId = 0;
    }
    if (output->fbHandle) {
        Hwcomposer::getInstance().getBufferManager()->freeFrameBuffer(output->fbHandle);
        output->fbHandle = 0;
    }
}

bool Drm::initDrmMode(int outputIndex)
{
    DrmOutput *output= &mOutputs[outputIndex];
    if (output->connector->count_modes <= 0) {
        ETRACE("invalid count of modes");
        return false;
    }

    drmModeModeInfoPtr mode;
    int index = 0;
    for (int i = 0; i < output->connector->count_modes; i++) {
        mode = &output->connector->modes[i];
        if (mode->type & DRM_MODE_TYPE_PREFERRED) {
            index = i;
            break;
        }
    }

    return setDrmMode(outputIndex, &output->connector->modes[index]);
}

bool Drm::setDrmMode(int index, drmModeModeInfoPtr mode)
{
    DrmOutput *output = &mOutputs[index];

    int oldFbId =0;
    int oldFbHandle = 0;

    drmModeModeInfo currentMode;
    memcpy(&currentMode, &output->mode, sizeof(drmModeModeInfo));

    if (isSameDrmMode(mode, &currentMode))
        return true;


    if (output->fbId) {
        oldFbId = output->fbId ;
        output->fbId = 0;
    }

    if (output->fbHandle) {
        oldFbHandle = output->fbHandle;
        output->fbHandle = 0;
    }

    // allocate frame buffer
    int stride = 0;
    output->fbHandle = Hwcomposer::getInstance().getBufferManager()->allocFrameBuffer(
        mode->hdisplay, mode->vdisplay, &stride);
    if (output->fbHandle == 0) {
        ETRACE("failed to allocate frame buffer");
        return false;
    }

    int ret = 0;
    ret = drmModeAddFB(
        mDrmFd,
        mode->hdisplay,
        mode->vdisplay,
        DrmConfig::getFrameBufferDepth(),
        DrmConfig::getFrameBufferBpp(),
        stride,
        output->fbHandle,
        &output->fbId);
    if (ret != 0) {
        ETRACE("drmModeAddFB failed, error: %d", ret);
        return false;
    }

    ITRACE("mode set: %dx%d@%dHz", mode->hdisplay, mode->vdisplay, mode->vrefresh);

    ret = drmModeSetCrtc(mDrmFd, output->crtc->crtc_id, output->fbId, 0, 0,
                   &output->connector->connector_id, 1, mode);
    if (ret == 0) {
        //save mode
        memcpy(&output->mode, mode, sizeof(drmModeModeInfo));
    } else {
        ETRACE("drmModeSetCrtc failed. error: %d", ret);
    }

    if (oldFbId) {
        drmModeRmFB(mDrmFd, oldFbId);
    }

    if (oldFbHandle) {
        Hwcomposer::getInstance().getBufferManager()->freeFrameBuffer(oldFbHandle);
    }

    return ret == 0;
}

int Drm::getOutputIndex(int device)
{
    switch (device) {
    case IDisplayDevice::DEVICE_PRIMARY:
        return OUTPUT_PRIMARY;
    case IDisplayDevice::DEVICE_EXTERNAL:
        return OUTPUT_EXTERNAL;
    default:
        ETRACE("invalid display device");
        break;
    }

    return -1;
}


} // namespace intel
} // namespace android

