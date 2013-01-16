/* drivers/i2c/chips/akm8975.c - akm8975 compass driver
 *
 * Copyright (C) 2007-2008 HTC Corporation.
 * Author: Hou-Kun Chen <houkun.chen@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*
 * Revised by AKM 2009/04/02
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/gpio.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include <linux/akm8975.h>

#ifdef CONFIG_ANDROID_POWER
#include <linux/android_power.h>
#endif

#define AK8975DRV_CALL_DBG 0
#if AK8975DRV_CALL_DBG
#define FUNCDBG(msg)  printk(KERN_INFO "%s:%s\n", __FUNCTION__, msg);
#else
#define FUNCDBG(msg)
#endif

#define AK8975DRV_DATA_DBG 0
#define MAX_FAILURE_COUNT 10

struct akm8975_data {
  struct input_dev *input_dev;
  struct work_struct work;
#ifdef CONFIG_ANDROID_POWER
  android_early_suspend_t early_suspend;
#endif
};

static struct i2c_client *this_client;

/* Addresses to scan -- protected by sense_data_mutex */
//static char sense_data[RBUFF_SIZE];
static struct mutex sense_data_mutex;

static DECLARE_WAIT_QUEUE_HEAD(data_ready_wq);
static DECLARE_WAIT_QUEUE_HEAD(open_wq);

//static atomic_t data_ready;
static atomic_t open_count;
static atomic_t open_flag;
static atomic_t reserve_open_flag;

static atomic_t m_flag;
static atomic_t a_flag;
static atomic_t t_flag;
static atomic_t mv_flag;

//static int failure_count = 0;

static short akmd_delay = 0;

#ifdef CONFIG_ANDROID_POWER
static atomic_t suspend_flag = ATOMIC_INIT(0);
#endif

//static struct akm8975_platform_data *pdata;

/* following are the sysfs callback functions */
#define config_ctrl_reg(name,address) \
static ssize_t name##_show(struct device *dev, struct device_attribute *attr, \
			   char *buf) \
{ \
	struct i2c_client *client = to_i2c_client(dev); \
        return sprintf(buf, "%u\n", i2c_smbus_read_byte_data(client,address)); \
} \
static ssize_t name##_store(struct device *dev, struct device_attribute *attr, \
			    const char *buf,size_t count) \
{ \
	struct i2c_client *client = to_i2c_client(dev); \
	unsigned long val = simple_strtoul(buf, NULL, 10); \
	if (val > 0xff) \
		return -EINVAL; \
	i2c_smbus_write_byte_data(client,address, val); \
        return count; \
} \
static DEVICE_ATTR(name, S_IWUSR | S_IRUGO, name##_show, name##_store)

config_ctrl_reg(ms1, AK8975_REG_CNTL1);

static int AKI2C_RxData(char *rxData, int length)
{
  struct i2c_msg msgs[] = {
    {
     .addr = this_client->addr,
     .flags = 0,
     .len = 1,
     .buf = rxData,
     },
    {
     .addr = this_client->addr,
     .flags = I2C_M_RD,
     .len = length,
     .buf = rxData,
     },
  };

  FUNCDBG("called");

  if (i2c_transfer(this_client->adapter, msgs, 2) < 0) {
    printk(KERN_ERR "AKI2C_RxData: transfer error\n");
    return -EIO;
  } else
    return 0;
}

static int AKI2C_TxData(char *txData, int length)
{
  struct i2c_msg msg[] = {
    {
     .addr = this_client->addr,
     .flags = 0,
     .len = length,
     .buf = txData,
     },
  };

  FUNCDBG("called");

  if (i2c_transfer(this_client->adapter, msg, 1) < 0) {
    printk(KERN_ERR "AKI2C_TxData: transfer error\n");
    return -EIO;
  } else
    return 0;
}

static int AKECS_GetData(void)
{
//  char buffer[RBUFF_SIZE];
//  int ret;

  FUNCDBG("called");

  /*
  memset(buffer, 0, RBUFF_SIZE);
  buffer[0] = AK8975_REG_ST1;
  ret = AKI2C_RxData(buffer, RBUFF_SIZE);
  if (ret < 0)
    return ret;

  mutex_lock(&sense_data_mutex);
  memcpy(sense_data, buffer, sizeof(buffer));
  atomic_set(&data_ready, 1);
  wake_up(&data_ready_wq);
  mutex_unlock(&sense_data_mutex);
  */
  return 0;
}

static int AKECS_TransRBuff(char *rbuf, int size)
{
  FUNCDBG("called");

  if (size < RBUFF_SIZE + 1)
    return -EINVAL;
  /*
  wait_event_interruptible_timeout(data_ready_wq,
                                   atomic_read(&data_ready), 1000);

  if (!atomic_read(&data_ready)) {
    if (!atomic_read(&suspend_flag)) {
      printk(KERN_ERR "AKECS_TransRBUFF: Data not ready\n");
      failure_count++;
      if (failure_count >= MAX_FAILURE_COUNT) {
	printk(KERN_ERR "AKECS_TransRBUFF: successive %d failure.\n",
	       failure_count);
	atomic_set(&open_flag, -1);
	wake_up(&open_wq);
        failure_count = 0;
      }
    }
    return -1;
  }

  mutex_lock(&sense_data_mutex);
  memcpy(&rbuf[1], &sense_data[1], size);
  atomic_set(&data_ready, 0);
  mutex_unlock(&sense_data_mutex);

  failure_count = 0;*/
  return 0;
}

static void AKECS_Report_Value(short *rbuf)
{
  struct akm8975_data *data = i2c_get_clientdata(this_client);
  FUNCDBG("called");

#if AK8975DRV_DATA_DBG
  printk("AKECS_Report_Value: yaw = %d, pitch = %d, roll = %d\n",
         rbuf[0], rbuf[1], rbuf[2]);
  printk("                    tmp = %d, m_stat= %d, g_stat=%d\n",
         rbuf[3], rbuf[4], rbuf[5]);
  printk("      Acceleration:   x = %d LSB, y = %d LSB, z = %d LSB\n",
         rbuf[6], rbuf[7], rbuf[8]);
  printk("          Magnetic:   x = %d LSB, y = %d LSB, z = %d LSB\n\n",
         rbuf[9], rbuf[10], rbuf[11]);
#endif
  /* Report magnetic sensor information */
  if (atomic_read(&m_flag)) {
    input_report_abs(data->input_dev, ABS_RX, rbuf[0]);
    input_report_abs(data->input_dev, ABS_RY, rbuf[1]);
    input_report_abs(data->input_dev, ABS_RZ, rbuf[2]);
    input_report_abs(data->input_dev, ABS_RUDDER, rbuf[4]);
  }

  /* Report acceleration sensor information */
  if (atomic_read(&a_flag)) {
    input_report_abs(data->input_dev, ABS_X, rbuf[6]);
    input_report_abs(data->input_dev, ABS_Y, rbuf[7]);
    input_report_abs(data->input_dev, ABS_Z, rbuf[8]);
    input_report_abs(data->input_dev, ABS_WHEEL, rbuf[5]);
  }

  /* Report temperature information */
  if (atomic_read(&t_flag)) {
    input_report_abs(data->input_dev, ABS_THROTTLE, rbuf[3]);
  }

  if (atomic_read(&mv_flag)) {
    input_report_abs(data->input_dev, ABS_HAT0X, rbuf[9]);
    input_report_abs(data->input_dev, ABS_HAT0Y, rbuf[10]);
    input_report_abs(data->input_dev, ABS_BRAKE, rbuf[11]);
  }

  input_sync(data->input_dev);
}

static int AKECS_GetOpenStatus(void)
{
  FUNCDBG("called");
  wait_event_interruptible(open_wq, (atomic_read(&open_flag) != 0));
  return atomic_read(&open_flag);
}

static int AKECS_GetCloseStatus(void)
{
  FUNCDBG("called");
  wait_event_interruptible(open_wq, (atomic_read(&open_flag) <= 0));
  return atomic_read(&open_flag);
}

static void AKECS_CloseDone(void)
{
  FUNCDBG("called");
  atomic_set(&m_flag, 1);
  atomic_set(&a_flag, 1);
  atomic_set(&t_flag, 1);
  atomic_set(&mv_flag, 1);
}

static int akm_aot_open(struct inode *inode, struct file *file)
{
  int ret = -1;

  FUNCDBG("called");
  if (atomic_cmpxchg(&open_count, 0, 1) == 0) {
    if (atomic_cmpxchg(&open_flag, 0, 1) == 0) {
      atomic_set(&reserve_open_flag, 1);
      wake_up(&open_wq);
      ret = 0;
    }
  }
  return ret;
}

static int akm_aot_release(struct inode *inode, struct file *file)
{
  FUNCDBG("called");
  atomic_set(&reserve_open_flag, 0);
  atomic_set(&open_flag, 0);
  atomic_set(&open_count, 0);
  wake_up(&open_wq);
  return 0;
}

static int
akm_aot_ioctl(struct inode *inode, struct file *file,
              unsigned int cmd, unsigned long arg)
{
  void __user *argp = (void __user *) arg;
  short flag;

  FUNCDBG("called");

  switch (cmd) {
  case ECS_IOCTL_APP_SET_MFLAG:
  case ECS_IOCTL_APP_SET_AFLAG:
  case ECS_IOCTL_APP_SET_MVFLAG:
    if (copy_from_user(&flag, argp, sizeof(flag)))
      return -EFAULT;
    if (flag < 0 || flag > 1)
      return -EINVAL;
    break;
  case ECS_IOCTL_APP_SET_DELAY:
    if (copy_from_user(&flag, argp, sizeof(flag)))
      return -EFAULT;
    break;
  default:
    break;
  }

  switch (cmd) {
  case ECS_IOCTL_APP_SET_MFLAG:
    atomic_set(&m_flag, flag);
    break;
  case ECS_IOCTL_APP_GET_MFLAG:
    flag = atomic_read(&m_flag);
    break;
  case ECS_IOCTL_APP_SET_AFLAG:
    atomic_set(&a_flag, flag);
    break;
  case ECS_IOCTL_APP_GET_AFLAG:
    flag = atomic_read(&a_flag);
    break;
  case ECS_IOCTL_APP_SET_MVFLAG:
    atomic_set(&mv_flag, flag);
    break;
  case ECS_IOCTL_APP_GET_MVFLAG:
    flag = atomic_read(&mv_flag);
    break;
  case ECS_IOCTL_APP_SET_DELAY:
    akmd_delay = flag;
    break;
  case ECS_IOCTL_APP_GET_DELAY:
    flag = akmd_delay;
    break;
  default:
    return -ENOTTY;
  }

  switch (cmd) {
  case ECS_IOCTL_APP_GET_MFLAG:
  case ECS_IOCTL_APP_GET_AFLAG:
  case ECS_IOCTL_APP_GET_MVFLAG:
  case ECS_IOCTL_APP_GET_DELAY:
    if (copy_to_user(argp, &flag, sizeof(flag)))
      return -EFAULT;
    break;
  default:
    break;
  }

  return 0;
}

static int akmd_open(struct inode *inode, struct file *file)
{
  FUNCDBG("called");
  return nonseekable_open(inode, file);
}

static int akmd_release(struct inode *inode, struct file *file)
{
  FUNCDBG("called");
  AKECS_CloseDone();
  return 0;
}

static int
akmd_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
           unsigned long arg)
{
  void __user *argp = (void __user *) arg;

  char msg[RBUFF_SIZE];
  char rwbuf[16];
  int ret = -1;
  int status;
//  short mode;
  short value[12];
  short delay;
//  char *pbuffer = 0;

  FUNCDBG("called");

  switch (cmd) {
  case ECS_IOCTL_READ:
  case ECS_IOCTL_WRITE:
    if (copy_from_user(&rwbuf, argp, sizeof(rwbuf)))
      return -EFAULT;
    break;

  case ECS_IOCTL_SET_YPR:
    if (copy_from_user(&value, argp, sizeof(value)))
      return -EFAULT;
    break;

  default:
    break;
  }

  switch (cmd) {
  case ECS_IOCTL_READ:
    if (rwbuf[0] < 1)
      return -EINVAL;
    ret = AKI2C_RxData(&rwbuf[1], rwbuf[0]);

    if (ret < 0)
      return ret;
    break;

  case ECS_IOCTL_WRITE:
    if (rwbuf[0] < 2)
      return -EINVAL;
    ret = AKI2C_TxData(&rwbuf[1], rwbuf[0]);

    if (ret < 0)
      return ret;
    break;

  case ECS_IOCTL_GETDATA:
    ret = AKECS_TransRBuff(msg, RBUFF_SIZE);
    if (ret < 0)
      return ret;
    break;

  case ECS_IOCTL_SET_YPR:
    AKECS_Report_Value(value);
    break;

  case ECS_IOCTL_GET_OPEN_STATUS:
    status = AKECS_GetOpenStatus();
    break;
  case ECS_IOCTL_GET_CLOSE_STATUS:
    status = AKECS_GetCloseStatus();
    break;

  case ECS_IOCTL_GET_DELAY:
    delay = akmd_delay;
    break;

  default:
    FUNCDBG("Unknown cmd\n");
    return -ENOTTY;
  }

  switch (cmd) {
  case ECS_IOCTL_READ:
    if (copy_to_user(argp, &rwbuf, sizeof(rwbuf)))
      return -EFAULT;
    break;
  case ECS_IOCTL_GETDATA:
    if (copy_to_user(argp, &msg, sizeof(msg)))
      return -EFAULT;
    break;
  case ECS_IOCTL_GET_OPEN_STATUS:
  case ECS_IOCTL_GET_CLOSE_STATUS:
    if (copy_to_user(argp, &status, sizeof(status)))
      return -EFAULT;
    break;
  case ECS_IOCTL_GET_DELAY:
    if (copy_to_user(argp, &delay, sizeof(delay)))
      return -EFAULT;
    break;
  default:
    break;
  }

  return 0;
}

static void akm_work_func(struct work_struct *work)
{
  FUNCDBG("called");
  AKECS_GetData();
  enable_irq(this_client->irq);
}

static irqreturn_t akm8975_interrupt(int irq, void *dev_id)
{
  struct akm8975_data *data = dev_id;
  disable_irq_nosync(this_client->irq);
  schedule_work(&data->work);
  return IRQ_HANDLED;
}

#ifdef CONFIG_ANDROID_POWER
static void akm8975_early_suspend(android_early_suspend_t * handler)
{
#if AK8975DRV_CALL_DBG
  printk(KERN_INFO "%s\n", __FUNCTION__);
#endif
  /*
  atomic_set(&suspend_flag, 1);
  if (atomic_read(&open_flag) == 2)
    AKECS_SetMode(AKECS_MODE_POWERDOWN);

  atomic_set(&reserve_open_flag, atomic_read(&open_flag));
  atomic_set(&open_flag, 0);
  wake_up(&open_wq);*/
}

static void akm8975_early_resume(android_early_suspend_t * handler)
{
#if AK8975DRV_CALL_DBG
  printk(KERN_INFO "%s\n", __FUNCTION__);
#endif
  /*
  atomic_set(&suspend_flag, 0);
  atomic_set(&open_flag, atomic_read(&reserve_open_flag));
  wake_up(&open_wq);*/
}
#endif


static int akm8975_init_client(struct i2c_client *client)
{
  struct akm8975_data *data;
  int ret;

  data = i2c_get_clientdata(client);

  mutex_init(&sense_data_mutex);

  ret = request_irq(client->irq, akm8975_interrupt, IRQF_TRIGGER_RISING,
		    "akm8975", data);

  if (ret < 0) {
    printk(KERN_ERR "akm8975_init_client: request irq failed\n");
    goto err;
  }
  /*
  pdata = client->dev.platform_data;
  if (pdata == NULL) {
    pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
    if (pdata == NULL) {
      ret = -ENOMEM;
      goto err_alloc_data_failed;
    } else {
      pdata->intr = ECS_INTR;
    }
  }
  */

  ret = gpio_request(ECS_INTR, "akm8975");
  if (ret < 0) {
    printk(KERN_ERR
	   "akm8975_init_client: request INT gpio failed\n");
    goto err_free_irq;
  }
  ret = gpio_direction_input(ECS_INTR);
  if (ret < 0) {
    printk(KERN_ERR
	   "akm8975_init_client: request INT gpio failed\n");
    goto err_free_gpio;
  }

  init_waitqueue_head(&data_ready_wq);
  init_waitqueue_head(&open_wq);

  /* As default, report all information */
  atomic_set(&m_flag, 1);
  atomic_set(&a_flag, 1);
  atomic_set(&t_flag, 1);
  atomic_set(&mv_flag, 1);

  return 0;

 err_free_gpio:
  gpio_free(ECS_INTR);
 err_free_irq:
  free_irq(client->irq, 0);
// err_alloc_data_failed:
 err:
  return ret;
}

static struct file_operations akmd_fops = {
  .owner = THIS_MODULE,
  .open = akmd_open,
  .release = akmd_release,
  .ioctl = akmd_ioctl,
};

static struct file_operations akm_aot_fops = {
  .owner = THIS_MODULE,
  .open = akm_aot_open,
  .release = akm_aot_release,
  .ioctl = akm_aot_ioctl,
};

static struct miscdevice akm_aot_device = {
  .minor = MISC_DYNAMIC_MINOR,
  .name = "akm8975_aot",
  .fops = &akm_aot_fops,
};

static struct miscdevice akmd_device = {
  .minor = MISC_DYNAMIC_MINOR,
  .name = "akm8975_dev",
  .fops = &akmd_fops,
};

int akm8975_probe(struct i2c_client *client,
                  const struct i2c_device_id *devid)
{
  struct akm8975_data *akm;
  int err;
  FUNCDBG("called");

  if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
    err = -ENODEV;
    goto exit_check_functionality_failed;
  }

  akm = kzalloc(sizeof(struct akm8975_data), GFP_KERNEL);
  if (!akm) {
    err = -ENOMEM;
    goto exit_alloc_data_failed;
  }

  INIT_WORK(&akm->work, akm_work_func);
  i2c_set_clientdata(client, akm);
  akm8975_init_client(client);
  this_client = client;

  akm->input_dev = input_allocate_device();

  if (!akm->input_dev) {
    err = -ENOMEM;
    printk(KERN_ERR "akm8975_probe: Failed to allocate input device\n");
    goto exit_input_dev_alloc_failed;
  }

  set_bit(EV_ABS, akm->input_dev->evbit);

  /* yaw */
  input_set_abs_params(akm->input_dev, ABS_RX, 0, 23040, 0, 0);
  /* pitch */
  input_set_abs_params(akm->input_dev, ABS_RY, -11520, 11520, 0, 0);
  /* roll */
  input_set_abs_params(akm->input_dev, ABS_RZ, -5760, 5760, 0, 0);
  /* x-axis acceleration */
  input_set_abs_params(akm->input_dev, ABS_X, -5760, 5760, 0, 0);
  /* y-axis acceleration */
  input_set_abs_params(akm->input_dev, ABS_Y, -5760, 5760, 0, 0);
  /* z-axis acceleration */
  input_set_abs_params(akm->input_dev, ABS_Z, -5760, 5760, 0, 0);
  /* temparature */
  input_set_abs_params(akm->input_dev, ABS_THROTTLE, -30, 85, 0, 0);
  /* status of magnetic sensor */
  input_set_abs_params(akm->input_dev, ABS_RUDDER, 0, 3, 0, 0);
  /* status of acceleration sensor */
  input_set_abs_params(akm->input_dev, ABS_WHEEL, 0, 3, 0, 0);
  /* x-axis of raw magnetic vector */
  input_set_abs_params(akm->input_dev, ABS_HAT0X, -2048, 2032, 0, 0);
  /* y-axis of raw magnetic vector */
  input_set_abs_params(akm->input_dev, ABS_HAT0Y, -2048, 2032, 0, 0);
  /* z-axis of raw magnetic vector */
  input_set_abs_params(akm->input_dev, ABS_BRAKE, -2048, 2032, 0, 0);

  akm->input_dev->name = "compass";

  err = input_register_device(akm->input_dev);

  if (err) {
    printk(KERN_ERR
           "akm8975_probe: Unable to register input device: %s\n",
           akm->input_dev->name);
    goto exit_input_register_device_failed;
  }

  err = misc_register(&akmd_device);
  if (err) {
    printk(KERN_ERR "akm8975_probe: akmd_device register failed\n");
    goto exit_misc_device_register_failed;
  }

  err = misc_register(&akm_aot_device);
  if (err) {
    printk(KERN_ERR "akm8975_probe: akm_aot_device register failed\n");
    goto exit_misc_device_register_failed;
  }

  err = device_create_file(&client->dev, &dev_attr_ms1);

#ifdef CONFIG_ANDROID_POWER
  akm->early_suspend.suspend = akm8975_early_suspend;
  akm->early_suspend.resume = akm8975_early_resume;
  android_register_early_suspend(&akm->early_suspend);
#endif
  return 0;

exit_misc_device_register_failed:
exit_input_register_device_failed:
  input_free_device(akm->input_dev);
exit_input_dev_alloc_failed:
  kfree(akm);
exit_alloc_data_failed:
exit_check_functionality_failed:
  return err;
}

static int akm8975_remove(struct i2c_client *client)
{
  struct akm8975_data *akm = i2c_get_clientdata(client);
  FUNCDBG("called");
  free_irq(client->irq, NULL);
  input_unregister_device(akm->input_dev);
  misc_deregister(&akmd_device);
  misc_deregister(&akm_aot_device);
  kfree(akm);
  return 0;
}

static const struct i2c_device_id akm8975_id[] = {
	{ "akm8975", 0 },
	{ }
};

static struct i2c_driver akm8975_driver = {
  .probe = akm8975_probe,
  .remove = akm8975_remove,
  .id_table = akm8975_id,
  .driver = {
             .name = "akm8975",
             },
};

static int __init akm8975_init(void)
{
  printk(KERN_INFO "AK8975 compass driver: init\n");
  FUNCDBG("AK8975 compass driver: init\n");
  return i2c_add_driver(&akm8975_driver);
}

static void __exit akm8975_exit(void)
{
  FUNCDBG("AK8975 compass driver: exit\n");
  i2c_del_driver(&akm8975_driver);
}

module_init(akm8975_init);
module_exit(akm8975_exit);

MODULE_AUTHOR("Hou-Kun Chen <hk_chen@htc.com>");
MODULE_DESCRIPTION("AK8975 compass driver");
MODULE_LICENSE("GPL");