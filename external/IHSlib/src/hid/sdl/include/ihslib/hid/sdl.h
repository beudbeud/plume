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

#pragma once

#include "ihslib/hid.h"
#include "ihslib/hid/sdl/config.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_gamepad.h>

/**
 *  This macro will evaluate to true if targeting SDL at least X.Y.Z.
 */
#define IHS_HID_SDL_TARGET_ATLEAST(X, Y, Z) \
    ((IHS_HID_SDL_TARGET_MAJOR >= X) && \
     (IHS_HID_SDL_TARGET_MAJOR > X || IHS_HID_SDL_TARGET_MINOR >= Y) && \
     (IHS_HID_SDL_TARGET_MAJOR > X || IHS_HID_SDL_TARGET_MINOR > Y || IHS_HID_SDL_TARGET_PATCH >= Z))

typedef struct IHS_HIDProviderSDLJoystickIDMapping {
    int (*count)(void *context);

    int (*index)(SDL_JoystickID joystickId, void *context);

    SDL_JoystickID (*instanceId)(int index, void *context);

    SDL_Gamepad *(*controller)(int index, void *context);
} IHS_HIDProviderSDLDeviceList;

IHS_HIDProvider *IHS_HIDProviderSDLCreateManaged();

IHS_HIDProvider *IHS_HIDProviderSDLCreateUnmanaged(const IHS_HIDProviderSDLDeviceList *list, void *listContext);

void IHS_HIDProviderSDLDestroy(IHS_HIDProvider *provider);

/**
 * For an event from IHS managed device, it will add report data.
 *
 * This only accumulates state; it never transmits. When it returns true, call
 * IHS_SessionHIDSendReport() to flush — once per rendered frame is a good rate.
 * Sending per event floods the reliable control channel (sticks and gyros emit
 * hundreds of events per second) and starves the video stream of bandwidth.
 *
 * @param session
 * @param event
 * @return true if the report changed and a flush is due
 */
bool IHS_HIDHandleSDLEvent(IHS_Session *session, const SDL_Event *event);

/**
 * Reset input state for all SDL game controllers
 * @param session
 * @return
 */
bool IHS_HIDResetSDLGameControllers(IHS_Session *session);