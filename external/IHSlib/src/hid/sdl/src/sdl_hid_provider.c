/*
 *  _____  _   _  _____  _  _  _     
 * |_   _|| | | |/  ___|| |(_)| |     Steam    
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "ihslib/hid/sdl.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ihslib/hid.h"
#include "hid/provider.h"
#include "sdl_hid_common.h"
#include "sdl_hid_enumerators.h"

#include <SDL3/SDL.h>

typedef struct HIDProviderSDL {
    IHS_HIDProvider base;
    bool managed;
    const IHS_HIDProviderSDLDeviceList *deviceList;
    void *deviceListContext;
} HIDProviderSDL;

static IHS_HIDProvider *ProviderAlloc(const IHS_HIDProviderClass *cls);

static void ProviderFree(IHS_HIDProvider *provider);

static bool ProviderSupportsDevice(IHS_HIDProvider *provider, const char *path);

static IHS_HIDDevice *ProviderOpenDevice(IHS_HIDProvider *provider, const char *path);

static bool ProviderHasChange(IHS_HIDProvider *provider);

static IHS_Enumeration *ProviderEnumerate(IHS_HIDProvider *provider);

static void ProviderDeviceInfo(IHS_HIDProvider *provider, IHS_Enumeration *enumeration,
                               IHS_HIDDeviceInfo *info);

static const IHS_HIDProviderClass ProviderClass = {
        .alloc = ProviderAlloc,
        .free = ProviderFree,
        .supportsDevice = ProviderSupportsDevice,
        .openDevice = ProviderOpenDevice,
        .hasChange = ProviderHasChange,
        .enumerateDevices = ProviderEnumerate,
        .deviceInfo = ProviderDeviceInfo,
};

IHS_HIDProvider *IHS_HIDProviderSDLCreateManaged() {
    HIDProviderSDL *provider = (HIDProviderSDL *) IHS_SessionHIDProviderCreate(&ProviderClass);
    provider->managed = true;
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    return (IHS_HIDProvider *) provider;
}

IHS_HIDProvider *IHS_HIDProviderSDLCreateUnmanaged(const IHS_HIDProviderSDLDeviceList *list, void *listContext) {
    HIDProviderSDL *provider = (HIDProviderSDL *) IHS_SessionHIDProviderCreate(&ProviderClass);
    provider->managed = false;
    provider->deviceList = list;
    provider->deviceListContext = listContext;
    return (IHS_HIDProvider *) provider;
}

void IHS_HIDProviderSDLDestroy(IHS_HIDProvider *provider) {
    // Must be unused provider
    if (((HIDProviderSDL *) provider)->managed) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
    IHS_SessionHIDProviderDestroy(provider);
}

static IHS_HIDProvider *ProviderAlloc(const IHS_HIDProviderClass *cls) {
    HIDProviderSDL *provider = calloc(1, sizeof(HIDProviderSDL));
    provider->base.cls = cls;
    return (IHS_HIDProvider *) provider;
}

static void ProviderFree(IHS_HIDProvider *provider) {
    free(provider);
}

static bool ProviderSupportsDevice(IHS_HIDProvider *provider, const char *path) {
    (void) provider;
    return strstr(path, "sdl://") == path;
}

static IHS_HIDDevice *ProviderOpenDevice(IHS_HIDProvider *provider, const char *path) {
    HIDProviderSDL *sdlProvider = (HIDProviderSDL *) provider;
    assert(strstr(path, "sdl://") == path);
    char *endptr = NULL;
    const char *begin = &path[6];
    SDL_JoystickID instanceId = (SDL_JoystickID) strtol(begin, &endptr, 10);
    if (endptr == begin) {
        return NULL;
    }
    SDL_Gamepad *controller;
    if (sdlProvider->managed) {
        controller = SDL_OpenGamepad(instanceId);
    } else {
        controller = SDL_GetGamepadFromID(instanceId);
    }
    if (controller == NULL) {
        return NULL;
    }
    return IHS_HIDDeviceSDLCreate(provider, controller, sdlProvider->managed);
}

static bool ProviderHasChange(IHS_HIDProvider *provider) {
    (void) provider;
    return false;
}

static IHS_Enumeration *ProviderEnumerate(IHS_HIDProvider *provider) {
    HIDProviderSDL *sdlProvider = (HIDProviderSDL *) provider;
    if (sdlProvider->managed) {
        return IHS_HIDDeviceSDLEnumerateManaged();
    } else {
        return IHS_HIDDeviceSDLEnumerateUnmanaged(sdlProvider->deviceList, sdlProvider->deviceListContext);
    }
}

static void ProviderDeviceInfo(IHS_HIDProvider *provider, IHS_Enumeration *enumeration,
                               IHS_HIDDeviceInfo *info) {
    (void) provider;
    IHS_HIDDeviceSDLEnumerationGetInfo(enumeration, info);
}