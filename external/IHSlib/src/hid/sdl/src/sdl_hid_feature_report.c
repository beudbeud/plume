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
#include "sdl_hid_common.h"
#include "sdl_hid_utils.h"

typedef enum GetFeatureReportCommand {
    GetFeatureReportGetPowerLevel = 0x02,
    GetFeatureReportGetCaps = 0x04,
    GetFeatureReportGetSerial = 0x07,
} GetFeatureReportCommand;

/**
 * @see https://github.com/libsdl-org/SDL/blob/53dea9830964eee8b5c2a7ee0a65d6e268dc78a1/src/joystick/controller_type.h
 */
typedef enum EControllerType {
    k_ControllerTypeNone = -1,
    k_ControllerTypeUnknown = 0,

    // Steam Controllers
    k_ControllerTypeUnknownSteamController = 1,
    k_ControllerTypeSteamController = 2,
    k_ControllerTypeSteamControllerV2 = 3,

    // Other Controllers
    k_ControllerTypeUnknownNonSteamController = 30,
    k_ControllerTypeXBox360Controller = 31,
    k_ControllerTypeXBoxOneController = 32,
    k_ControllerTypePS3Controller = 33,
    k_ControllerTypePS4Controller = 34,
    k_ControllerTypeWiiController = 35,
    k_ControllerTypeAppleController = 36,
    k_ControllerTypeAndroidController = 37,
    k_ControllerTypeSwitchProController = 38,
    k_ControllerTypeSwitchJoyConLeft = 39,
    k_ControllerTypeSwitchJoyConRight = 40,
    k_ControllerTypeSwitchJoyConPair = 41,
    k_ControllerTypeSwitchInputOnlyController = 42,
    k_ControllerTypeMobileTouch = 43,
    k_ControllerTypeXInputSwitchController = 44,  // Client-side only, used to mark Switch-compatible controllers as not supporting Switch controller protocol
    k_ControllerTypePS5Controller = 45,
    k_ControllerTypeXboxEliteController = 46,
    k_ControllerTypeLastController, // Don't add game controllers below this enumeration - this enumeration can change value
} EControllerType;

typedef struct __attribute__((__packed__)) {
    bool valid;
    bool xinput;
    EControllerType controllerType: 32;
    int32_t playerIndex: 32;
    bool hid;
    uint8_t pad[9];
} DeviceFeatureReport;

_Static_assert(sizeof(DeviceFeatureReport) == 20, "");

bool IsXinputDevice(const SDL_GUID *guid);

bool IsHIDAPIDevice(const SDL_GUID *guid);

/**
 * Map SDL3's SDL_PowerState + battery percentage back onto the SDL2
 * SDL_JoystickPowerLevel byte the Steam host expects on the wire.
 * UNKNOWN/ERROR -> SDL_JOYSTICK_POWER_UNKNOWN (-1 as a byte, i.e. 0xFF).
 */
static uint8_t JoystickPowerLevelByte(SDL_Joystick *joystick) {
    int percent = -1;
    switch (SDL_GetJoystickPowerInfo(joystick, &percent)) {
        case SDL_POWERSTATE_NO_BATTERY:
            return 4; /* SDL_JOYSTICK_POWER_WIRED */
        case SDL_POWERSTATE_ON_BATTERY:
        case SDL_POWERSTATE_CHARGING:
        case SDL_POWERSTATE_CHARGED:
            if (percent < 0) return (uint8_t) -1; /* UNKNOWN */
            if (percent <= 5) return 0;  /* EMPTY */
            if (percent <= 20) return 1; /* LOW */
            if (percent <= 70) return 2; /* MEDIUM */
            return 3;                    /* FULL */
        default:
            return (uint8_t) -1; /* SDL_JOYSTICK_POWER_UNKNOWN */
    }
}

int IHS_HIDDeviceSDLGetFeatureReport(IHS_HIDDevice *device, const uint8_t *reportNumber, size_t reportNumberLen,
                                     IHS_Buffer *dest, size_t length) {
    if (reportNumberLen < 1) return -1;
    IHS_HIDDeviceSDL *sdl = (IHS_HIDDeviceSDL *) device;
    switch (reportNumber[0]) {
        case GetFeatureReportGetSerial: {
            IHS_BufferWriteMem(dest, 0, reportNumber, 1);
            const char *serial = SDL_GetGamepadSerial(sdl->controller);
            if (serial == NULL) serial = "";
            IHS_BufferWriteMem(dest, 1, (const uint8_t *) serial, strlen(serial) + 1);
            break;
        }
        case GetFeatureReportGetPowerLevel: {
            IHS_BufferWriteMem(dest, 0, reportNumber, 1);
            SDL_Joystick *joystick = SDL_GetGamepadJoystick(sdl->controller);
            uint8_t level = JoystickPowerLevelByte(joystick);
            IHS_BufferWriteMem(dest, 1, &level, 1);
            break;
        }
        case GetFeatureReportGetCaps: {
            int playerIndex = -1;
            SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(sdl->controller));

            bool xinput = IsXinputDevice(&guid);
            // Initialize so the xinput-true branch doesn't leak uninitialized stack
            // contents through the wire report (DeviceFeatureReport.controllerType below).
            EControllerType controllerType = k_ControllerTypeUnknown;
            if (!xinput) {
                playerIndex = SDL_GetGamepadPlayerIndex(sdl->controller);
                switch (SDL_GetGamepadType(sdl->controller)) {
                    case SDL_GAMEPAD_TYPE_XBOX360:
                        controllerType = k_ControllerTypeXBox360Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_XBOXONE:
                        controllerType = k_ControllerTypeXBoxOneController;
                        break;
                    case SDL_GAMEPAD_TYPE_PS3:
                        controllerType = k_ControllerTypePS3Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_PS4:
                        controllerType = k_ControllerTypePS4Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
                        controllerType = k_ControllerTypeSwitchProController;
                        break;
                    /* SDL3 dropped SDL_GAMEPAD_TYPE_VIRTUAL; virtual pads now report
                     * as whatever type they emulate, so there is nothing to map. */
                    case SDL_GAMEPAD_TYPE_PS5:
                        controllerType = k_ControllerTypePS5Controller;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                        controllerType = k_ControllerTypeSwitchJoyConLeft;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                        controllerType = k_ControllerTypeSwitchJoyConRight;
                        break;
                    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                        controllerType = k_ControllerTypeSwitchJoyConPair;
                        break;
                    default:
                        controllerType = k_ControllerTypeUnknownNonSteamController;
                        break;
                }
            }
            DeviceFeatureReport report = {
                    .valid = true,
                    .xinput = xinput,
                    .playerIndex = playerIndex,
                    .controllerType = controllerType,
                    .hid = IsHIDAPIDevice(&guid),
            };
            IHS_BufferWriteMem(dest, 0, reportNumber, 1);
            IHS_BufferWriteMem(dest, 1, (const unsigned char *) &report, 20);
            break;
        }
        default: {
            // Write an empty array
            IHS_BufferFillMem(dest, 1, 0, 1);
            return 0;
        }
    }
    return 0;
}

bool IsXinputDevice(const SDL_GUID *guid) {
    return guid->data[14] == 'x';
}

bool IsHIDAPIDevice(const SDL_GUID *guid) {
    return guid->data[14] == 'h';
}