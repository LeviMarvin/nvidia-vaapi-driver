#define _GNU_SOURCE 1

#include "../export-buf.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <string.h>

#if defined __has_include && __has_include(<libdrm/drm.h>)
#  include <libdrm/drm.h>
#  include <libdrm/drm_fourcc.h>
#else
#  include <drm/drm.h>
#  include <drm/drm_fourcc.h>
#endif

void findGPUIndexFromFd(NVDriver *drv) {
    //find the CUDA device id
    char drmUuid[16];
    get_device_uuid(&drv->driverContext, drmUuid);

    int gpuCount = 0;
    CHECK_CUDA_RESULT(drv->cu->cuDeviceGetCount(&gpuCount));

    for (int i = 0; i < gpuCount; i++) {
        CUuuid uuid;
        CUresult ret = drv->cu->cuDeviceGetUuid(&uuid, i);
        if (ret == CUDA_SUCCESS && memcmp(drmUuid, uuid.bytes, 16) == 0) {
            drv->cudaGpuId = i;
            return;
        }
    }

    drv->cudaGpuId = 0;
}

static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

bool initExporter(NVDriver *drv) {
    static const EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};
    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");
    eglDebugMessageControlKHR(debug, debugAttribs);

    //make sure we have a drm fd
    if (drv->drmFd == -1) {
        //TODO make this configurable
        drv->drmFd = open("/dev/dri/renderD128", O_RDWR|O_CLOEXEC);
        LOG("Manually opened DRM device");
    } else {
        //dup it so we can close it later and not effect firefox
        drv->drmFd = dup(drv->drmFd);
    }

    bool ret = init_nvdriver(&drv->driverContext, drv->drmFd);

    findGPUIndexFromFd(drv);

    return ret;
}

void releaseExporter(NVDriver *drv) {
    free_nvdriver(&drv->driverContext);
}

bool exportBackingImage(NVDriver *drv, BackingImage *img) {
    //backing image is already exported, we don't need to do anything here
    return true;
}

void import_to_cuda(NVDriver *drv, NVDriverImage *image, int bpc, int channels, NVCudaImage *cudaImage, CUarray *array) {
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = image->nvFd,
        .flags = 0,
        .size = image->memorySize
    };

    LOG("importing memory size: %dx%d = %x", image->width, image->height, image->memorySize);

    CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&cudaImage->extMem, &extMemDesc));

    close(image->nvFd);

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
        .arrayDesc = {
            .Width = image->width,
            .Height = image->height,
            .Depth = 0,
            .Format = bpc == 8 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
            .NumChannels = channels,
            .Flags = 0
        },
        .numLevels = 1,
        .offset = 0
    };
    //create a mimap array from the imported memory
    CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&cudaImage->mipmapArray, cudaImage->extMem, &mipmapArrayDesc));

    //create an array from the mipmap array
    CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(array, cudaImage->mipmapArray, 0));
}

BackingImage *allocateBackingImage(NVDriver *drv, const NVSurface *surface) {
    NVDriverImage driverImageY = { 0 }, driverImageUV = { 0 };

    BackingImage *backingImage = calloc(1, sizeof(BackingImage));

    alloc_image(&drv->driverContext, surface->width, surface->height, 1, 8, &driverImageY);
    alloc_image(&drv->driverContext, surface->width>>1, surface->height>>1, 2, 8, &driverImageUV);

    import_to_cuda(drv, &driverImageY, 8, 1, &backingImage->cudaImages[0], &backingImage->arrays[0]);
    import_to_cuda(drv, &driverImageUV, 8, 2, &backingImage->cudaImages[1], &backingImage->arrays[1]);

    backingImage->fds[0] = driverImageY.drmFd;
    backingImage->fds[1] = driverImageUV.drmFd;

    backingImage->fourcc = DRM_FORMAT_NV12;

    backingImage->width = surface->width;
    backingImage->height = surface->height;

    backingImage->strides[0] = driverImageY.pitch;
    backingImage->strides[1] = driverImageUV.pitch;

    backingImage->mods[0] = driverImageY.mods;
    backingImage->mods[1] = driverImageUV.mods;

    backingImage->size[0] = driverImageY.memorySize;
    backingImage->size[1] = driverImageUV.memorySize;

    return backingImage;
}

void destroyBackingImage(NVDriver *drv, BackingImage *img) {
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }

    for (int i = 0; i < 4; i++) {
        if (img->fds[i] > 0) {
            close(img->fds[i]);
        }
    }

    for (int i = 0; i < 2; i++) {
        if (img->arrays[i] != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[i]));
        }

        CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray));
        CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem));
    }

    memset(img, 0, sizeof(BackingImage));
    free(img);
}

void attachBackingImageToSurface(NVSurface *surface, BackingImage *img) {
    surface->backingImage = img;
    img->surface = surface;
}

void detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface) {
    if (surface->backingImage == NULL) {
        return;
    }

    destroyBackingImage(drv, surface->backingImage);
    surface->backingImage = NULL;
}

void destroyAllBackingImage(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH_REV(BackingImage*, it, &drv->images)
        destroyBackingImage(drv, it);
        remove_element_at(&drv->images, it_idx);
    END_FOR_EACH

    pthread_mutex_unlock(&drv->imagesMutex);
}

bool copyFrameToSurface(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    int bpp = surface->format == cudaVideoSurfaceFormat_NV12 ? 1 : 2;
    CUDA_MEMCPY2D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[0],
        .Height = surface->height,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0));
    CUDA_MEMCPY2D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = surface->height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[1],
        .Height = surface->height >> 1,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy2));

    //notify anyone waiting for us to be resolved
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 0;
    pthread_cond_signal(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

bool realiseSurface(NVDriver *drv, NVSurface *surface) {
    //make sure we're the only thread updating this surface
    pthread_mutex_lock(&surface->mutex);
    //check again to see if it's just been created
    if (surface->backingImage == NULL) {
        //try to find a free surface
        BackingImage *img = img = allocateBackingImage(drv, surface);
        if (img == NULL) {
            LOG("Unable to realise surface: %p (%d)", surface, surface->pictureIdx)
            pthread_mutex_unlock(&surface->mutex);
            return false;
        }

        attachBackingImageToSurface(surface, img);
    }
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

bool exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    if (!realiseSurface(drv, surface)) {
        return false;
    }

    if (ptr != 0 && !copyFrameToSurface(drv, ptr, surface, pitch)) {
        LOG("Unable to update surface from frame");
        return false;
    } else if (ptr == 0) {
        LOG("exporting with null ptr");
    }

    return true;
}

bool fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    BackingImage *img = surface->backingImage;

    //TODO only support 420 images (either NV12, P010 or P012)
    desc->fourcc = img->fourcc;
    desc->width = surface->width;
    desc->height = surface->height;
    desc->num_layers = 2;
    desc->num_objects = 2;

    desc->objects[0].fd = dup(img->fds[0]);
    desc->objects[0].size = img->size[0];
    desc->objects[0].drm_format_modifier = img->mods[0];

    desc->objects[1].fd = dup(img->fds[1]);
    desc->objects[1].size = img->size[1];
    desc->objects[1].drm_format_modifier = img->mods[1];

    desc->layers[0].drm_format = img->fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_R8 : DRM_FORMAT_R16;
    desc->layers[0].num_planes = 1;
    desc->layers[0].object_index[0] = 0;
    desc->layers[0].offset[0] = img->offsets[0];
    desc->layers[0].pitch[0] = img->strides[0];

    desc->layers[1].drm_format = img->fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_RG88 : DRM_FORMAT_RG1616;
    desc->layers[1].num_planes = 1;
    desc->layers[1].object_index[0] = 1;
    desc->layers[1].offset[0] = img->offsets[1];
    desc->layers[1].pitch[0] = img->strides[1];

    return true;
}
