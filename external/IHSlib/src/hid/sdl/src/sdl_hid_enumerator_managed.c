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

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h> /* snprintf; SDL2 pulled it in transitively, SDL3 does not */
#include "sdl_hid_common.h"
#include "sdl_hid_utils.h"
#include "sdl_hid_enumerators.h"

#include "ihslib/enumeration.h"
#include "ihslib/hid.h"

static IHS_Enumeration *EnumerationAlloc(const IHS_EnumerationClass *cls, void *arg);

static size_t EnumerationCount(const IHS_Enumeration *enumeration);

static void EnumerationReset(IHS_Enumeration *enumeration);

static bool EnumerationEnded(const IHS_Enumeration *enumeration);

static void *EnumerationGet(const IHS_Enumeration *enumeration);

static void *EnumerationNext(IHS_Enumeration *enumeration);

static bool EnumerationGetInfo(IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info);

static void EnumerationFree(IHS_Enumeration *enumeration);

static int NextControllerIndex(const GameControllerEnumeration *gce, int current);

const static IHS_HIDDeviceSDLEnumerationClass ManagedEnumerationClass = {
        .base = {
                .alloc = EnumerationAlloc,
                .free = EnumerationFree,
                .count = EnumerationCount,
                .reset = EnumerationReset,
                .ended = EnumerationEnded,
                .get = EnumerationGet,
                .next = EnumerationNext,
        },
        .getInfo = EnumerationGetInfo,
};

IHS_Enumeration *IHS_HIDDeviceSDLEnumerateManaged() {
    return IHS_EnumerationCreate((const IHS_EnumerationClass *) &ManagedEnumerationClass, NULL);
}

static IHS_Enumeration *EnumerationAlloc(const IHS_EnumerationClass *cls, void *arg) {
    (void) arg;
    GameControllerEnumeration *enumeration = calloc(1, sizeof(GameControllerEnumeration));
    enumeration->base.cls = cls;
    EnumerationReset((IHS_Enumeration *) enumeration);
    return (IHS_Enumeration *) enumeration;
}

static size_t EnumerationCount(const IHS_Enumeration *enumeration) {
    size_t count = 0;
    const GameControllerEnumeration *gce = (const GameControllerEnumeration *) enumeration;
    for (int i = 0, j = gce->joystickCount; i < j; i++) {
        if (SDL_IsGamepad(gce->joystickIds[i])) {
            count++;
        }
    }
    return count;
}

static void EnumerationReset(IHS_Enumeration *enumeration) {
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    SDL_free(gce->joystickIds);
    int count = 0;
    gce->joystickIds = SDL_GetJoysticks(&count);
    gce->joystickIndex = 0;
    gce->joystickCount = gce->joystickIds != NULL ? count : 0;
}

static void EnumerationFree(IHS_Enumeration *enumeration) {
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    SDL_free(gce->joystickIds);
    free(gce);
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
    gce->joystickIndex = NextControllerIndex(gce, gce->joystickIndex);
    return EnumerationGet(enumeration);
}

static bool EnumerationGetInfo(IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info) {
    GameControllerEnumeration *gce = (GameControllerEnumeration *) enumeration;
    if (gce->joystickIndex >= gce->joystickCount) {
        return false;
    }
    SDL_JoystickID instanceId = gce->joystickIds[gce->joystickIndex];
    snprintf(gce->temp.path, 16, "sdl://%u", instanceId);
    const char *name = SDL_GetJoystickNameForID(instanceId);
    if (name != NULL) {
        strncpy(gce->temp.product_string, name, 63);
        gce->temp.product_string[63] = '\0';
    } else {
        strcpy(gce->temp.product_string, "Generic Gamepad");
    }
    info->vendor_id = SDL_GetJoystickVendorForID(instanceId);
    info->product_id = SDL_GetJoystickProductForID(instanceId);
    info->product_version = SDL_GetJoystickProductVersionForID(instanceId);
    info->path = gce->temp.path;
    info->product_string = gce->temp.product_string;
    return true;
}

static int NextControllerIndex(const GameControllerEnumeration *gce, int current) {
    int count = gce->joystickCount;
    if (current >= count) {
        return count;
    }
    int i = current + 1;
    while (i < count && !SDL_IsGamepad(gce->joystickIds[i])) {
        i++;
    }
    return i;
}