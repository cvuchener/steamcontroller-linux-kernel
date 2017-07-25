/*
 * HID driver for Valve Steam Controller
 *
 * Copyright (c) 2015 Clement Vuchener
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#define DEBUG

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/delay.h>

#include "hid-ids.h"

#define to_hid_device(pdev) container_of(pdev, struct hid_device, dev)

#define CONTROLLER_NAME	"Valve Software Steam Controller"
#define SENSOR_SUFFIX	" Accelerometer"

#define RAW_REPORT_DESC_SIZE	33
static const u8 raw_report_desc[RAW_REPORT_DESC_SIZE] = {
	0x06, 0x00, 0xFF,	/* Usage Page (FF00 - Vendor) */
	0x09, 0x01,		/* Usage (0001 - Vendor) */
	0xA1, 0x01,		/* Collection (Application) */
	0x15, 0x00,		/*  Logical Minimum (0) */
	0x26, 0xFF, 0x00,	/*  Logical Maximum (255) */
	0x75, 0x08,		/*  Report Size (8) */
	0x95, 0x40,		/*  Report Count (64) */
	0x09, 0x01,		/*  Usage (0001 - Vendor) */
	0x81, 0x02,		/*  Input (Data, Variable, Absolute) */
	0x95, 0x40,		/*  Report Count (64) */
	0x09, 0x01,		/*  Usage (0001 - Vendor) */
	0x91, 0x02,		/*  Output (Data, Variable, Absolute) */
	0x95, 0x40,		/*  Report Count (64) */
	0x09, 0x01,		/*  Usage (0001 - Vendor) */
	0xB1, 0x02,		/*  Feature (Data, Variable, Absolute) */
	0xC0,			/* End Collection */
};

/* Input report offsets */
#define SC_OFFSET_TYPE		2
#define SC_OFFSET_LENGTH	3
#define SC_OFFSET_SEQNUM	4
#define SC_OFFSET_BUTTONS	7
#define SC_OFFSET_TRIGGERS_8	11
#define SC_OFFSET_LEFT_AXES	16
#define SC_OFFSET_RIGHT_AXES	20
#define SC_OFFSET_TRIGGERS_16	24
#define SC_OFFSET_ACCEL		28
#define SC_OFFSET_GYRO		34
#define SC_OFFSET_QUATERNION	40
#define SC_OFFSET_LEFT_TOUCHPAD	58

/* Button mask */
#define SC_BTN_TOUCH_RIGHT	0x10000000
#define SC_BTN_TOUCH_LEFT	0x08000000
#define SC_BTN_CLICK_RIGHT	0x04000000
#define SC_BTN_CLICK_LEFT	0x02000000
#define SC_BTN_GRIP_RIGHT	0x01000000
#define SC_BTN_GRIP_LEFT	0x00800000
#define SC_BTN_START		0x00400000
#define SC_BTN_MODE		0x00200000
#define SC_BTN_SELECT		0x00100000
#define SC_BTN_A		0x00008000
#define SC_BTN_X		0x00004000
#define SC_BTN_B		0x00002000
#define SC_BTN_Y		0x00001000
#define SC_BTN_SHOULDER_LEFT	0x00000800
#define SC_BTN_SHOULDER_RIGHT	0x00000400
#define SC_BTN_TRIGGER_LEFT	0x00000200
#define SC_BTN_TRIGGER_RIGHT	0x00000100

#define BTN_STICK_CLICK	BTN_GAMEPAD+0xf

#define SC_FEATURE_REPORT_SIZE 65

#define SC_FEATURE_DISABLE_AUTO_BUTTONS	0x81
#define SC_FEATURE_ENABLE_AUTO_BUTTONS	0x85
#define SC_FEATURE_SETTINGS	0x87
#define SC_FEATURE_GET_SERIAL	0xae
#define SC_FEATURE_GET_CONNECTION_STATE	0xb4

#define SC_SETTINGS_AUTOMOUSE	0x08
#define SC_SETTINGS_AUTOMOUSE_ON	0x00
#define SC_SETTINGS_AUTOMOUSE_OFF	0x07
#define SC_SETTINGS_ORIENTATION	0x30
#define SC_SETTINGS_ORIENTATION_TILT_X	0x01
#define SC_SETTINGS_ORIENTATION_TILT_Y	0x02
#define SC_SETTINGS_ORIENTATION_ACCEL	0x04
#define SC_SETTINGS_ORIENTATION_Q	0x08
#define SC_SETTINGS_ORIENTATION_GYRO	0x10

#define SC_ACCEL_RES_PER_G	0x4000

struct valve_sc_device {
	struct hid_device *hdev;
	bool parse_raw_report;
	bool connected;
	struct input_dev *input;
	struct input_dev *sensor;
	bool center_touchpads;
	bool automouse;
	bool autobuttons;
	u8 orientation;
	struct work_struct connect_work;
	struct work_struct disconnect_work;
	char *uniq;
};

static int valve_sc_send_request(struct valve_sc_device *sc, u8 report_id,
				 const u8 *params, int params_size,
				 u8 *answer, int *answer_size)
{
	int ret;
	struct hid_device *hdev = sc->hdev;
	u8 *report;

	if (params_size > 62)
		return -EINVAL;

	report = kmalloc (SC_FEATURE_REPORT_SIZE, GFP_KERNEL);
	if (!report)
		return -ENOMEM;

	report[0] = 0;
	report[1] = report_id;
	report[2] = params_size;
	memcpy(&report[3], params, params_size);

	ret = hid_hw_raw_request(hdev, 0, report, SC_FEATURE_REPORT_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		hid_warn(hdev, "Error sending feature: %d\n", -ret);
		goto out;
	}
	if (ret != SC_FEATURE_REPORT_SIZE) {
		hid_warn(hdev, "Sent incomplete feature.\n");
		ret = -EIO;
		goto out;
	}

	if (!answer) {
		ret = 0;
		goto out;
	}

	msleep(50);

	ret = hid_hw_raw_request(hdev, 0, report, SC_FEATURE_REPORT_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_warn(hdev, "Error receiving feature: %d\n", -ret);
		goto out;
	}
	if (ret != SC_FEATURE_REPORT_SIZE) {
		hid_warn(hdev, "Received incomplete feature.\n");
		ret = -EIO;
		goto out;
	}

	if (report[1] != report_id) {
		hid_warn(hdev, "Invalid feature id.\n");
		ret = -EIO;
		goto out;
	}

	*answer_size = report[2];
	if (*answer_size > 61) {
		hid_warn(hdev, "Invalid answer size: %d\n", *answer_size);
		ret = -EIO;
		goto out;
	}
	memcpy(answer, &report[3], *answer_size);
	ret = 0;

out:
	kfree (report);
	return ret;
}

static ssize_t valve_sc_show_automouse(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct valve_sc_device *sc = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n", sc->automouse ? "on" : "off");
}

static ssize_t valve_sc_store_automouse(struct device *dev,
				        struct device_attribute *attr,
				        const char *buf, size_t count)
{
	int ret;
	struct valve_sc_device *sc = dev_get_drvdata(dev);
	struct hid_device *hdev = to_hid_device(dev);
	u8 params[3];

	if (strncmp(buf, "on", 2) == 0)
		sc->automouse = true;
	else if (strncmp(buf, "off", 3) == 0)
		sc->automouse = false;
	else
		return -EINVAL;

	if (sc->connected) {
		params[0] = SC_SETTINGS_AUTOMOUSE;
		if (sc->automouse)
			params[1] = SC_SETTINGS_AUTOMOUSE_ON;
		else
			params[1] = SC_SETTINGS_AUTOMOUSE_OFF;
		params[2] = 0;
		ret = valve_sc_send_request(sc, SC_FEATURE_SETTINGS,
					    params, sizeof(params),
					    NULL, NULL);
		if (ret < 0)
			hid_warn(hdev, "Error while setting automouse: %d\n", -ret);
	}
	return count;
}

static ssize_t valve_sc_show_autobuttons(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct valve_sc_device *sc = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n", sc->autobuttons ? "on" : "off");
}

static ssize_t valve_sc_store_autobuttons(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	int ret;
	struct valve_sc_device *sc = dev_get_drvdata(dev);
	struct hid_device *hdev = to_hid_device(dev);

	if (strncmp(buf, "on", 2) == 0)
		sc->autobuttons = true;
	else if (strncmp(buf, "off", 3) == 0)
		sc->autobuttons = false;
	else
		return -EINVAL;

	if (sc->connected) {
		u8 feature;
		if (sc->autobuttons)
			feature = SC_FEATURE_ENABLE_AUTO_BUTTONS;
		else
			feature = SC_FEATURE_DISABLE_AUTO_BUTTONS;

		ret = valve_sc_send_request(sc, feature,
					    NULL, 0,
					    NULL, NULL);
		if (ret < 0)
			hid_warn(hdev, "Error while setting autobuttons: %d\n", -ret);
	}
	return count;
}

static ssize_t valve_sc_show_center_touchpads(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct valve_sc_device *sc = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n", sc->center_touchpads ? "on" : "off");
}

static ssize_t valve_sc_store_center_touchpads(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct valve_sc_device *sc = dev_get_drvdata(dev);

	if (strncmp(buf, "on", 2) == 0)
		sc->center_touchpads = true;
	else if (strncmp(buf, "off", 3) == 0)
		sc->center_touchpads = false;
	else
		return -EINVAL;
	return count;
}

static DEVICE_ATTR(automouse, 0644,
		   valve_sc_show_automouse, valve_sc_store_automouse);
static DEVICE_ATTR(autobuttons, 0644,
		   valve_sc_show_autobuttons, valve_sc_store_autobuttons);
static DEVICE_ATTR(center_touchpads, 0644,
		   valve_sc_show_center_touchpads, valve_sc_store_center_touchpads);

static struct attribute *valve_sc_attrs[] = {
	&dev_attr_automouse.attr,
	&dev_attr_autobuttons.attr,
	&dev_attr_center_touchpads.attr,
	NULL
};

static const struct attribute_group valve_sc_attr_group = {
	.attrs = valve_sc_attrs,
};

#define SC_REPORT_BTN(input, buttons, sc_btn, code) \
	input_report_key(input, code, (buttons & sc_btn ? 1 : 0))

static void valve_sc_parse_input_events(struct valve_sc_device *sc,
					const u8 *raw_data)
{
	unsigned int i, axis;
	u32 buttons = 0;
	s16 left[2] = {0}, right[2] = {0};
	u8 triggers[2] = {0};
	s16 accel[3] = {0};
	s16 gyro[3] = {0};

	/* Read fields */
	for (i = 0; i < sizeof (u32); ++i)
		buttons |= raw_data[SC_OFFSET_BUTTONS+i] << i*8;

	for (axis = 0; axis < 2; ++axis) {
		triggers[axis] = raw_data[SC_OFFSET_TRIGGERS_8+axis];
		for (i = 0; i < sizeof (s16); ++i) {
			left[axis] |= raw_data[SC_OFFSET_LEFT_AXES+2*axis+i] << i*8;
			right[axis] |= raw_data[SC_OFFSET_RIGHT_AXES+2*axis+i] << i*8;
		}
	}

	for (axis = 0; axis < 3; ++axis) {
		for (i = 0; i < sizeof(s16); ++i) {
			accel[axis] |= raw_data[SC_OFFSET_ACCEL+2*axis+i] << i*8;
			gyro[axis] |= raw_data[SC_OFFSET_GYRO+2*axis+i] << i*8;
		}
	}

	if (sc->input) {
		if (buttons & SC_BTN_TOUCH_LEFT) {
			input_report_abs(sc->input, ABS_HAT0X, left[0]);
			input_report_abs(sc->input, ABS_HAT0Y, -left[1]);
		}
		else if (sc->center_touchpads && left[0] == 0 && left[1] == 0) {
			/* Left touch pad release is not detected if the stick
			 * is not centered at the same time. Since they are used
			 * with the same finger, it should not happen often. */
			input_report_abs(sc->input, ABS_HAT0X, 0);
			input_report_abs(sc->input, ABS_HAT0Y, 0);
		}

		if (sc->center_touchpads || buttons & SC_BTN_TOUCH_RIGHT) {
			input_report_abs(sc->input, ABS_HAT1X, right[0]);
			input_report_abs(sc->input, ABS_HAT1Y, -right[1]);
		}

		input_report_abs(sc->input, ABS_BRAKE, triggers[0]);
		input_report_abs(sc->input, ABS_GAS, triggers[1]);

		if (buttons & SC_BTN_TOUCH_LEFT) {
			/* Left events are touchpad events */
			SC_REPORT_BTN(sc->input, buttons,
				      SC_BTN_CLICK_LEFT, BTN_THUMBL);
		}
		else {
			/* Left events are stick events */
			SC_REPORT_BTN(sc->input, buttons,
				      SC_BTN_CLICK_LEFT, BTN_STICK_CLICK);
			input_report_abs(sc->input, ABS_X, left[0]);
			input_report_abs(sc->input, ABS_Y, -left[1]);
		}
		if (buttons & SC_BTN_TOUCH_RIGHT) {
			SC_REPORT_BTN(sc->input, buttons,
				      SC_BTN_CLICK_RIGHT, BTN_THUMBR);
		}
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_A, BTN_SOUTH);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_B, BTN_EAST);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_X, BTN_WEST);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_Y, BTN_NORTH);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_SELECT, BTN_SELECT);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_MODE, BTN_MODE);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_START, BTN_START);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_SHOULDER_LEFT, BTN_TL);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_SHOULDER_RIGHT, BTN_TR);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_TRIGGER_LEFT, BTN_TL2);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_TRIGGER_RIGHT, BTN_TR2);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_GRIP_LEFT, BTN_C);
		SC_REPORT_BTN(sc->input, buttons, SC_BTN_GRIP_RIGHT, BTN_Z);

		input_sync(sc->input);
	}

	if (sc->sensor) {
		input_report_abs(sc->sensor, ABS_X, accel[0]);
		input_report_abs(sc->sensor, ABS_Y, accel[1]);
		input_report_abs(sc->sensor, ABS_Z, accel[2]);
		input_report_abs(sc->sensor, ABS_RX, gyro[0]);
		input_report_abs(sc->sensor, ABS_RY, gyro[1]);
		input_report_abs(sc->sensor, ABS_RZ, gyro[2]);
		input_sync(sc->sensor);
	}
}

static int valve_sc_init_input(struct valve_sc_device *sc)
{
	int ret;
	struct hid_device *hdev = sc->hdev;

	sc->input = input_allocate_device();
	if (!sc->input) {
		hid_err(hdev, "Failed to allocate input device.\n");
		return -ENOMEM;
	}

	sc->input->dev.parent = &hdev->dev;
	sc->input->id.bustype = hdev->bus;
	sc->input->id.vendor = hdev->vendor;
	sc->input->id.product = hdev->product;
	sc->input->id.version = hdev->version;
	sc->input->name = CONTROLLER_NAME;
	if (sc->uniq)
		sc->input->uniq = sc->uniq;

	set_bit(EV_KEY, sc->input->evbit);
	set_bit(BTN_SOUTH, sc->input->keybit);
	set_bit(BTN_EAST, sc->input->keybit);
	set_bit(BTN_WEST, sc->input->keybit);
	set_bit(BTN_NORTH, sc->input->keybit);
	set_bit(BTN_SELECT, sc->input->keybit);
	set_bit(BTN_MODE, sc->input->keybit);
	set_bit(BTN_START, sc->input->keybit);
	set_bit(BTN_TL, sc->input->keybit);
	set_bit(BTN_TR, sc->input->keybit);
	set_bit(BTN_TL2, sc->input->keybit);
	set_bit(BTN_TR2, sc->input->keybit);
	set_bit(BTN_C, sc->input->keybit); /* Left grip */
	set_bit(BTN_Z, sc->input->keybit); /* Right grip */
	set_bit(BTN_THUMBL, sc->input->keybit);
	set_bit(BTN_THUMBR, sc->input->keybit);
	set_bit(BTN_STICK_CLICK, sc->input->keybit);

	set_bit(EV_ABS, sc->input->evbit);
	/* Stick */
	set_bit(ABS_X, sc->input->absbit);
	set_bit(ABS_Y, sc->input->absbit);
	input_set_abs_params(sc->input, ABS_X, -32767, 32767, 100, 100);
	input_set_abs_params(sc->input, ABS_Y, -32767, 32767, 100, 100);
	/* Touchpads */
	set_bit(ABS_HAT0X, sc->input->absbit);
	set_bit(ABS_HAT0Y, sc->input->absbit);
	set_bit(ABS_HAT1X, sc->input->absbit);
	set_bit(ABS_HAT1Y, sc->input->absbit);
	input_set_abs_params(sc->input, ABS_HAT0X, -32767, 32767, 500, 1000);
	input_set_abs_params(sc->input, ABS_HAT0Y, -32767, 32767, 500, 1000);
	input_set_abs_params(sc->input, ABS_HAT1X, -32767, 32767, 500, 1000);
	input_set_abs_params(sc->input, ABS_HAT1Y, -32767, 32767, 500, 1000);
	/* Triggers */
	set_bit(ABS_GAS, sc->input->absbit);
	set_bit(ABS_BRAKE, sc->input->absbit);
	input_set_abs_params(sc->input, ABS_GAS, 0, 255, 2, 1);
	input_set_abs_params(sc->input, ABS_BRAKE, 0, 255, 2, 1);

	ret = input_register_device(sc->input);
	if (ret != 0) {
		hid_err(hdev, "Failed to register input device: %d.\n", -ret);
		input_free_device(sc->input);
		sc->input = NULL;
		return ret;
	}

	return 0;
}

static int valve_sc_update_orientation_setting(struct valve_sc_device *sc)
{
	int ret = 0;
	struct hid_device *hdev = sc->hdev;
	u8 params[3];

	if (sc->connected) {
		params[0] = SC_SETTINGS_ORIENTATION;
		params[1] = sc->orientation;
		params[2] = 0;
		ret = valve_sc_send_request(sc, SC_FEATURE_SETTINGS,
					    params, sizeof(params),
					    NULL, NULL);
		if (ret < 0)
			hid_warn(hdev, "Error while setting orientation: %d\n", -ret);
	}

	return ret;
}

static int valve_sc_open_sensor(struct input_dev *dev)
{
	struct valve_sc_device *sc = input_get_drvdata(dev);
	sc->orientation |= SC_SETTINGS_ORIENTATION_ACCEL | SC_SETTINGS_ORIENTATION_GYRO;
	valve_sc_update_orientation_setting(sc);
	return 0;
}

static void valve_sc_close_sensor(struct input_dev *dev)
{
	struct valve_sc_device *sc = input_get_drvdata(dev);
	sc->orientation &= ~SC_SETTINGS_ORIENTATION_ACCEL & ~SC_SETTINGS_ORIENTATION_GYRO;
	valve_sc_update_orientation_setting(sc);
}

static int valve_sc_init_sensor(struct valve_sc_device *sc)
{
	int ret;
	struct hid_device *hdev = sc->hdev;

	sc->sensor = input_allocate_device();
	if (!sc->sensor) {
		hid_err(hdev, "Failed to allocate input device for sensors.\n");
		return -ENOMEM;
	}

	input_set_drvdata(sc->sensor, sc);
	sc->sensor->dev.parent = &hdev->dev;
	sc->sensor->open = valve_sc_open_sensor;
	sc->sensor->close = valve_sc_close_sensor;
	sc->sensor->id.bustype = hdev->bus;
	sc->sensor->id.vendor = hdev->vendor;
	sc->sensor->id.product = hdev->product;
	sc->sensor->id.version = hdev->version;
	sc->sensor->name = CONTROLLER_NAME SENSOR_SUFFIX;
	if (sc->uniq)
		sc->sensor->uniq = sc->uniq;

	set_bit(EV_ABS, sc->sensor->evbit);
	set_bit(ABS_X, sc->sensor->absbit);
	set_bit(ABS_Y, sc->sensor->absbit);
	set_bit(ABS_Z, sc->sensor->absbit);
	input_set_abs_params(sc->sensor, ABS_X, -32767, 32767, 0, 0);
	input_set_abs_params(sc->sensor, ABS_Y, -32767, 32767, 0, 0);
	input_set_abs_params(sc->sensor, ABS_Z, -32767, 32767, 0, 0);
	input_abs_set_res(sc->sensor, ABS_X, SC_ACCEL_RES_PER_G);
	input_abs_set_res(sc->sensor, ABS_Y, SC_ACCEL_RES_PER_G);
	input_abs_set_res(sc->sensor, ABS_Z, SC_ACCEL_RES_PER_G);
	set_bit(ABS_RX, sc->sensor->absbit);
	set_bit(ABS_RY, sc->sensor->absbit);
	set_bit(ABS_RZ, sc->sensor->absbit);
	input_set_abs_params(sc->sensor, ABS_RX, -32767, 32767, 0, 0);
	input_set_abs_params(sc->sensor, ABS_RY, -32767, 32767, 0, 0);
	input_set_abs_params(sc->sensor, ABS_RZ, -32767, 32767, 0, 0);
	/* TODO: gyroscope resolution */
	set_bit(INPUT_PROP_ACCELEROMETER, sc->sensor->propbit);

	ret = input_register_device(sc->sensor);
	if (ret != 0) {
		hid_err(hdev, "Failed to register sensors input device: %d.\n", -ret);
		input_free_device(sc->sensor);
		sc->sensor = NULL;
		return ret;
	}

	return 0;
}

static int valve_sc_init_device(struct valve_sc_device *sc)
{
	int ret;
	struct hid_device *hdev = sc->hdev;
	u8 params[6];
	u8 feature;
	u8 serial[64];
	int serial_len;

	hid_info(hdev, "Initializing device.\n");

	/* Retrieve controller serial */
	serial[0] = 1;
	ret = valve_sc_send_request(sc, SC_FEATURE_GET_SERIAL,
				    serial, 21,
				    serial, &serial_len);
	if (ret < 0 || serial_len < 1 || serial_len > 62) {
		hid_warn(hdev, "Error while get controller serial: %d\n", -ret);
		serial_len = 1;
		serial[1] = '\0';
	}
	else {
		serial[serial_len] = '\0';
	}

	sc->uniq = kmalloc(serial_len, GFP_KERNEL);
	if (!sc->uniq) {
		hid_warn(hdev, "Failed to allocate memory for uniq.\n");
	} else {
		strncpy(sc->uniq, &serial[1], serial_len-1);
		sc->uniq[serial_len-1] = '\0';
	}

	/* Set mouse mode for right pad */
	params[0] = SC_SETTINGS_AUTOMOUSE;
	if (sc->automouse)
		params[1] = SC_SETTINGS_AUTOMOUSE_ON;
	else
		params[1] = SC_SETTINGS_AUTOMOUSE_OFF;
	params[2] = 0;
	params[3] = SC_SETTINGS_ORIENTATION;
	params[4] = sc->orientation;
	params[5] = 0;
	ret = valve_sc_send_request(sc, SC_FEATURE_SETTINGS,
				    params, 6,
				    NULL, NULL);
	if (ret < 0)
		hid_warn(hdev, "Error while disabling mouse: %d\n", -ret);

	/* Disable buttons acting as keys */
	if (sc->autobuttons)
		feature = SC_FEATURE_ENABLE_AUTO_BUTTONS;
	else
		feature = SC_FEATURE_DISABLE_AUTO_BUTTONS;

	ret = valve_sc_send_request(sc, feature,
				    NULL, 0,
				    NULL, NULL);
	if (ret < 0)
		hid_warn(hdev, "Error while setting auto buttons: %d\n", -ret);

	ret = valve_sc_init_input(sc);
	if (ret < 0)
		hid_warn(hdev, "Failed to initialize input device: %d\n", -ret);

	ret = valve_sc_init_sensor(sc);
	if (ret < 0)
		hid_warn(hdev, "Failed to initialize sensors input device: %d\n", -ret);

	return 0;
}

static void valve_sc_connect_work(struct work_struct *work)
{
	struct valve_sc_device *sc = container_of(work, struct valve_sc_device,
						     connect_work);
	valve_sc_init_device(sc);
}

static void valve_sc_stop_device(struct valve_sc_device *sc)
{
	if (sc->input) {
		input_unregister_device(sc->input);
		input_free_device(sc->input);
		sc->input = NULL;
	}
	if (sc->sensor) {
		input_unregister_device(sc->sensor);
		input_free_device(sc->sensor);
		sc->sensor = NULL;
	}
	kfree(sc->uniq);
	sc->uniq = NULL;
}

static void valve_sc_disconnect_work(struct work_struct *work)
{
	struct valve_sc_device *sc = container_of(work, struct valve_sc_device,
						     disconnect_work);
	valve_sc_stop_device(sc);
}

static int valve_sc_raw_event(struct hid_device *hdev, struct hid_report *report,
			      u8 *raw_data, int size)
{
	struct valve_sc_device *sc = hid_get_drvdata (hdev);

	if (sc->parse_raw_report && size == 64) {
		switch (raw_data[SC_OFFSET_TYPE]) {
		case 0x01: /* Input events */
			if (raw_data[SC_OFFSET_LENGTH] != 60)
				hid_warn(hdev, "Wrong input event length.\n");
			if (sc->input || sc->sensor)
				valve_sc_parse_input_events(sc, raw_data);
			break;

		case 0x03: /* Connection events */
			if (raw_data[SC_OFFSET_LENGTH] != 1)
				hid_warn(hdev, "Wrong connection event length.\n");
			switch (raw_data[4]) {
			case 0x01: /* Disconnected device */
				hid_dbg(hdev, "Disconnected event\n");
				if (sc->connected) {
					sc->connected = false;
					schedule_work(&sc->disconnect_work);
				}
				break;

			case 0x02: /* Connected device */
				hid_dbg(hdev, "Connected event\n");
				if (!sc->connected) {
					sc->connected = true;
					schedule_work(&sc->connect_work);
				}
				break;

			case 0x03: /* Paired device*/
			default:
				break;
			}
			break;

		default:
			break;
		}
	}
	return 0;
}

static int valve_sc_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	struct valve_sc_device *sc;
	char answer[64];
	int answer_len;

	sc = devm_kzalloc(&hdev->dev, sizeof (struct valve_sc_device),
			       GFP_KERNEL);
	if (!sc) {
		hid_err (hdev, "cannot alloc driver data\n");
		return -ENOMEM;
	}
	hid_set_drvdata(hdev, sc);

	sc->hdev = hdev;
	sc->automouse = false;
	sc->autobuttons = false;
	sc->orientation = 0;
	sc->center_touchpads = true;

	INIT_WORK(&sc->connect_work, valve_sc_connect_work);
	INIT_WORK(&sc->disconnect_work, valve_sc_disconnect_work);

	ret = hid_parse(hdev);
	if (ret != 0) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	if (hdev->rsize == RAW_REPORT_DESC_SIZE &&
	    strncmp (hdev->rdesc, raw_report_desc, RAW_REPORT_DESC_SIZE) == 0) {
		sc->parse_raw_report = true;

		ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
		if (ret != 0) {
			hid_err(hdev, "HW start failed\n");
			return ret;
		}

		ret = hid_hw_open(hdev);
		if (ret != 0) {
			hid_err(hdev, "HW open failed\n");
			return ret;
		}

		switch (id->product) {
		case USB_DEVICE_ID_STEAM_CONTROLLER:
			/* Wired device is always connected */
			sc->connected = true;
			valve_sc_init_device(sc);
			break;

		case USB_DEVICE_ID_STEAM_CONTROLLER_RECEIVER:
			/* Wireless will be initialized when connected */
			sc->connected = false;
			ret = valve_sc_send_request(sc, SC_FEATURE_GET_CONNECTION_STATE,
						 NULL, 0,
						 answer, &answer_len);
			if (ret < 0)
				hid_warn(hdev, "Error while getting connection state: %d\n", -ret);
			break;
		}

		ret = sysfs_create_group(&hdev->dev.kobj, &valve_sc_attr_group);
		if (ret != 0)
			hid_warn(hdev, "Failed to create sysfs attribute group.\n");
	}
	else {
		/* This is a generic mouse/keyboard interface */
		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret != 0) {
			hid_err(hdev, "HW start failed\n");
			return ret;
		}
	}

	return 0;
}

static void valve_sc_remove(struct hid_device *hdev)
{
	struct valve_sc_device *sc = hid_get_drvdata (hdev);

	sysfs_remove_group(&hdev->dev.kobj, &valve_sc_attr_group);

	cancel_work_sync(&sc->connect_work);
	cancel_work_sync(&sc->disconnect_work);

	if (sc->connected)
		valve_sc_stop_device(sc);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id valve_sc_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_VALVE,
			 USB_DEVICE_ID_STEAM_CONTROLLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_VALVE,
			 USB_DEVICE_ID_STEAM_CONTROLLER_RECEIVER) },
	{ }
};
MODULE_DEVICE_TABLE(hid, valve_sc_devices);

static struct hid_driver valve_sc_hid_driver = {
	.name = "valve-sc",
	.id_table = valve_sc_devices,
	.probe = valve_sc_probe,
	.remove = valve_sc_remove,
	.raw_event = valve_sc_raw_event,
};
module_hid_driver(valve_sc_hid_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Clement Vuchener");
MODULE_DESCRIPTION("HID driver for Valve Steam Controller");
