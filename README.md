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
| Left Pad Click       | BTN_THUMBL             |
| Right Pad Click      | BTN_THUMBR             |
| Joystick Click       | 0x13f                  |
| Joystick             | ABS_X ,ABS_Y           |
| Left Pad             | ABS_HAT0X, ABS_HAT0Y   |
| Right Pad            | ABS_HAT1X, ABS_HAT1Y   |
| Left Trigger         | ABS_BRAKE              |
| Right Trigger        | ABS_GAS                |
| Accelerometer (tilt) | ABS_TILT_X, ABS_TILT_Y |
| Gyroscope            | REL_RX, REL_RY, REL_RZ |


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

For a more permanent installation, install the module with `make install` and add the rules in `90-valve-sc-rules` for automatically rebind any Steam Controller.


Testing
-------

The driver will create input devices named `Valve Software Steam Controller (<serialnumber>)`. Devices simply named `Valve Software Steam Controller` are the generic mouse and keyboard devices used by the uninitialized controller.

Use `evtest` for testing the event device, or `jstest` for testing the js device.


Sysfs attributes
----------------

The driver expose some attributes in the sysfs for each device.

 - **automouse**: enable or disable the right pad behaving like a mouse. Accepted values are *on* or *off*.
 - **autobuttons**: enable or disable the buttons acting as keys or mouse buttons. Accepted values are *on* or *off*.
 - **orientation**: enable or disable orientation sensors (accelerometer and gyroscope). Accepted values are *on* or *off*.
 - **centertouchpads**: enable or disable centering the touch pads when released (for using them as joysticks). Accepted values are *on* or *off*

