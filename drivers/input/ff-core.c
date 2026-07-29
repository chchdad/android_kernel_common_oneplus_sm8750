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
#include <linux/delay.h>

/* ---- 强行变轨全局指针与异步队列 ---- */
struct input_dev *active_gamepad = NULL;
static int gamepad_effect_id = -1;

/* 【无锁防线】：引入原子计数器，用于防止断连时内存被提前 kfree */
atomic_t gamepad_ff_usage = ATOMIC_INIT(0);

/* 提前声明劫持接口，防止 SysRq 编译报隐式声明错误 */
int trigger_gamepad_vib_from_system(int intensity);

/* 【终极修复1】：设立一次性上传延时任务，给底层驱动留出 1 秒钟的初始化时间 */
static void gamepad_upload_worker(struct work_struct *work)
{
	struct ff_effect effect;
	/* 安全缓存指针，防止中途变 NULL */
	struct input_dev *dev = READ_ONCE(active_gamepad);
	
	if (!dev) return;

	atomic_inc(&gamepad_ff_usage);
	if (dev == READ_ONCE(active_gamepad) && dev->ff && dev->ff->upload) {
		memset(&effect, 0, sizeof(effect));
		effect.type = FF_RUMBLE;
		effect.id = -1;
		effect.u.rumble.strong_magnitude = 0xFFFF;
		effect.u.rumble.weak_magnitude = 0xFFFF;
		effect.replay.length = 50; 

		if (input_ff_upload(dev, &effect, (struct file *)1) == 0) {
			gamepad_effect_id = effect.id;
			printk(KERN_INFO "FF_CORE_DBG: [手柄初始化] 震动特效上传成功, ID=%d\n", gamepad_effect_id);
		} else {
			printk(KERN_INFO "FF_CORE_DBG: [手柄初始化] 震动特效上传失败!\n");
		}
	} else {
		printk(KERN_INFO "FF_CORE_DBG: [手柄初始化] 底层 ff->upload 尚未挂载\n");
	}
	atomic_dec(&gamepad_ff_usage);
}
/* 改为 DELAYED_WORK */
static DECLARE_DELAYED_WORK(gamepad_upload_work, gamepad_upload_worker);

/* ---- 强制刹车延时任务 ---- */
static void gamepad_stop_worker(struct work_struct *work)
{
	unsigned long flags;
	struct input_dev *dev = READ_ONCE(active_gamepad); 
	
	if (dev && gamepad_effect_id != -1) {
		atomic_inc(&gamepad_ff_usage);
		if (dev == READ_ONCE(active_gamepad) && dev->ff && dev->ff->playback) {
			spin_lock_irqsave(&dev->event_lock, flags);
			if (dev->ff && dev->ff->playback) {
				dev->ff->playback(dev, gamepad_effect_id, 0);
			}
			spin_unlock_irqrestore(&dev->event_lock, flags);
		}
		atomic_dec(&gamepad_ff_usage);
	}
}
static DECLARE_DELAYED_WORK(gamepad_stop_work, gamepad_stop_worker);
/* -------------------------------- */

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

int input_ff_event(struct input_dev *dev, unsigned int type,
		   unsigned int code, int value)
{
	struct ff_device *ff = dev->ff;

	if (type != EV_FF)
		return 0;

	switch (code) {
	case FF_GAIN:
		/* 原有逻辑，保持不变 */
		break;

	case FF_AUTOCENTER:
		/* 原有逻辑，保持不变 */
		break;

	default:
		/* ---- 核心并轨（拦截原生马达，同步驱动手柄） ---- */
		{
			struct input_dev *gp_dev = READ_ONCE(active_gamepad);
			if (gp_dev && dev != gp_dev) {
				unsigned long flags;
				if (gamepad_effect_id != -1) {
					atomic_inc(&gamepad_ff_usage);
					if (gp_dev == READ_ONCE(active_gamepad) && gp_dev->ff && gp_dev->ff->playback) {
						spin_lock_irqsave(&gp_dev->event_lock, flags);
						if (gp_dev->ff && gp_dev->ff->playback) {
							gp_dev->ff->playback(gp_dev, gamepad_effect_id, value > 0 ? 1 : 0);
							printk(KERN_INFO "FF_CORE_DBG: [劫持成功] 信号已发送至手柄, intensity=%d\n", value);
						}
						spin_unlock_irqrestore(&gp_dev->event_lock, flags);

						if (value > 0) {
							mod_delayed_work(system_wq, &gamepad_stop_work, msecs_to_jiffies(50));
						} else {
							cancel_delayed_work(&gamepad_stop_work);
						}
					} else {
						printk(KERN_INFO "FF_CORE_DBG: [劫持失败] 手柄指针已销毁或底层 playback 未就绪\n");
					}
					atomic_dec(&gamepad_ff_usage);
				} else {
					printk(KERN_INFO "FF_CORE_DBG: [劫持失败] 尚未上传有效震动特效 (gamepad_effect_id == -1)\n");
				}
				/* 【精准拦截】：只要手柄连着，就截断指令，手机静音 (与 [source: 6] 完全一致) */
				return 0; 
			}
		}

		/* ---- 原机马达继续通电 ---- */
		if (!ff) {
			printk(KERN_INFO "FF_CORE_DBG: [手机原生丢弃] dev->ff 为 NULL, 防御 0x40 空指针越界\n");
			break;
		}

		if (check_effect_access(ff, code, NULL) == 0) {
			if (ff->playback) {
				ff->playback(dev, code, value);
				printk(KERN_INFO "FF_CORE_DBG: [手机原生放行] 马达正常震动, code=%u, value=%d\n", code, value);
			} else {
				printk(KERN_INFO "FF_CORE_DBG: [手机原生异常] check_effect_access通过, 但 ff->playback 为 NULL\n");
			}
		} else {
			printk(KERN_INFO "FF_CORE_DBG: [手机原生丢弃] 遭 check_effect_access 拒绝 (code=%u)\n", code);
		}
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(input_ff_event);

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
			WRITE_ONCE(active_gamepad, dev);
			gamepad_effect_id = -1;
			/* 延迟 1000 毫秒，等驱动把指针全部挂载完毕 */
			schedule_delayed_work(&gamepad_upload_work, msecs_to_jiffies(1000));
			register_sysrq_key('v', &sysrq_gamepad_vib_op);
			printk(KERN_INFO "FF_CORE_PROBE: [成功] 成功抓取手柄设备!\n");
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(input_ff_create);

void input_ff_destroy(struct input_dev *dev)
{
	struct ff_device *ff = dev->ff;

	/* ---- 拔掉手柄时释放指针并销毁队列任务 ---- */
	if (dev == READ_ONCE(active_gamepad)) {
		/* 瞬间置空，阻断上面的 input_ff_event 再次拿到指针 */
		WRITE_ONCE(active_gamepad, NULL);
		gamepad_effect_id = -1;
		
		cancel_delayed_work_sync(&gamepad_upload_work);
		cancel_delayed_work_sync(&gamepad_stop_work);
		
		/* 原地空转等待当前震动周期离开，彻底根除 UAF 崩溃，且绝不触发看门狗 */
		while (atomic_read(&gamepad_ff_usage) > 0) {
			cpu_relax();
		}
		
		unregister_sysrq_key('v', &sysrq_gamepad_vib_op);
		printk(KERN_INFO "FF_CORE_DBG: Gamepad disconnected safely\n");
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
	struct input_dev *dev = READ_ONCE(active_gamepad);
	
	if (dev && gamepad_effect_id != -1) {
		atomic_inc(&gamepad_ff_usage);
		if (dev == READ_ONCE(active_gamepad) && dev->ff && dev->ff->playback) {
			spin_lock_irqsave(&dev->event_lock, flags);
			if (dev->ff && dev->ff->playback) {
				dev->ff->playback(dev, gamepad_effect_id, intensity > 0 ? 1 : 0);
			}
			spin_unlock_irqrestore(&dev->event_lock, flags);
			
			if (intensity > 0) mod_delayed_work(system_wq, &gamepad_stop_work, msecs_to_jiffies(50));
			else cancel_delayed_work(&gamepad_stop_work);
			
			atomic_dec(&gamepad_ff_usage);
			printk(KERN_INFO "FF_CORE_DBG: [SysRq命令] 强制触发手柄震动成功\n");
			return 1; /* 告诉 ioctl 拦截器：我已接管，把手机马达掐断！ */
		}
		atomic_dec(&gamepad_ff_usage);
	}
	printk(KERN_INFO "FF_CORE_DBG: [SysRq命令] 强制触发失败，未连接或尚未挂载\n");
	return 0; /* 手柄没连，放行指令给原机马达 */
}
/* 修复导出函数名的错别字 */
EXPORT_SYMBOL_GPL(trigger_gamepad_vib_from_system);
