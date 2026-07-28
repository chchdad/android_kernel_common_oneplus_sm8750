// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Force feedback support for Linux input subsystem
 *
 *  Copyright (c) 2006 Anssi Hannula <anssi.hannula@gmail.com>
 *  Copyright (c) 2006 Dmitry Torokhov <dtor@mail.ru>
 */

/* #define DEBUG */

#include <linux/input.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sysrq.h>

/* ---- 强行变轨全局指针与异步队列 ---- */
struct input_dev *active_gamepad = NULL;
static int gamepad_effect_id = -1;

/* 【终极修复1】：设立一次性上传 Worker，仅在手柄连接时执行一次，绝不干扰游戏过程 */
static void gamepad_upload_worker(struct work_struct *work)
{
	struct ff_effect effect;
	if (!active_gamepad || !active_gamepad->ff) return;

	memset(&effect, 0, sizeof(effect));
	effect.type = FF_RUMBLE;
	effect.id = -1;
	effect.u.rumble.strong_magnitude = 0xFFFF;
	effect.u.rumble.weak_magnitude = 0xFFFF;
	/* 遵循确认：50ms时长的纯粹震感 */
	effect.replay.length = 50; 

	if (input_ff_upload(active_gamepad, &effect, (struct file *)1) == 0) {
		gamepad_effect_id = effect.id;
	}
}
static DECLARE_WORK(gamepad_upload_work, gamepad_upload_worker);

/* ---- 强制刹车延时任务 ---- */
static void gamepad_stop_worker(struct work_struct *work)
{
	unsigned long flags;
	struct input_dev *dev = active_gamepad; 
	
	if (dev && dev->ff && gamepad_effect_id != -1) {
		spin_lock_irqsave(&dev->event_lock, flags);
		if (dev->ff->playback) {
			dev->ff->playback(dev, gamepad_effect_id, 0);
		}
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}
static DECLARE_DELAYED_WORK(gamepad_stop_work, gamepad_stop_worker);
/* -------------------------------- */

static void gamepad_rumble_worker(struct work_struct *work)
{
	struct ff_effect effect;
	int val = gamepad_rumble_value;
	int ret;
	unsigned long flags; 
	struct input_dev *dev;

	mutex_lock(&gamepad_mutex);
	dev = active_gamepad;
	if (dev) {
		input_get_device(dev); /* 拿到引用 */
	}
	mutex_unlock(&gamepad_mutex);

	/* 如果连设备都没有，直接安全退出，不需要 put */
	if (!dev) {
		return;
	}

	if (!dev->ff) {
		goto out_put; 
	}

	if (gamepad_effect_id == -1) {
		memset(&effect, 0, sizeof(effect));
		effect.type = FF_RUMBLE;
		effect.id = -1;
		effect.u.rumble.strong_magnitude = 0xFFFF;
		effect.u.rumble.weak_magnitude = 0xFFFF;
		effect.replay.length = 50; 

		ret = input_ff_upload(dev, &effect, (struct file *)1);
		if (ret == 0) {
			gamepad_effect_id = effect.id;
		} else {
			/* 【核心修复：绝对不能在这里直接 return，必须走 out_put！】 */
			goto out_put; 
		}
	}

	if (gamepad_effect_id != -1) {
		spin_lock_irqsave(&dev->event_lock, flags);
		if (dev->ff && dev->ff->playback) {
			dev->ff->playback(dev, gamepad_effect_id, (val > 0) ? 1 : 0);
		}
		spin_unlock_irqrestore(&dev->event_lock, flags);

		if (val > 0) {
			mod_delayed_work(system_wq, &gamepad_stop_work, msecs_to_jiffies(50));
		} else {
			cancel_delayed_work(&gamepad_stop_work);
		}
	}

out_put:
	/* 【最后收尾】：绝对守恒！走到这必放行引用，绝不留僵尸设备给休眠机制 */
	input_put_device(dev);
}
static DECLARE_WORK(gamepad_rumble_work, gamepad_rumble_worker);


/* ---- 专属内核调试指令 (SysRq) ---- */
static void sysrq_handle_gamepad_vib(u8 key)
{
	printk(KERN_INFO "FF_CORE_PROBE: 收到 SysRq 调试指令，强制触发手柄震动!\n");
	trigger_gamepad_vib_from_system(1);
}
static const struct sysrq_key_op sysrq_gamepad_vib_op = {
	.handler = sysrq_handle_gamepad_vib,
	.help_msg = "vibrate-gamepad(v)",
	.action_msg = "Trigger Gamepad Vibration",
	.enable_mask = SYSRQ_ENABLE_DUMP,
};

/*
 * Check that the effect_id is a valid effect and whether the user
 * is the owner
 */
static int check_effect_access(struct ff_device *ff, int effect_id,
				struct file *file)
{
	if (effect_id < 0 || effect_id >= ff->max_effects ||
	    !ff->effect_owners[effect_id])
		return -EINVAL;

	if (file && ff->effect_owners[effect_id] != file)
		return -EACCES;

	return 0;
}

/*
 * Checks whether 2 effects can be combined together
 */
static inline int check_effects_compatible(struct ff_effect *e1,
					   struct ff_effect *e2)
{
	return e1->type == e2->type &&
	       (e1->type != FF_PERIODIC ||
		e1->u.periodic.waveform == e2->u.periodic.waveform);
}

/*
 * Convert an effect into compatible one
 */
static int compat_effect(struct ff_device *ff, struct ff_effect *effect)
{
	int magnitude;

	switch (effect->type) {
	case FF_RUMBLE:
		if (!test_bit(FF_PERIODIC, ff->ffbit))
			return -EINVAL;

		/*
		 * calculate magnitude of sine wave as average of rumble's
		 * 2/3 of strong magnitude and 1/3 of weak magnitude
		 */
		magnitude = effect->u.rumble.strong_magnitude / 3 +
			    effect->u.rumble.weak_magnitude / 6;

		effect->type = FF_PERIODIC;
		effect->u.periodic.waveform = FF_SINE;
		effect->u.periodic.period = 50;
		effect->u.periodic.magnitude = magnitude;
		effect->u.periodic.offset = 0;
		effect->u.periodic.phase = 0;
		effect->u.periodic.envelope.attack_length = 0;
		effect->u.periodic.envelope.attack_level = 0;
		effect->u.periodic.envelope.fade_length = 0;
		effect->u.periodic.envelope.fade_level = 0;

		return 0;

	default:
		/* Let driver handle conversion */
		return 0;
	}
}

/**
 * input_ff_upload() - upload effect into force-feedback device
 * @dev: input device
 * @effect: effect to be uploaded
 * @file: owner of the effect
 */
int input_ff_upload(struct input_dev *dev, struct ff_effect *effect,
		    struct file *file)
{
	struct ff_device *ff = dev->ff;
	struct ff_effect *old;
	int ret = 0;
	int id;

	if (!test_bit(EV_FF, dev->evbit))
		return -ENOSYS;

	if (effect->type < FF_EFFECT_MIN || effect->type > FF_EFFECT_MAX ||
	    !test_bit(effect->type, dev->ffbit)) {
		dev_dbg(&dev->dev, "invalid or not supported effect type in upload\n");
		return -EINVAL;
	}

	if (effect->type == FF_PERIODIC &&
	    (effect->u.periodic.waveform < FF_WAVEFORM_MIN ||
	     effect->u.periodic.waveform > FF_WAVEFORM_MAX ||
	     !test_bit(effect->u.periodic.waveform, dev->ffbit))) {
		dev_dbg(&dev->dev, "invalid or not supported wave form in upload\n");
		return -EINVAL;
	}

	if (!test_bit(effect->type, ff->ffbit)) {
		ret = compat_effect(ff, effect);
		if (ret)
			return ret;
	}

	mutex_lock(&ff->mutex);

	if (effect->id == -1) {
		for (id = 0; id < ff->max_effects; id++)
			if (!ff->effect_owners[id])
				break;

		if (id >= ff->max_effects) {
			ret = -ENOSPC;
			goto out;
		}

		effect->id = id;
		old = NULL;

	} else {
		id = effect->id;

		ret = check_effect_access(ff, id, file);
		if (ret)
			goto out;

		old = &ff->effects[id];

		if (!check_effects_compatible(effect, old)) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = ff->upload(dev, effect, old);
	if (ret)
		goto out;

	spin_lock_irq(&dev->event_lock);
	ff->effects[id] = *effect;
	ff->effect_owners[id] = file;
	spin_unlock_irq(&dev->event_lock);

 out:
	mutex_unlock(&ff->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(input_ff_upload);

/*
 * Erases the effect if the requester is also the effect owner. The mutex
 * should already be locked before calling this function.
 */
static int erase_effect(struct input_dev *dev, int effect_id,
			struct file *file)
{
	struct ff_device *ff = dev->ff;
	int error;

	error = check_effect_access(ff, effect_id, file);
	if (error)
		return error;

	spin_lock_irq(&dev->event_lock);
	ff->playback(dev, effect_id, 0);
	ff->effect_owners[effect_id] = NULL;
	spin_unlock_irq(&dev->event_lock);

	if (ff->erase) {
		error = ff->erase(dev, effect_id);
		if (error) {
			spin_lock_irq(&dev->event_lock);
			ff->effect_owners[effect_id] = file;
			spin_unlock_irq(&dev->event_lock);

			return error;
		}
	}

	return 0;
}

/**
 * input_ff_erase - erase a force-feedback effect from device
 * @dev: input device to erase effect from
 * @effect_id: id of the effect to be erased
 * @file: purported owner of the request
 *
 * This function erases a force-feedback effect from specified device.
 * The effect will only be erased if it was uploaded through the same
 * file handle that is requesting erase.
 */
int input_ff_erase(struct input_dev *dev, int effect_id, struct file *file)
{
	struct ff_device *ff = dev->ff;
	int ret;

	if (!test_bit(EV_FF, dev->evbit))
		return -ENOSYS;

	mutex_lock(&ff->mutex);
	ret = erase_effect(dev, effect_id, file);
	mutex_unlock(&ff->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(input_ff_erase);

/*
 * input_ff_flush - erase all effects owned by a file handle
 * @dev: input device to erase effect from
 * @file: purported owner of the effects
 *
 * This function erases all force-feedback effects associated with
 * the given owner from specified device. Note that @file may be %NULL,
 * in which case all effects will be erased.
 */
int input_ff_flush(struct input_dev *dev, struct file *file)
{
	struct ff_device *ff = dev->ff;
	int i;

	dev_dbg(&dev->dev, "flushing now\n");

	mutex_lock(&ff->mutex);

	for (i = 0; i < ff->max_effects; i++)
		erase_effect(dev, i, file);

	mutex_unlock(&ff->mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(input_ff_flush);

/**
 * input_ff_event() - generic handler for force-feedback events
 * @dev: input device to send the effect to
 * @type: event type (anything but EV_FF is ignored)
 * @code: event code
 * @value: event value
 */
int input_ff_event(struct input_dev *dev, unsigned int type,
		   unsigned int code, int value)
{
	struct ff_device *ff = dev->ff;

	if (type != EV_FF)
		return 0;

	switch (code) {
	case FF_GAIN:
		if (active_gamepad && dev != active_gamepad && active_gamepad->ff && active_gamepad->ff->set_gain) {
			active_gamepad->ff->set_gain(active_gamepad, value);
		}
		if (!test_bit(FF_GAIN, dev->ffbit) || value > 0xffffU)
			break;
		ff->set_gain(dev, value);
		break;

	case FF_AUTOCENTER:
		if (!test_bit(FF_AUTOCENTER, dev->ffbit) || value > 0xffffU)
			break;
		ff->set_autocenter(dev, value);
		break;

	default:
		/* ---- 核心并轨（拦截原生马达，同步驱动手柄） ---- */
		if (active_gamepad && dev != active_gamepad) {
			unsigned long flags;
			if (gamepad_effect_id != -1 && active_gamepad->ff && active_gamepad->ff->playback) {
				/* 同步极速调用，与内核原生安全机制保持100%一致 */
				spin_lock_irqsave(&active_gamepad->event_lock, flags);
				active_gamepad->ff->playback(active_gamepad, gamepad_effect_id, value > 0 ? 1 : 0);
				spin_unlock_irqrestore(&active_gamepad->event_lock, flags);

				if (value > 0) {
					mod_delayed_work(system_wq, &gamepad_stop_work, msecs_to_jiffies(50));
				} else {
					cancel_delayed_work(&gamepad_stop_work);
				}
			}
			/* 【终极静音防线】：只要手柄连着，就截断指令并返回0，手机绝对不震！ */
			return 0; 
		}

		/* ---- 原机马达继续通电 ---- */
		if (check_effect_access(ff, code, NULL) == 0)
			ff->playback(dev, code, value);
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(input_ff_event);

/**
 * input_ff_create() - create force-feedback device
 * @dev: input device supporting force-feedback
 * @max_effects: maximum number of effects supported by the device
 *
 * This function allocates all necessary memory for a force feedback
 * portion of an input device and installs all default handlers.
 * @dev->ffbit should be already set up before calling this function.
 * Once ff device is created you need to setup its upload, erase,
 * playback and other handlers before registering input device
 */
int input_ff_create(struct input_dev *dev, unsigned int max_effects)
{
	struct ff_device *ff;
	size_t ff_dev_size;
	int i;

	if (!max_effects) {
		dev_err(&dev->dev, "cannot allocate device without any effects\n");
		return -EINVAL;
	}

	if (max_effects > FF_MAX_EFFECTS) {
		dev_err(&dev->dev, "cannot allocate more than FF_MAX_EFFECTS effects\n");
		return -EINVAL;
	}

	ff_dev_size = struct_size(ff, effect_owners, max_effects);
	if (ff_dev_size == SIZE_MAX) /* overflow */
		return -EINVAL;

	ff = kzalloc(ff_dev_size, GFP_KERNEL);
	if (!ff)
		return -ENOMEM;

	ff->effects = kcalloc(max_effects, sizeof(struct ff_effect),
			      GFP_KERNEL);
	if (!ff->effects) {
		kfree(ff);
		return -ENOMEM;
	}

	ff->max_effects = max_effects;
	mutex_init(&ff->mutex);

	dev->ff = ff;
	dev->flush = input_ff_flush;
	dev->event = input_ff_event;
	__set_bit(EV_FF, dev->evbit);

	/* Copy "true" bits into ff device bitmap */
	for_each_set_bit(i, dev->ffbit, FF_CNT)
		__set_bit(i, ff->ffbit);

	/* we can emulate RUMBLE with periodic effects */
	if (test_bit(FF_PERIODIC, ff->ffbit))
		__set_bit(FF_RUMBLE, dev->ffbit);

	/* ---- 抓取手柄设备探针 ---- */
	if (dev->name) {
		if (strstr(dev->name, "Xbox") || strstr(dev->name, "Controller")) {
			active_gamepad = dev;
			gamepad_effect_id = -1;
			/* 连接时立刻异步上传特效，一劳永逸！ */
			schedule_work(&gamepad_upload_work);
			register_sysrq_key('v', &sysrq_gamepad_vib_op);
			printk(KERN_INFO "FF_CORE_PROBE: [成功] 成功抓取手柄设备!\n");
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(input_ff_create);

/**
 * input_ff_destroy() - frees force feedback portion of input device
 * @dev: input device supporting force feedback
 *
 * This function is only needed in error path as input core will
 * automatically free force feedback structures when device is
 * destroyed.
 */
void input_ff_destroy(struct input_dev *dev)
{
	struct ff_device *ff = dev->ff;

	/* ---- 拔掉手柄时释放指针并销毁队列任务 ---- */
	if (dev == active_gamepad) {
		/* 【终极修复2】：瞬间置空并立刻放行！绝不使用 _sync 阻塞底层硬件销毁！ */
		active_gamepad = NULL;
		gamepad_effect_id = -1;
		
		cancel_work(&gamepad_upload_work);
		cancel_delayed_work(&gamepad_stop_work);
		
		unregister_sysrq_key('v', &sysrq_gamepad_vib_op);
		printk(KERN_INFO "FF_CORE: Gamepad disconnected\n");
	}

	__clear_bit(EV_FF, dev->evbit);
	if (ff) {
		if (ff->destroy)
			ff->destroy(ff);
		kfree(ff->private);
		kfree(ff->effects);
		kfree(ff);
		dev->ff = NULL;
	}
}

/* ---- 暴露给原机马达驱动的劫持接口 ---- */
int trigger_gamepad_vib_from_system(int intensity)
{
	unsigned long flags;
	if (active_gamepad && gamepad_effect_id != -1) {
		spin_lock_irqsave(&active_gamepad->event_lock, flags);
		if (active_gamepad->ff && active_gamepad->ff->playback) {
			active_gamepad->ff->playback(active_gamepad, gamepad_effect_id, intensity > 0 ? 1 : 0);
		}
		spin_unlock_irqrestore(&active_gamepad->event_lock, flags);
		
		if (intensity > 0) mod_delayed_work(system_wq, &gamepad_stop_work, msecs_to_jiffies(50));
		else cancel_delayed_work(&gamepad_stop_work);
		
		return 1; /* 告诉 ioctl 拦截器：我已接管，把手机马达掐断！ */
	}
	return 0; /* 手柄没连，放行指令给原机马达 */
}
EXPORT_SYMBOL_GPL(input_ff_destroy);
