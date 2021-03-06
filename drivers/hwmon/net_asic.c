/*
 * net_asic.c - The i2c driver for net_asic
 *
 * Copyright 2019-present Facebook. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#define BUILD_U64(_high, _low) (((u_int64_t)(_high) << 32) | (_low))

#define NET_ASIC_DELAY     10
#define NET_NCSI_MSG_LEN   4

#define HEARTBEAT_REFRESH_INTERVAL (HZ / 10)
#define NET_ASIC_HEARTBEAT_LSB_REG  0x108
#define NET_ASIC_HEARTBEAT_MSB_REG  0x10C

enum sdk_status {
  NOT_RUNNING,
  RUNNING,
};

struct heart_beat {
  /*
   * When SDK is running, the heart beat value should keep increasing
   * every 100ms, or it should be keep the same.
   */
  u_int64_t curr;
  u_int64_t last;
};

struct net_asic_data {
	struct i2c_client *client;
	struct mutex update_lock; /* mutex protect updates */
  unsigned long last_updated;	/* In jiffies */
  struct heart_beat heartbeat;
  unsigned int sdk_status;
};

/*
 * Check sdk is running or not
 * return value: 1-running, 0-not running
 * When SDK is running, the heart beat value should keep increasing
 * every 100ms, or it should be keep the same.
 */
static inline int sdk_is_running(struct heart_beat heartbeat)
{
  return heartbeat.curr > heartbeat.last ? RUNNING : NOT_RUNNING;
}

static int net_asic_i2c_read(struct net_asic_data *data,
                             unsigned int reg, unsigned int *value)
{
  struct i2c_client *client = data->client;
  char send_buf[NET_NCSI_MSG_LEN], read_buf[NET_NCSI_MSG_LEN];
  int index, bit_shift;

  for (index = 0; index < NET_NCSI_MSG_LEN; index++) {
    // Convert 32-bits address to buffer
    bit_shift = (NET_NCSI_MSG_LEN - 1 - index) * 8;
    send_buf[index] = (reg & (0xff << bit_shift)) >> bit_shift;
  }

  mutex_lock(&data->update_lock);
  if(i2c_master_send(client, send_buf, NET_NCSI_MSG_LEN) != NET_NCSI_MSG_LEN) {
    goto err_exit;
  }
  if(i2c_master_recv(client, read_buf, NET_NCSI_MSG_LEN) != NET_NCSI_MSG_LEN) {
    goto err_exit;
  }
  mutex_unlock(&data->update_lock);

  *value = 0;
  for (index = 0; index < NET_NCSI_MSG_LEN; index++) {
    // Convert read buffer to 32-bits value
    bit_shift = (NET_NCSI_MSG_LEN - 1 - index) * 8;
    *value |= (read_buf[index] << bit_shift);
  }

  return 0;

err_exit:
  mutex_unlock(&data->update_lock);
  return -1;
}

static int net_asic_heartbeat_update(struct net_asic_data *data)
{
  u_int32_t lsb, msb;

  if(time_after(jiffies, data->last_updated + HEARTBEAT_REFRESH_INTERVAL)) {
    if(net_asic_i2c_read(data, NET_ASIC_HEARTBEAT_LSB_REG, &lsb))
      goto err_exit;
    if(net_asic_i2c_read(data, NET_ASIC_HEARTBEAT_MSB_REG, &msb))
      goto err_exit;

    data->last_updated = jiffies;
    data->heartbeat.curr = BUILD_U64(msb, lsb);
    if(sdk_is_running(data->heartbeat) == RUNNING) {
      data->sdk_status = RUNNING;
    } else {
      data->sdk_status = NOT_RUNNING;
    }
    data->heartbeat.last = data->heartbeat.curr;
  }

  return 0;

err_exit:
  return -1;
}

static int net_asic_sdk_status_show(struct device *dev, struct device_attribute *dev_attr,
                                    char *buf)
{
  struct net_asic_data *data = dev_get_drvdata(dev);

  if(net_asic_heartbeat_update(data))
    return -1;

  return sprintf(buf, "%d\n", data->sdk_status == RUNNING ? 1 : 0);
}

/* sysfs callback function */
static ssize_t net_asic_temp_show(struct device *dev, struct device_attribute *dev_attr,
                                  char *buf)
{
  struct sensor_device_attribute *attr = to_sensor_dev_attr(dev_attr);
  struct net_asic_data *data = dev_get_drvdata(dev);
  int value = -1;

  int file_index = attr->index;
  int reg = file_index * NET_NCSI_MSG_LEN;
  if(net_asic_i2c_read(data, reg, &value))
    goto err_exit;

  /*
   * Check sdk is running or not
   * return value: 1-running, 0-not running
   * When SDK is running, the heart beat value should keep increasing
   * every 100ms, or it should be keep the same.
   * Only when SDK is running, the temperature values are valid.
   */
  if(net_asic_heartbeat_update(data))
    return -1;

  /* if SDK is running in the last 100ms, return the temperature */
  if(data->sdk_status == RUNNING)
    return sprintf(buf, "%d\n", value);

err_exit:
    return -1;
}

static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, net_asic_temp_show, NULL, 1);
static SENSOR_DEVICE_ATTR(temp2_input, S_IRUGO, net_asic_temp_show, NULL, 2);
static SENSOR_DEVICE_ATTR(temp3_input, S_IRUGO, net_asic_temp_show, NULL, 3);
static SENSOR_DEVICE_ATTR(temp4_input, S_IRUGO, net_asic_temp_show, NULL, 4);
static SENSOR_DEVICE_ATTR(temp5_input, S_IRUGO, net_asic_temp_show, NULL, 5);
static SENSOR_DEVICE_ATTR(temp6_input, S_IRUGO, net_asic_temp_show, NULL, 6);
static SENSOR_DEVICE_ATTR(temp7_input, S_IRUGO, net_asic_temp_show, NULL, 7);
static SENSOR_DEVICE_ATTR(temp8_input, S_IRUGO, net_asic_temp_show, NULL, 8);
static SENSOR_DEVICE_ATTR(temp9_input, S_IRUGO, net_asic_temp_show, NULL, 9);
static SENSOR_DEVICE_ATTR(temp10_input, S_IRUGO, net_asic_temp_show, NULL, 10);
static SENSOR_DEVICE_ATTR(sdk_status, S_IRUGO, net_asic_sdk_status_show, NULL, 0);

static struct attribute *net_asic_attrs[] = {
  &sensor_dev_attr_temp1_input.dev_attr.attr,
  &sensor_dev_attr_temp2_input.dev_attr.attr,
  &sensor_dev_attr_temp3_input.dev_attr.attr,
  &sensor_dev_attr_temp4_input.dev_attr.attr,
  &sensor_dev_attr_temp5_input.dev_attr.attr,
  &sensor_dev_attr_temp6_input.dev_attr.attr,
  &sensor_dev_attr_temp7_input.dev_attr.attr,
  &sensor_dev_attr_temp8_input.dev_attr.attr,
  &sensor_dev_attr_temp9_input.dev_attr.attr,
  &sensor_dev_attr_temp10_input.dev_attr.attr,
  &sensor_dev_attr_sdk_status.dev_attr.attr,
  NULL,
};
ATTRIBUTE_GROUPS(net_asic);

static int net_asic_probe(struct i2c_client *client,
                          const struct i2c_device_id *id)
{
  struct net_asic_data *data;
  struct device *dev = &client->dev;
  struct device *hwmon_dev;
  u_int32_t lsb = 0xffffffff;
  u_int32_t msb = 0xffffffff;

  data = devm_kzalloc(dev, sizeof(struct net_asic_data), GFP_KERNEL);
  if (!data)
    return -ENOMEM;

  if (!i2c_verify_client(dev)) {
    return -1;
  }
  data->client = client;
  mutex_init(&data->update_lock);

  net_asic_i2c_read(data, NET_ASIC_HEARTBEAT_LSB_REG, &lsb);
  net_asic_i2c_read(data, NET_ASIC_HEARTBEAT_MSB_REG, &msb);
  data->heartbeat.last = BUILD_U64(msb, lsb);
  data->last_updated = jiffies;
  data->sdk_status = NOT_RUNNING;

  hwmon_dev = devm_hwmon_device_register_with_groups(dev, client->name,
                                                     data, net_asic_groups);
  return PTR_ERR_OR_ZERO(hwmon_dev);
}

/* net_asic id */
static const struct i2c_device_id net_asic_id[] = {
  {"net_asic", 0},
  { },
};
MODULE_DEVICE_TABLE(i2c, net_asic_id);

static struct i2c_driver net_asic_driver = {
  .driver = {
    .name = "net_asic",
  },
  .probe    = net_asic_probe,
  .id_table = net_asic_id,
};

module_i2c_driver(net_asic_driver);

MODULE_AUTHOR("Tianhui Xu <tianhui@celestica.com>");
MODULE_DESCRIPTION("NET_ASIC Driver");
MODULE_LICENSE("GPL");
