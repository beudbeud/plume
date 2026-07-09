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
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h> /* snprintf; SDL2 pulled it in transitively, SDL3 does not */

#include "sdl_hid_common.h"
#include "sdl_hid_utils.h"
#include "sdl_hid_enumerators.h"

#include "ihslib/enumeration.h"
#include "ihslib/hid.h"
#include "ihslib/hid/sdl.h"

typedef struct UnmanagedEnumerationAllocArg {
    const IHS_HIDProviderSDLDeviceList *list;
    void *listContext;
} UnmanagedEnumerationAllocArg;

typedef struct UnmanagedEnumeration {
    GameControllerEnumeration base;
    const IHS_HIDProviderSDLDeviceList *list;
    void *listContext;
} UnmanagedEnumeration;

static IHS_Enumeration *EnumerationAlloc(const IHS_EnumerationClass *cls, void *arg);

static size_t EnumerationCount(const IHS_Enumeration *enumeration);

static void EnumerationReset(IHS_Enumeration *enumeration);

static bool EnumerationEnded(const IHS_Enumeration *enumeration);

static void *EnumerationGet(const IHS_Enumeration *enumeration);

static void *EnumerationNext(IHS_Enumeration *enumeration);

static bool EnumerationGetInfo(IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info);

const static IHS_HIDDeviceSDLEnumerationClass UnmanagedEnumerationClass = {
        .base = {
                .alloc = EnumerationAlloc,
                .free = (void (*)(IHS_Enumeration *)) free,
                .count = EnumerationCount,
                .reset = EnumerationReset,
                .ended = EnumerationEnded,
                .get = EnumerationGet,
                .next = EnumerationNext,
        },
        .getInfo = EnumerationGetInfo,
};

IHS_Enumeration *IHS_HIDDeviceSDLEnumerateUnmanaged(const IHS_HIDProviderSDLDeviceList *list, void *listContext) {
    assert(list != NULL);
    assert(list->count != NULL);
    assert(list->index != NULL);
    assert(list->instanceId != NULL);
    assert(list->controller != NULL);
    UnmanagedEnumerationAllocArg arg = {.list = list, .listContext =listContext};
    return IHS_EnumerationCreate((const IHS_EnumerationClass *) &UnmanagedEnumerationClass, &arg);
}

static IHS_Enumeration *EnumerationAlloc(const IHS_EnumerationClass *cls, void *arg) {
    UnmanagedEnumeration *enumeration = calloc(1, sizeof(UnmanagedEnumeration));
    enumeration->base.base.cls = cls;
    UnmanagedEnumerationAllocArg *ua = arg;
    enumeration->list = ua->list;
    enumeration->listContext = ua->listContext;
    EnumerationReset((IHS_Enumeration *) enumeration);
    return (IHS_Enumeration *) enumeration;
}

static size_t EnumerationCount(const IHS_Enumeration *enumeration) {
    const GameControllerEnumeration *gce = (const GameControllerEnumeration *) enumeration;
    return gce->joystickCount;
}

static void EnumerationReset(IHS_Enumeration *enumeration) {
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    UnmanagedEnumeration *ue = (UnmanagedEnumeration *) enumeration;
    gce->joystickIndex = 0;
    gce->joystickCount = ue->list->count(ue->listContext);
}

static bool EnumerationEnded(const IHS_Enumeration *enumeration) {
    const GameControllerEnumeration *gce = (const GameControllerEnumeration *) enumeration;
    return gce->joystickIndex >= gce->joystickCount;
}

static void *EnumerationGet(const IHS_Enumeration *enumeration) {
    const GameControllerEnumeration *gce = (const GameControllerEnumeration *) enumeration;
    if (gce->joystickIndex < gce->joystickCount) {
        return (void *) &gce->joystickIndex;
    }
    return NULL;
}

static void *EnumerationNext(IHS_Enumeration *enumeration) {
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    gce->joystickIndex++;
    return EnumerationGet(enumeration);
}

static bool EnumerationGetInfo(IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info) {
    UnmanagedEnumeration *ue = (UnmanagedEnumeration *) enumeration;
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    SDL_Gamepad *controller = ue->list->controller(gce->joystickIndex, ue->listContext);
    if (controller == NULL) {
        return false;
    }
    SDL_Joystick *joystick = SDL_GetGamepadJoystick(controller);
    SDL_JoystickID instanceId = SDL_GetJoystickID(joystick);
    snprintf(gce->temp.path, 16, "sdl://%u", instanceId);
    const char *name = SDL_GetJoystickName(joystick);
    if (name != NULL) {
        strncpy(gce->temp.product_string, name, 63);
        gce->temp.product_string[63] = '\0';
    } else {
        strcpy(gce->temp.product_string, "Generic Gamepad");
    }
    SDL_GUID guid = SDL_GetJoystickGUID(joystick);
    if (!IHS_HIDDeviceSDLGetJoystickGUIDInfo(&guid, &info->vendor_id, &info->product_id, &info->product_version,
                                             NULL)) {
        info->vendor_id = 0;
        info->product_id = 0;
        info->product_version = 0;
    }
    info->path = gce->temp.path;
    info->product_string = gce->temp.product_string;
    return true;
}
