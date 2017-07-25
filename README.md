Steam Controller Linux Kernel Driver
====================================

This driver exposes Steam Controllers as generic gamepads. It does not allow to remap the controller.

Input mappings are:

| Steam Controller     | Linux input code       |
| -------------------- | ---------------------- |
| Button A             | BTN_SOUTH              |
| Button B             | BTN_EAST               |
| Button X             | BTN_WEST               |
| Button Y             | BTN_NORTH              |
| Button <             | BTN_SELECT             |
| Button Steam         | BTN_MODE               |
| Button >             | BTN_START              |
| Left Bumper          | BTN_TL                 |
| Right Bumper         | BTN_TR                 |
| Left Trigger full    | BTN_TL2                |
| Right Trigger full   | BTN_TR2                |
| Left Grip            | BTN_C                  |
| Right Grip           | BTN_Z                  |
| Left Pad Click       | BTN_GAMEPAD+15 (0x13f) |
| Right Pad Click      | BTN_THUMBR             |
| Joystick Click       | BTN_THUMBL             |
| Joystick             | ABS_X, ABS_Y           |
| Left Pad             | ABS_HAT0X, ABS_HAT0Y   |
| Right Pad            | ABS_RX, ABS_RY         |
| Left Trigger         | ABS_HAT2Y              |
| Right Trigger        | ABS_HAT2X              |

Accelerometer and gyroscope events are sent through a second input device (called "Valve Software Steam Controller Accelerometer") using, respectively, `ABS_X`, `ABS_Y`, `ABS_Z` and `ABS_RX`, `ABS_RY`, `ABS_RZ`. The sensors are only enabled when the input device is opened in order to reduce power consumption.


Building
--------

Use `make`.


Installation
------------

Without patching the kernel hid-generic will still manage the device. You will need to rebind the driver for hid-valve-sc to take over.

You may prefer to manually load the module with `insmod hid-valve-sc.ko` and then manually rebind the desired device:

```
tee /sys/bus/hid/drivers/hid-generic/unbind <<< "0003:<vendorid>:<productid>:<devnum>"
tee /sys/bus/hid/drivers/valve-sc/bind <<< "0003:<vendorid>:<productid>:<devnum>"
```

For a more permanent installation, install the module with `make install` and add the rules in `90-valve-sc-rules` for automatically rebindind any Steam Controller to the valve-sc driver.


Installation with DKMS
----------------------

Copy the files to `/usr/src/hid-valve-sc-0.1/` and run `dkms install hid-valve-sc/0.1`.


Testing
-------

The driver will create input devices named `Valve Software Steam Controller (<serialnumber>)`. Devices simply named `Valve Software Steam Controller` are the generic mouse and keyboard devices used by the uninitialized controller.

Use `evtest` for testing the event device, or `jstest` for testing the js device.


Sysfs attributes
----------------

The driver expose some attributes in the sysfs for each device.

 - **automouse**: enable or disable the right pad behaving like a mouse. Accepted values are *on* or *off*.
 - **autobuttons**: enable or disable the buttons acting as keys or mouse buttons. Accepted values are *on* or *off*.
 - **centertouchpads**: enable or disable centering the touch pads when released (for using them as joysticks). Accepted values are *on* or *off*

