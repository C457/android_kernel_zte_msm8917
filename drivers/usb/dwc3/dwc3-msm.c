/* Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/pm_runtime.h>
#include <linux/ratelimit.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/of.h>
#include <linux/usb/msm_hsusb.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/clk/msm-clk.h>
#include <linux/msm-bus.h>
#include <linux/irq.h>

#include "power.h"
#include "core.h"
#include "gadget.h"
#include "dbm.h"
#include "debug.h"
#include "xhci.h"

#define DWC3_IDEV_CHG_MAX 1500
#define DWC3_HVDCP_CHG_MAX 1800
#define DWC3_WAKEUP_SRC_TIMEOUT 5000

#define MICRO_5V    5000000
#define MICRO_9V    9000000

/* AHB2PHY register offsets */
#define PERIPH_SS_AHB2PHY_TOP_CFG 0x10

/* AHB2PHY read/write waite value */
#define ONE_READ_WRITE_WAIT 0x11

/* cpu to fix usb interrupt */
static int cpu_to_affin;
module_param(cpu_to_affin, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(cpu_to_affin, "affin usb irq to this cpu");

static bool disable_host_mode;
module_param(disable_host_mode, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(disable_host_mode, "To stop HOST mode detection");

static int override_phy_init;
module_param(override_phy_init, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(override_phy_init, "Override HSPHY Init Seq");

/* Max current to be drawn for HVDCP charger */
static int hvdcp_max_current = DWC3_HVDCP_CHG_MAX;
module_param(hvdcp_max_current, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(hvdcp_max_current, "max current drawn for HVDCP charger");

/* Max current to be drawn for DCP charger */
int dcp_max_current = DWC3_IDEV_CHG_MAX;
module_param(dcp_max_current, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dcp_max_current, "max current drawn for DCP charger");

/* XHCI registers */
#define USB3_HCSPARAMS1		(0x4)
#define USB3_HCCPARAMS2		(0x1c)
#define HCC_CTC(p)		((p) & (1 << 3))
#define USB3_PORTSC		(0x420)

/**
 *  USB QSCRATCH Hardware registers
 *
 */
#define QSCRATCH_REG_OFFSET	(0x000F8800)
#define QSCRATCH_GENERAL_CFG	(QSCRATCH_REG_OFFSET + 0x08)
#define CGCTL_REG		(QSCRATCH_REG_OFFSET + 0x28)
#define PWR_EVNT_IRQ_STAT_REG    (QSCRATCH_REG_OFFSET + 0x58)
#define PWR_EVNT_IRQ_MASK_REG    (QSCRATCH_REG_OFFSET + 0x5C)
#define QSCRATCH_USB30_STS_REG	(QSCRATCH_REG_OFFSET + 0xF8)


#define PWR_EVNT_POWERDOWN_IN_P3_MASK		BIT(2)
#define PWR_EVNT_POWERDOWN_OUT_P3_MASK		BIT(3)
#define PWR_EVNT_LPM_IN_L2_MASK			BIT(4)
#define PWR_EVNT_LPM_OUT_L2_MASK		BIT(5)
#define PWR_EVNT_LPM_OUT_L1_MASK		BIT(13)

/* QSCRATCH_GENERAL_CFG register bit offset */
#define PIPE_UTMI_CLK_SEL	BIT(0)
#define PIPE3_PHYSTATUS_SW	BIT(3)
#define PIPE_UTMI_CLK_DIS	BIT(8)

#define HS_PHY_CTRL_REG		(QSCRATCH_REG_OFFSET + 0x10)
#define UTMI_OTG_VBUS_VALID	BIT(20)
#define SW_SESSVLD_SEL		BIT(28)

#define SS_PHY_CTRL_REG		(QSCRATCH_REG_OFFSET + 0x30)
#define LANE0_PWR_PRESENT	BIT(24)

/* GSI related registers */
#define GSI_TRB_ADDR_BIT_53_MASK	(1 << 21)
#define GSI_TRB_ADDR_BIT_55_MASK	(1 << 23)

#define	GSI_GENERAL_CFG_REG		(QSCRATCH_REG_OFFSET + 0xFC)
#define	GSI_RESTART_DBL_PNTR_MASK	BIT(20)
#define	GSI_CLK_EN_MASK			BIT(12)
#define	BLOCK_GSI_WR_GO_MASK		BIT(1)
#define	GSI_EN_MASK			BIT(0)

#define GSI_DBL_ADDR_L(n)	((QSCRATCH_REG_OFFSET + 0x110) + (n*4))
#define GSI_DBL_ADDR_H(n)	((QSCRATCH_REG_OFFSET + 0x120) + (n*4))
#define GSI_RING_BASE_ADDR_L(n)	((QSCRATCH_REG_OFFSET + 0x130) + (n*4))
#define GSI_RING_BASE_ADDR_H(n)	((QSCRATCH_REG_OFFSET + 0x140) + (n*4))

#define	GSI_IF_STS	(QSCRATCH_REG_OFFSET + 0x1A4)
#define	GSI_WR_CTRL_STATE_MASK	BIT(15)

struct dwc3_msm_req_complete {
	struct list_head list_item;
	struct usb_request *req;
	void (*orig_complete)(struct usb_ep *ep,
			      struct usb_request *req);
};

enum dwc3_id_state {
	DWC3_ID_GROUND = 0,
	DWC3_ID_FLOAT,
};

enum dwc3_perf_mode {
	DWC3_PERF_NOM,
	DWC3_PERF_SVS,
	DWC3_PERF_OFF,
	DWC3_PERF_INVALID,
};

/* Input bits to state machine (mdwc->inputs) */

#define ID			0
#define B_SESS_VLD		1
#define B_SUSPEND		2
/*
 * USB chargers
 *
 * DWC3_INVALID_CHARGER		Invalid USB charger.
 * DWC3_SDP_CHARGER		Standard downstream port. Refers to a
 *				downstream port on USB compliant host/hub.
 * DWC3_DCP_CHARGER		Dedicated charger port(AC charger/ Wall charger)
 * DWC3_CDP_CHARGER		Charging downstream port. Enumeration can happen
 *				and IDEV_CHG_MAX can be drawn irrespective of
 *				USB state.
 * DWC3_PROPRIETARY_CHARGER	A non-standard charger that pulls DP/DM to
 *				specific voltages between 2.0-3.3v for
 *				identification.
 */
enum dwc3_chg_type {
	DWC3_INVALID_CHARGER = 0,
	DWC3_SDP_CHARGER,
	DWC3_DCP_CHARGER,
	DWC3_CDP_CHARGER,
	DWC3_PROPRIETARY_CHARGER,
};

struct dwc3_msm {
	struct device *dev;
	void __iomem *base;
	void __iomem *ahb2phy_base;
	struct platform_device	*dwc3;
	const struct usb_ep_ops *original_ep_ops[DWC3_ENDPOINTS_NUM];
	struct list_head req_complete_list;
	struct clk		*xo_clk;
	struct clk		*core_clk;
	long			core_clk_rate;
	long                    core_clk_svs_rate;
	struct clk		*iface_clk;
	struct clk		*sleep_clk;
	struct clk		*utmi_clk;
	unsigned int		utmi_clk_rate;
	struct clk		*utmi_clk_src;
	struct clk		*bus_aggr_clk;
	struct clk		*cfg_ahb_clk;
	struct regulator	*dwc3_gdsc;

	struct usb_phy		*hs_phy, *ss_phy;

	struct dbm		*dbm;

	/* VBUS regulator for host mode */
	struct regulator	*vbus_reg;
	int			vbus_retry_count;
	bool			resume_pending;
	atomic_t                pm_suspended;
	int			hs_phy_irq;
	int			ss_phy_irq;
	struct delayed_work	resume_work;
	struct work_struct	restart_usb_work;
	bool			in_restart;
	struct workqueue_struct *dwc3_resume_wq;
	struct workqueue_struct *sm_usb_wq;
	struct delayed_work	sm_work;
	unsigned long		inputs;
	enum dwc3_chg_type	chg_type;
	unsigned		max_power;
	bool			charging_disabled;
	bool			detect_dpdm_floating;
	enum usb_otg_state	otg_state;
	enum usb_chg_state	chg_state;
	int			pmic_id_irq;
	enum dwc3_perf_mode     mode;
	unsigned int		bus_vote;
	u32			bus_perf_client;
	struct msm_bus_scale_pdata	*bus_scale_table;
	struct power_supply	usb_psy;
	enum power_supply_type	usb_supply_type;
	unsigned int		online;
	bool			in_host_mode;
	unsigned int		voltage_max;
	unsigned int		current_max;
	unsigned int		bc1p2_current_max;
	unsigned int		typec_current_max;
	unsigned int		health_status;
	unsigned int		tx_fifo_size;
	bool			vbus_active;
	bool			suspend;
	bool			disable_host_mode_pm;
	enum dwc3_id_state	id_state;
	unsigned long		lpm_flags;
#define MDWC3_SS_PHY_SUSPEND		BIT(0)
#define MDWC3_ASYNC_IRQ_WAKE_CAPABILITY	BIT(1)
#define MDWC3_POWER_COLLAPSE		BIT(2)

	unsigned int		irq_to_affin;
	struct notifier_block	dwc3_cpu_notifier;
	struct notifier_block	usbdev_nb;
	bool			hc_died;
	bool			stop_host;
	bool			no_wakeup_src_in_hostmode;
	bool			host_only_mode;
	bool			psy_not_used;
	bool			xhci_ss_compliance_enable;

	int  pwr_event_irq;
	atomic_t                in_p3;
	unsigned int		lpm_to_suspend_delay;
	bool			init;

	u32                     pm_qos_latency;
	struct pm_qos_request   pm_qos_req_dma;
	struct delayed_work     perf_vote_work;
	enum dwc3_perf_mode	curr_mode;
	/* wall charger in which D+/D- is disconnected
		would be recognized as usb cable, 1/5 */
	struct delayed_work	invalid_chg_work;
	/* end */
};

#define USB_HSPHY_3P3_VOL_MIN		3050000 /* uV */
#define USB_HSPHY_3P3_VOL_MAX		3300000 /* uV */
#define USB_HSPHY_3P3_HPM_LOAD		16000	/* uA */

#define USB_HSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_HSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_HSPHY_1P8_HPM_LOAD		19000	/* uA */

#define USB_SSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_SSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_SSPHY_1P8_HPM_LOAD		23000	/* uA */

#define DSTS_CONNECTSPD_SS		0x4

#define PM_QOS_SAMPLE_SEC		2
#define PM_QOS_THRESHOLD_NOM		400

static void dwc3_pwr_event_handler(struct dwc3_msm *mdwc);
static int dwc3_msm_gadget_vbus_draw(struct dwc3_msm *mdwc, unsigned mA);
/* wall charger in which D+/D- is disconnected
	would be recognized as an usb cable, 2/5 */
static int skip_invalid_chg_work = 0;
#define DWC3_IDEV_CHG_INVALID	501

static void dwc3_invalid_chg_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, invalid_chg_work.work);

	if (skip_invalid_chg_work) {
		dev_info(mdwc->dev, "do skip_invalid_chg_work\n");
		skip_invalid_chg_work = 0;
		return;
	}

	dev_info(mdwc->dev, "usb schedule %s\n", __func__);
	dwc3_msm_gadget_vbus_draw(mdwc, DWC3_IDEV_CHG_INVALID);
}
/*end*/


/*
 * for notify otg from gadget, 8/8
 * event 1:  skip invalid_chg work, as usb has already been configured
 * event 2:  ...
 */
static int dwc3_extra_event_from_gadget(struct dwc3_msm *mdwc, unsigned event)
{
	if (event == 1) {
		dev_dbg(mdwc->dev, "prepare skip_invalid_chg_work\n");
		skip_invalid_chg_work = 1;
	}
	return 0;
}
/* end */

/**
 *
 * Read register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg(void *base, u32 offset)
{
	u32 val = ioread32(base + offset);
	return val;
}

/**
 * Read register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg_field(void *base,
					  u32 offset,
					  const u32 mask)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 val = ioread32(base + offset);
	val &= mask;		/* clear other bits */
	val >>= shift;
	return val;
}

/**
 *
 * Write register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg(void *base, u32 offset, u32 val)
{
	iowrite32(val, base + offset);
}

/**
 * Write register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg_field(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 tmp = ioread32(base + offset);

	tmp &= ~mask;		/* clear written bits */
	val = tmp | (val << shift);
	iowrite32(val, base + offset);
}

/**
 * Write register and read back masked value to confirm it is written
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask specifying what should be updated
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_readback(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 write_val, tmp = ioread32(base + offset);

	tmp &= ~mask;		/* retain other bits */
	write_val = tmp | val;

	iowrite32(write_val, base + offset);

	/* Read back to see if val was written */
	tmp = ioread32(base + offset);
	tmp &= mask;		/* clear other bits */

	if (tmp != val)
		pr_err("%s: write: %x to QSCRATCH: %x FAILED\n",
			__func__, val, offset);
}

static bool dwc3_msm_is_host_superspeed(struct dwc3_msm *mdwc)
{
	int i, num_ports;
	u32 reg;

	reg = dwc3_msm_read_reg(mdwc->base, USB3_HCSPARAMS1);
	num_ports = HCS_MAX_PORTS(reg);

	for (i = 0; i < num_ports; i++) {
		reg = dwc3_msm_read_reg(mdwc->base, USB3_PORTSC + i*0x10);
		if ((reg & PORT_PE) && DEV_SUPERSPEED(reg))
			return true;
	}

	return false;
}

static inline bool dwc3_msm_is_dev_superspeed(struct dwc3_msm *mdwc)
{
	u8 speed;

	speed = dwc3_msm_read_reg(mdwc->base, DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;
	return !!(speed & DSTS_CONNECTSPD_SS);
}

static inline bool dwc3_msm_is_superspeed(struct dwc3_msm *mdwc)
{
	if (mdwc->in_host_mode)
		return dwc3_msm_is_host_superspeed(mdwc);

	return dwc3_msm_is_dev_superspeed(mdwc);
}

int dwc3_msm_dbm_disable_updxfer(struct dwc3 *dwc, u8 usb_ep)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	dev_dbg(mdwc->dev, "%s\n", __func__);
	dwc3_dbm_disable_update_xfer(mdwc->dbm, usb_ep);

	return 0;
}

/**
 * Configure the DBM with the BAM's data fifo.
 * This function is called by the USB BAM Driver
 * upon initialization.
 *
 * @ep - pointer to usb endpoint.
 * @addr - address of data fifo.
 * @size - size of data fifo.
 *
 */
int msm_data_fifo_config(struct usb_ep *ep, phys_addr_t addr,
			 u32 size, u8 dst_pipe_idx)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	dev_dbg(mdwc->dev, "%s\n", __func__);

	return	dbm_data_fifo_config(mdwc->dbm, dep->number, addr, size,
						dst_pipe_idx);
}


/**
* Cleanups for msm endpoint on request complete.
*
* Also call original request complete.
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
*
* @return int - 0 on success, negative on error.
*/
static void dwc3_msm_req_complete_func(struct usb_ep *ep,
				       struct usb_request *request)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete = NULL;

	/* Find original request complete function and remove it from list */
	list_for_each_entry(req_complete, &mdwc->req_complete_list, list_item) {
		if (req_complete->req == request)
			break;
	}
	if (!req_complete || req_complete->req != request) {
		dev_err(dep->dwc->dev, "%s: could not find the request\n",
					__func__);
		return;
	}
	list_del(&req_complete->list_item);

	/*
	 * Release another one TRB to the pool since DBM queue took 2 TRBs
	 * (normal and link), and the dwc3/gadget.c :: dwc3_gadget_giveback
	 * released only one.
	 */
	dep->busy_slot++;

	/* Unconfigure dbm ep */
	dbm_ep_unconfig(mdwc->dbm, dep->number);

	/*
	 * If this is the last endpoint we unconfigured, than reset also
	 * the event buffers; unless unconfiguring the ep due to lpm,
	 * in which case the event buffer only gets reset during the
	 * block reset.
	 */
	if (0 == dbm_get_num_of_eps_configured(mdwc->dbm) &&
		!dbm_reset_ep_after_lpm(mdwc->dbm))
			dbm_event_buffer_config(mdwc->dbm, 0, 0, 0);

	/*
	 * Call original complete function, notice that dwc->lock is already
	 * taken by the caller of this function (dwc3_gadget_giveback()).
	 */
	request->complete = req_complete->orig_complete;
	if (request->complete)
		request->complete(ep, request);

	kfree(req_complete);
}


/**
* Helper function
*
* Reset  DBM endpoint.
*
* @mdwc - pointer to dwc3_msm instance.
* @dep - pointer to dwc3_ep instance.
*
* @return int - 0 on success, negative on error.
*/
static int __dwc3_msm_dbm_ep_reset(struct dwc3_msm *mdwc, struct dwc3_ep *dep)
{
	int ret;

	dev_dbg(mdwc->dev, "Resetting dbm endpoint %d\n", dep->number);

	/* Reset the dbm endpoint */
	ret = dbm_ep_soft_reset(mdwc->dbm, dep->number, true);
	if (ret) {
		dev_err(mdwc->dev, "%s: failed to assert dbm ep reset\n",
				__func__);
		return ret;
	}

	/*
	 * The necessary delay between asserting and deasserting the dbm ep
	 * reset is based on the number of active endpoints. If there is more
	 * than one endpoint, a 1 msec delay is required. Otherwise, a shorter
	 * delay will suffice.
	 */
	if (dbm_get_num_of_eps_configured(mdwc->dbm) > 1)
		usleep_range(1000, 1200);
	else
		udelay(10);
	ret = dbm_ep_soft_reset(mdwc->dbm, dep->number, false);
	if (ret) {
		dev_err(mdwc->dev, "%s: failed to deassert dbm ep reset\n",
				__func__);
		return ret;
	}

	return 0;
}

/**
* Reset the DBM endpoint which is linked to the given USB endpoint.
*
* @usb_ep - pointer to usb_ep instance.
*
* @return int - 0 on success, negative on error.
*/

int msm_dwc3_reset_dbm_ep(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	return __dwc3_msm_dbm_ep_reset(mdwc, dep);
}
EXPORT_SYMBOL(msm_dwc3_reset_dbm_ep);


/**
* Helper function.
* See the header of the dwc3_msm_ep_queue function.
*
* @dwc3_ep - pointer to dwc3_ep instance.
* @req - pointer to dwc3_request instance.
*
* @return int - 0 on success, negative on error.
*/
static int __dwc3_msm_ep_queue(struct dwc3_ep *dep, struct dwc3_request *req)
{
	struct dwc3_trb *trb;
	struct dwc3_trb *trb_link;
	struct dwc3_gadget_ep_cmd_params params;
	u32 cmd;
	int ret = 0;

	/* We push the request to the dep->req_queued list to indicate that
	 * this request is issued with start transfer. The request will be out
	 * from this list in 2 cases. The first is that the transfer will be
	 * completed (not if the transfer is endless using a circular TRBs with
	 * with link TRB). The second case is an option to do stop stransfer,
	 * this can be initiated by the function driver when calling dequeue.
	 */
	req->queued = true;
	list_add_tail(&req->list, &dep->req_queued);

	/* First, prepare a normal TRB, point to the fake buffer */
	trb = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb, 0, sizeof(*trb));

	req->trb = trb;
	trb->bph = DBM_TRB_BIT | DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb->size = DWC3_TRB_SIZE_LENGTH(req->request.length);
	trb->ctrl = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_HWO |
		DWC3_TRB_CTRL_CHN | (req->direction ? 0 : DWC3_TRB_CTRL_CSP);
	req->trb_dma = dwc3_trb_dma_offset(dep, trb);

	/* Second, prepare a Link TRB that points to the first TRB*/
	trb_link = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb_link, 0, sizeof *trb_link);

	trb_link->bpl = lower_32_bits(req->trb_dma);
	trb_link->bph = DBM_TRB_BIT |
			DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb_link->size = 0;
	trb_link->ctrl = DWC3_TRBCTL_LINK_TRB | DWC3_TRB_CTRL_HWO;

	/*
	 * Now start the transfer
	 */
	memset(&params, 0, sizeof(params));
	params.param0 = 0; /* TDAddr High */
	params.param1 = lower_32_bits(req->trb_dma); /* DAddr Low */

	/* DBM requires IOC to be set */
	cmd = DWC3_DEPCMD_STARTTRANSFER | DWC3_DEPCMD_CMDIOC;
	ret = dwc3_send_gadget_ep_cmd(dep->dwc, dep->number, cmd, &params);
	if (ret < 0) {
		dev_dbg(dep->dwc->dev,
			"%s: failed to send STARTTRANSFER command\n",
			__func__);

		list_del(&req->list);
		return ret;
	}
	dep->flags |= DWC3_EP_BUSY;
	dep->resource_index = dwc3_gadget_ep_get_transfer_index(dep->dwc,
		dep->number);

	return ret;
}

/**
* Queue a usb request to the DBM endpoint.
* This function should be called after the endpoint
* was enabled by the ep_enable.
*
* This function prepares special structure of TRBs which
* is familiar with the DBM HW, so it will possible to use
* this endpoint in DBM mode.
*
* The TRBs prepared by this function, is one normal TRB
* which point to a fake buffer, followed by a link TRB
* that points to the first TRB.
*
* The API of this function follow the regular API of
* usb_ep_queue (see usb_ep_ops in include/linuk/usb/gadget.h).
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
* @gfp_flags - possible flags.
*
* @return int - 0 on success, negative on error.
*/
static int dwc3_msm_ep_queue(struct usb_ep *ep,
			     struct usb_request *request, gfp_t gfp_flags)
{
	struct dwc3_request *req = to_dwc3_request(request);
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete;
	unsigned long flags;
	int ret = 0, size;
	bool superspeed;

	/*
	 * We must obtain the lock of the dwc3 core driver,
	 * including disabling interrupts, so we will be sure
	 * that we are the only ones that configure the HW device
	 * core and ensure that we queuing the request will finish
	 * as soon as possible so we will release back the lock.
	 */
	spin_lock_irqsave(&dwc->lock, flags);
	if (!dep->endpoint.desc) {
		dev_err(mdwc->dev,
			"%s: trying to queue request %pK to disabled ep %s\n",
			__func__, request, ep->name);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EPERM;
	}

	if (!mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] was unconfigured as msm endpoint\n",
			ep->name, dep->number);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EINVAL;
	}

	if (!request) {
		dev_err(mdwc->dev, "%s: request is NULL\n", __func__);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EINVAL;
	}

	if (!(request->udc_priv & MSM_SPS_MODE)) {
		dev_err(mdwc->dev, "%s: sps mode is not set\n",
					__func__);

		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EINVAL;
	}

	/* HW restriction regarding TRB size (8KB) */
	if (req->request.length < 0x2000) {
		dev_err(mdwc->dev, "%s: Min TRB size is 8KB\n", __func__);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EINVAL;
	}

	if (dep->number == 0 || dep->number == 1) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %pK to control ep %s\n",
			__func__, request, ep->name);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EPERM;
	}

	if (dep->busy_slot != dep->free_slot || !list_empty(&dep->request_list)
					 || !list_empty(&dep->req_queued)) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %pK tp ep %s\n",
			__func__, request, ep->name);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EPERM;
	}
	dep->busy_slot = 0;
	dep->free_slot = 0;

	/*
	 * Override req->complete function, but before doing that,
	 * store it's original pointer in the req_complete_list.
	 */
	req_complete = kzalloc(sizeof(*req_complete), gfp_flags);
	if (!req_complete) {
		dev_err(mdwc->dev, "%s: not enough memory\n", __func__);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -ENOMEM;
	}
	req_complete->req = request;
	req_complete->orig_complete = request->complete;
	list_add_tail(&req_complete->list_item, &mdwc->req_complete_list);
	request->complete = dwc3_msm_req_complete_func;

	dev_vdbg(dwc->dev, "%s: queing request %pK to ep %s length %d\n",
			__func__, request, ep->name, request->length);
	size = dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTSIZ(0));
	dbm_event_buffer_config(mdwc->dbm,
		dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTADRLO(0)),
		dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTADRHI(0)),
		DWC3_GEVNTSIZ_SIZE(size));

	ret = __dwc3_msm_ep_queue(dep, req);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling __dwc3_msm_ep_queue\n", ret);
		goto err;
	}

	spin_unlock_irqrestore(&dwc->lock, flags);
	superspeed = dwc3_msm_is_dev_superspeed(mdwc);
	dbm_set_speed(mdwc->dbm, (u8)superspeed);

	return 0;

err:
	list_del(&req_complete->list_item);
	spin_unlock_irqrestore(&dwc->lock, flags);
	kfree(req_complete);
	return ret;
}

/*
* Returns XferRscIndex for the EP. This is stored at StartXfer GSI EP OP
*
* @usb_ep - pointer to usb_ep instance.
*
* @return int - XferRscIndex
*/
static inline int gsi_get_xfer_index(struct usb_ep *ep)
{
	struct dwc3_ep			*dep = to_dwc3_ep(ep);

	return dep->resource_index;
}

/*
* Fills up the GSI channel information needed in call to IPA driver
* for GSI channel creation.
*
* @usb_ep - pointer to usb_ep instance.
* @ch_info - output parameter with requested channel info
*/
static void gsi_get_channel_info(struct usb_ep *ep,
			struct gsi_channel_info *ch_info)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	int last_trb_index = 0;
	struct dwc3	*dwc = dep->dwc;
	struct usb_gsi_request *request = ch_info->ch_req;

	/* Provide physical USB addresses for DEPCMD and GEVENTCNT registers */
	ch_info->depcmd_low_addr = (u32)(dwc->reg_phys +
						DWC3_DEPCMD(dep->number));
	ch_info->depcmd_hi_addr = 0;

	ch_info->xfer_ring_base_addr = dwc3_trb_dma_offset(dep,
							&dep->trb_pool[0]);
	/* Convert to multipled of 1KB */
	ch_info->const_buffer_size = request->buf_len/1024;

	/* IN direction */
	if (dep->direction) {
		/*
		 * Multiply by size of each TRB for xfer_ring_len in bytes.
		 * 2n + 2 TRBs as per GSI h/w requirement. n Xfer TRBs + 1
		 * extra Xfer TRB followed by n ZLP TRBs + 1 LINK TRB.
		 */
		ch_info->xfer_ring_len = (2 * request->num_bufs + 2) * 0x10;
		last_trb_index = 2 * request->num_bufs + 2;
	} else { /* OUT direction */
		/*
		 * Multiply by size of each TRB for xfer_ring_len in bytes.
		 * n + 1 TRBs as per GSI h/w requirement. n Xfer TRBs + 1
		 * LINK TRB.
		 */
		ch_info->xfer_ring_len = (request->num_bufs + 2) * 0x10;
		last_trb_index = request->num_bufs + 2;
	}

	/* Store last 16 bits of LINK TRB address as per GSI hw requirement */
	ch_info->last_trb_addr = (dwc3_trb_dma_offset(dep,
			&dep->trb_pool[last_trb_index - 1]) & 0x0000FFFF);
	ch_info->gevntcount_low_addr = (u32)(dwc->reg_phys +
			DWC3_GEVNTCOUNT(ep->ep_intr_num));
	ch_info->gevntcount_hi_addr = 0;

	dev_dbg(dwc->dev,
	"depcmd_laddr=%x last_trb_addr=%x gevtcnt_laddr=%x gevtcnt_haddr=%x",
		ch_info->depcmd_low_addr, ch_info->last_trb_addr,
		ch_info->gevntcount_low_addr, ch_info->gevntcount_hi_addr);
}

/*
* Perform StartXfer on GSI EP. Stores XferRscIndex.
*
* @usb_ep - pointer to usb_ep instance.
*
* @return int - 0 on success
*/
static int gsi_startxfer_for_ep(struct usb_ep *ep)
{
	int ret;
	struct dwc3_gadget_ep_cmd_params params;
	u32				cmd;
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3	*dwc = dep->dwc;

	memset(&params, 0, sizeof(params));
	params.param0 = GSI_TRB_ADDR_BIT_53_MASK | GSI_TRB_ADDR_BIT_55_MASK;
	params.param0 |= (ep->ep_intr_num << 16);
	params.param1 = lower_32_bits(dwc3_trb_dma_offset(dep,
						&dep->trb_pool[0]));
	cmd = DWC3_DEPCMD_STARTTRANSFER;
	cmd |= DWC3_DEPCMD_PARAM(0);
	ret = dwc3_send_gadget_ep_cmd(dwc, dep->number, cmd, &params);

	if (ret < 0)
		dev_dbg(dwc->dev, "Fail StrtXfr on GSI EP#%d\n", dep->number);
	dep->resource_index = dwc3_gadget_ep_get_transfer_index(dwc,
								dep->number);
	dev_dbg(dwc->dev, "XferRsc = %x", dep->resource_index);
	return ret;
}

/*
* Store Ring Base and Doorbell Address for GSI EP
* for GSI channel creation.
*
* @usb_ep - pointer to usb_ep instance.
* @dbl_addr - Doorbell address obtained from IPA driver
*/
static void gsi_store_ringbase_dbl_info(struct usb_ep *ep, u32 dbl_addr)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3	*dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	int n = ep->ep_intr_num - 1;

	dwc3_msm_write_reg(mdwc->base, GSI_RING_BASE_ADDR_L(n),
			dwc3_trb_dma_offset(dep, &dep->trb_pool[0]));
	dwc3_msm_write_reg(mdwc->base, GSI_DBL_ADDR_L(n), dbl_addr);

	dev_dbg(mdwc->dev, "Ring Base Addr %d = %x", n,
			dwc3_msm_read_reg(mdwc->base, GSI_RING_BASE_ADDR_L(n)));
	dev_dbg(mdwc->dev, "GSI DB Addr %d = %x", n,
			dwc3_msm_read_reg(mdwc->base, GSI_DBL_ADDR_L(n)));
}

/*
* Rings Doorbell for GSI Channel
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to GSI request. This is used to pass in the
* address of the GSI doorbell obtained from IPA driver
*/
static void gsi_ring_db(struct usb_ep *ep, struct usb_gsi_request *request)
{
	void __iomem *gsi_dbl_address_lsb;
	void __iomem *gsi_dbl_address_msb;
	dma_addr_t offset;
	u64 dbl_addr = *((u64 *)request->buf_base_addr);
	u32 dbl_lo_addr = (dbl_addr & 0xFFFFFFFF);
	u32 dbl_hi_addr = (dbl_addr >> 32);
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3	*dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	int num_trbs = (dep->direction) ? (2 * (request->num_bufs) + 2)
					: (request->num_bufs + 2);

	gsi_dbl_address_lsb = devm_ioremap_nocache(mdwc->dev,
					dbl_lo_addr, sizeof(u32));
	if (!gsi_dbl_address_lsb) {
		dev_err(mdwc->dev, "Failed to get GSI DBL address LSB\n");
		return;
	}

	gsi_dbl_address_msb = devm_ioremap_nocache(mdwc->dev,
					dbl_hi_addr, sizeof(u32));
	if (!gsi_dbl_address_msb) {
		dev_err(mdwc->dev, "Failed to get GSI DBL address MSB\n");
		return;
	}

	offset = dwc3_trb_dma_offset(dep, &dep->trb_pool[num_trbs-1]);
	dev_dbg(mdwc->dev, "Writing link TRB addr:%pKa to %pK (%x) for ep:%s\n",
		&offset, gsi_dbl_address_lsb, dbl_lo_addr, ep->name);

	writel_relaxed(offset, gsi_dbl_address_lsb);
	writel_relaxed(0, gsi_dbl_address_msb);
}

/*
* Sets HWO bit for TRBs and performs UpdateXfer for OUT EP.
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to GSI request. Used to determine num of TRBs for OUT EP.
*
* @return int - 0 on success
*/
static int gsi_updatexfer_for_ep(struct usb_ep *ep,
					struct usb_gsi_request *request)
{
	int i;
	int ret;
	u32				cmd;
	int num_trbs = request->num_bufs + 1;
	struct dwc3_trb *trb;
	struct dwc3_gadget_ep_cmd_params params;
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;

	for (i = 0; i < num_trbs - 1; i++) {
		trb = &dep->trb_pool[i];
		trb->ctrl |= DWC3_TRB_CTRL_HWO;
	}

	memset(&params, 0, sizeof(params));
	cmd = DWC3_DEPCMD_UPDATETRANSFER;
	cmd |= DWC3_DEPCMD_PARAM(dep->resource_index);
	ret = dwc3_send_gadget_ep_cmd(dwc, dep->number, cmd, &params);
	dep->flags |= DWC3_EP_BUSY;
	if (ret < 0)
		dev_dbg(dwc->dev, "UpdateXfr fail on GSI EP#%d\n", dep->number);
	return ret;
}

/*
* Perform EndXfer on particular GSI EP.
*
* @usb_ep - pointer to usb_ep instance.
*/
static void gsi_endxfer_for_ep(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3	*dwc = dep->dwc;

	dwc3_stop_active_transfer(dwc, dep->number, true);
}

/*
* Allocates and configures TRBs for GSI EPs.
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to GSI request.
*
* @return int - 0 on success
*/
static int gsi_prepare_trbs(struct usb_ep *ep, struct usb_gsi_request *req)
{
	int i = 0;
	dma_addr_t buffer_addr = req->dma;
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3		*dwc = dep->dwc;
	struct dwc3_trb *trb;
	int num_trbs = (dep->direction) ? (2 * (req->num_bufs) + 2)
					: (req->num_bufs + 2);

	dep->trb_dma_pool = dma_pool_create(ep->name, dwc->dev,
					num_trbs * sizeof(struct dwc3_trb),
					num_trbs * sizeof(struct dwc3_trb), 0);
	if (!dep->trb_dma_pool) {
		dev_err(dep->dwc->dev, "failed to alloc trb dma pool for %s\n",
				dep->name);
		return -ENOMEM;
	}

	dep->num_trbs = num_trbs;

	dep->trb_pool = dma_pool_alloc(dep->trb_dma_pool,
					   GFP_KERNEL, &dep->trb_pool_dma);
	if (!dep->trb_pool) {
		dev_err(dep->dwc->dev, "failed to allocate trb pool for %s\n",
				dep->name);
		return -ENOMEM;
	}

	/* IN direction */
	if (dep->direction) {
		for (i = 0; i < num_trbs ; i++) {
			trb = &dep->trb_pool[i];
			memset(trb, 0, sizeof(*trb));
			/* Set up first n+1 TRBs for ZLPs */
			if (i < (req->num_bufs + 1)) {
				trb->bpl = 0;
				trb->bph = 0;
				trb->size = 0;
				trb->ctrl = DWC3_TRBCTL_NORMAL
						| DWC3_TRB_CTRL_IOC;
				continue;
			}

			/* Setup n TRBs pointing to valid buffers */
			trb->bpl = lower_32_bits(buffer_addr);
			trb->bph = 0;
			trb->size = 0;
			trb->ctrl = DWC3_TRBCTL_NORMAL
					| DWC3_TRB_CTRL_IOC;
			buffer_addr += req->buf_len;

			/* Set up the Link TRB at the end */
			if (i == (num_trbs - 1)) {
				trb->bpl = dwc3_trb_dma_offset(dep,
							&dep->trb_pool[0]);
				trb->bph = (1 << 23) | (1 << 21)
						| (ep->ep_intr_num << 16);
				trb->size = 0;
				trb->ctrl = DWC3_TRBCTL_LINK_TRB
						| DWC3_TRB_CTRL_HWO;
			}
		}
	} else { /* OUT direction */

		for (i = 0; i < num_trbs ; i++) {

			trb = &dep->trb_pool[i];
			memset(trb, 0, sizeof(*trb));
			/* Setup LINK TRB to start with TRB ring */
			if (i == 0) {
				trb->bpl = dwc3_trb_dma_offset(dep,
							&dep->trb_pool[1]);
				trb->ctrl = DWC3_TRBCTL_LINK_TRB;
			} else if (i == (num_trbs - 1)) {
				/* Set up the Link TRB at the end */
				trb->bpl = dwc3_trb_dma_offset(dep,
						&dep->trb_pool[0]);
				trb->bph = (1 << 23) | (1 << 21)
						| (ep->ep_intr_num << 16);
				trb->ctrl = DWC3_TRBCTL_LINK_TRB
						| DWC3_TRB_CTRL_HWO;
			} else {
				trb->bpl = lower_32_bits(buffer_addr);
				trb->size = req->buf_len;
				buffer_addr += req->buf_len;
				trb->ctrl = DWC3_TRBCTL_NORMAL
					| DWC3_TRB_CTRL_IOC
					| DWC3_TRB_CTRL_CSP
					| DWC3_TRB_CTRL_ISP_IMI;
			}
		 }
	}

	pr_debug("%s: Initialized TRB Ring for %s\n", __func__, dep->name);
	trb = &dep->trb_pool[0];
	if (trb) {
		for (i = 0; i < num_trbs; i++) {
			pr_debug("TRB(%d): ADDRESS:%lx bpl:%x bph:%x size:%x ctrl:%x\n",
				i, (unsigned long)dwc3_trb_dma_offset(dep,
				&dep->trb_pool[i]), trb->bpl, trb->bph,
				trb->size, trb->ctrl);
			trb++;
		}
	}

	return 0;
}

/*
* Frees TRBs for GSI EPs.
*
* @usb_ep - pointer to usb_ep instance.
*
*/
static void gsi_free_trbs(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);

	if (dep->endpoint.ep_type == EP_TYPE_NORMAL)
		return;

	/*  Free TRBs and TRB pool for EP */
	if (dep->trb_dma_pool) {
		dma_pool_free(dep->trb_dma_pool, dep->trb_pool,
						dep->trb_pool_dma);
		dma_pool_destroy(dep->trb_dma_pool);
		dep->trb_pool = NULL;
		dep->trb_pool_dma = 0;
		dep->trb_dma_pool = NULL;
	}
}
/*
* Configures GSI EPs. For GSI EPs we need to set interrupter numbers.
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to GSI request.
*/
static void gsi_configure_ep(struct usb_ep *ep, struct usb_gsi_request *request)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3		*dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_gadget_ep_cmd_params params;
	const struct usb_endpoint_descriptor *desc = ep->desc;
	const struct usb_ss_ep_comp_descriptor *comp_desc = ep->comp_desc;
	u32 reg;
	int ret;

	memset(&params, 0x00, sizeof(params));

	/* Configure GSI EP */
	params.param0 = DWC3_DEPCFG_EP_TYPE(usb_endpoint_type(desc))
		| DWC3_DEPCFG_MAX_PACKET_SIZE(usb_endpoint_maxp(desc));

	/* Burst size is only needed in SuperSpeed mode */
	if (dwc->gadget.speed == USB_SPEED_SUPER) {
		u32 burst = dep->endpoint.maxburst - 1;

		params.param0 |= DWC3_DEPCFG_BURST_SIZE(burst);
	}

	if (usb_ss_max_streams(comp_desc) && usb_endpoint_xfer_bulk(desc)) {
		params.param1 |= DWC3_DEPCFG_STREAM_CAPABLE
					| DWC3_DEPCFG_STREAM_EVENT_EN;
		dep->stream_capable = true;
	}

	/* Set EP number */
	params.param1 |= DWC3_DEPCFG_EP_NUMBER(dep->number);

	/* Set interrupter number for GSI endpoints */
	params.param1 |= DWC3_DEPCFG_INT_NUM(ep->ep_intr_num);

	/* Enable XferInProgress and XferComplete Interrupts */
	params.param1 |= DWC3_DEPCFG_XFER_COMPLETE_EN;
	params.param1 |= DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
	params.param1 |= DWC3_DEPCFG_FIFO_ERROR_EN;
	/*
	 * We must use the lower 16 TX FIFOs even though
	 * HW might have more
	 */
	/* Remove FIFO Number for GSI EP*/
	if (dep->direction)
		params.param0 |= DWC3_DEPCFG_FIFO_NUMBER(dep->number >> 1);

	params.param0 |= DWC3_DEPCFG_ACTION_INIT;

	dev_dbg(mdwc->dev, "Set EP config to params = %x %x %x, for %s\n",
	params.param0, params.param1, params.param2, dep->name);

	dwc3_send_gadget_ep_cmd(dwc, dep->number,
				DWC3_DEPCMD_SETEPCONFIG, &params);

	/* Set XferRsc Index for GSI EP */
	if (!(dep->flags & DWC3_EP_ENABLED)) {
		ret = dwc3_gadget_resize_tx_fifos(dwc, dep);
		if (ret)
			return;

		memset(&params, 0x00, sizeof(params));
		params.param0 = DWC3_DEPXFERCFG_NUM_XFER_RES(1);
		dwc3_send_gadget_ep_cmd(dwc, dep->number,
				DWC3_DEPCMD_SETTRANSFRESOURCE, &params);

		dep->endpoint.desc = desc;
		dep->comp_desc = comp_desc;
		dep->type = usb_endpoint_type(desc);
		dep->flags |= DWC3_EP_ENABLED;
		reg = dwc3_readl(dwc->regs, DWC3_DALEPENA);
		reg |= DWC3_DALEPENA_EP(dep->number);
		dwc3_writel(dwc->regs, DWC3_DALEPENA, reg);
	}

}

/*
* Enables USB wrapper for GSI
*
* @usb_ep - pointer to usb_ep instance.
*/
static void gsi_enable(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	dwc3_msm_write_reg_field(mdwc->base,
			GSI_GENERAL_CFG_REG, GSI_CLK_EN_MASK, 1);
	dwc3_msm_write_reg_field(mdwc->base,
			GSI_GENERAL_CFG_REG, GSI_RESTART_DBL_PNTR_MASK, 1);
	dwc3_msm_write_reg_field(mdwc->base,
			GSI_GENERAL_CFG_REG, GSI_RESTART_DBL_PNTR_MASK, 0);
	dev_dbg(mdwc->dev, "%s: Enable GSI\n", __func__);
	dwc3_msm_write_reg_field(mdwc->base,
			GSI_GENERAL_CFG_REG, GSI_EN_MASK, 1);
}

/*
* Block or allow doorbell towards GSI
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to GSI request. In this case num_bufs is used as a bool
* to set or clear the doorbell bit
*/
static void gsi_set_clear_dbell(struct usb_ep *ep,
					bool block_db)
{

	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	dwc3_msm_write_reg_field(mdwc->base,
		GSI_GENERAL_CFG_REG, BLOCK_GSI_WR_GO_MASK, block_db);
}

/*
* Performs necessary checks before stopping GSI channels
*
* @usb_ep - pointer to usb_ep instance to access DWC3 regs
*/
static bool gsi_check_ready_to_suspend(struct usb_ep *ep, bool f_suspend)
{
	u32	timeout = 1500;
	u32	reg = 0;
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	while (dwc3_msm_read_reg_field(mdwc->base,
		GSI_IF_STS, GSI_WR_CTRL_STATE_MASK)) {
		if (!timeout--) {
			dev_err(mdwc->dev,
			"Unable to suspend GSI ch. WR_CTRL_STATE != 0\n");
			return false;
		}
	}
	/* Check for U3 only if we are not handling Function Suspend */
	if (!f_suspend) {
		reg = dwc3_readl(dwc->regs, DWC3_DSTS);
		if (DWC3_DSTS_USBLNKST(reg) != DWC3_LINK_STATE_U3) {
			dev_err(mdwc->dev, "Unable to suspend GSI ch\n");
			return false;
		}
	}

	return true;
}


/**
* Performs GSI operations or GSI EP related operations.
*
* @usb_ep - pointer to usb_ep instance.
* @op_data - pointer to opcode related data.
* @op - GSI related or GSI EP related op code.
*
* @return int - 0 on success, negative on error.
* Also returns XferRscIdx for GSI_EP_OP_GET_XFER_IDX.
*/
static int dwc3_msm_gsi_ep_op(struct usb_ep *ep,
		void *op_data, enum gsi_ep_op op)
{
	u32 ret = 0;
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_gsi_request *request;
	struct gsi_channel_info *ch_info;
	bool block_db, f_suspend;
	unsigned long flags;

	switch (op) {
	case GSI_EP_OP_PREPARE_TRBS:
		request = (struct usb_gsi_request *)op_data;
		dbg_print(0xFF, "PREPARE_TRB", 0, ep->name);
		ret = gsi_prepare_trbs(ep, request);
		break;
	case GSI_EP_OP_FREE_TRBS:
		dbg_print(0xFF, "FREE_TRB", 0, ep->name);
		gsi_free_trbs(ep);
		break;
	case GSI_EP_OP_CONFIG:
		request = (struct usb_gsi_request *)op_data;
		dbg_print(0xFF, "EP_CONFIG", 0, ep->name);
		spin_lock_irqsave(&dwc->lock, flags);
		gsi_configure_ep(ep, request);
		spin_unlock_irqrestore(&dwc->lock, flags);
		break;
	case GSI_EP_OP_STARTXFER:
		dbg_print(0xFF, "EP_STARTXFER", 0, ep->name);
		spin_lock_irqsave(&dwc->lock, flags);
		ret = gsi_startxfer_for_ep(ep);
		spin_unlock_irqrestore(&dwc->lock, flags);
		break;
	case GSI_EP_OP_GET_XFER_IDX:
		dbg_print(0xFF, "EP_GETXFERID", 0, ep->name);
		ret = gsi_get_xfer_index(ep);
		break;
	case GSI_EP_OP_STORE_DBL_INFO:
		dbg_print(0xFF, "EP_STRDBL", 0, ep->name);
		gsi_store_ringbase_dbl_info(ep, *((u32 *)op_data));
		break;
	case GSI_EP_OP_ENABLE_GSI:
		dbg_print(0xFF, "ENABLE_GSI", 0, ep->name);
		gsi_enable(ep);
		break;
	case GSI_EP_OP_GET_CH_INFO:
		ch_info = (struct gsi_channel_info *)op_data;
		dbg_print(0xFF, "GET_CH_INFO", 0, ep->name);
		gsi_get_channel_info(ep, ch_info);
		break;
	case GSI_EP_OP_RING_DB:
		request = (struct usb_gsi_request *)op_data;
		dbg_print(0xFF, "RING_DB", 0, ep->name);
		gsi_ring_db(ep, request);
		break;
	case GSI_EP_OP_UPDATEXFER:
		request = (struct usb_gsi_request *)op_data;
		dbg_print(0xFF, "EP_UPDATEXFER", 0, ep->name);
		spin_lock_irqsave(&dwc->lock, flags);
		ret = gsi_updatexfer_for_ep(ep, request);
		spin_unlock_irqrestore(&dwc->lock, flags);
		break;
	case GSI_EP_OP_ENDXFER:
		request = (struct usb_gsi_request *)op_data;
		dbg_print(0xFF, "EP_ENDXFER", 0, ep->name);
		spin_lock_irqsave(&dwc->lock, flags);
		gsi_endxfer_for_ep(ep);
		spin_unlock_irqrestore(&dwc->lock, flags);
		break;
	case GSI_EP_OP_SET_CLR_BLOCK_DBL:
		block_db = *((bool *)op_data);
		dbg_print(0xFF, "CLR_BLK_DBL", block_db, ep->name);
		gsi_set_clear_dbell(ep, block_db);
		break;
	case GSI_EP_OP_CHECK_FOR_SUSPEND:
		dbg_print(0xFF, "CHK_SUSPEND", 0, ep->name);
		f_suspend = *((bool *)op_data);
		ret = gsi_check_ready_to_suspend(ep, f_suspend);
		break;
	default:
		dev_err(mdwc->dev, "%s: Invalid opcode GSI EP\n", __func__);
	}

	return ret;
}

/**
 * Configure MSM endpoint.
 * This function do specific configurations
 * to an endpoint which need specific implementaion
 * in the MSM architecture.
 *
 * This function should be called by usb function/class
 * layer which need a support from the specific MSM HW
 * which wrap the USB3 core. (like GSI or DBM specific endpoints)
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negetive on error.
 */
int msm_ep_config(struct usb_ep *ep, struct usb_request *request)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *new_ep_ops;
	int ret = 0;
	u8 bam_pipe;
	bool producer;
	bool disable_wb;
	bool internal_mem;
	bool ioc;
	unsigned long flags;


	spin_lock_irqsave(&dwc->lock, flags);
	/* Save original ep ops for future restore*/
	if (mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] already configured as msm endpoint\n",
			ep->name, dep->number);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EPERM;
	}
	mdwc->original_ep_ops[dep->number] = ep->ops;

	/* Set new usb ops as we like */
	new_ep_ops = kzalloc(sizeof(struct usb_ep_ops), GFP_ATOMIC);
	if (!new_ep_ops) {
		dev_err(mdwc->dev,
			"%s: unable to allocate mem for new usb ep ops\n",
			__func__);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -ENOMEM;
	}
	(*new_ep_ops) = (*ep->ops);
	new_ep_ops->queue = dwc3_msm_ep_queue;
	new_ep_ops->gsi_ep_op = dwc3_msm_gsi_ep_op;
	ep->ops = new_ep_ops;

	if (!mdwc->dbm || !request || (dep->endpoint.ep_type == EP_TYPE_GSI)) {
		spin_unlock_irqrestore(&dwc->lock, flags);
		return 0;
	}

	/*
	 * Configure the DBM endpoint if required.
	 */
	bam_pipe = request->udc_priv & MSM_PIPE_ID_MASK;
	producer = ((request->udc_priv & MSM_PRODUCER) ? true : false);
	disable_wb = ((request->udc_priv & MSM_DISABLE_WB) ? true : false);
	internal_mem = ((request->udc_priv & MSM_INTERNAL_MEM) ? true : false);
	ioc = ((request->udc_priv & MSM_ETD_IOC) ? true : false);

	ret = dbm_ep_config(mdwc->dbm, dep->number, bam_pipe, producer,
					disable_wb, internal_mem, ioc);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling dbm_ep_config\n", ret);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return ret;
	}

	spin_unlock_irqrestore(&dwc->lock, flags);

	return 0;
}
EXPORT_SYMBOL(msm_ep_config);

/**
 * Un-configure MSM endpoint.
 * Tear down configurations done in the
 * dwc3_msm_ep_config function.
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negative on error.
 */
int msm_ep_unconfig(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *old_ep_ops;
	unsigned long flags;

	spin_lock_irqsave(&dwc->lock, flags);
	/* Restore original ep ops */
	if (!mdwc->original_ep_ops[dep->number]) {
		dev_dbg(mdwc->dev,
			"ep [%s,%d] was not configured as msm endpoint\n",
			ep->name, dep->number);
		spin_unlock_irqrestore(&dwc->lock, flags);
		return -EINVAL;
	}
	old_ep_ops = (struct usb_ep_ops	*)ep->ops;
	ep->ops = mdwc->original_ep_ops[dep->number];
	mdwc->original_ep_ops[dep->number] = NULL;
	kfree(old_ep_ops);

	/*
	 * Do HERE more usb endpoint un-configurations
	 * which are specific to MSM.
	 */
	if (!mdwc->dbm || (dep->endpoint.ep_type == EP_TYPE_GSI)) {
		spin_unlock_irqrestore(&dwc->lock, flags);
		return 0;
	}

	if (dep->busy_slot == dep->free_slot && list_empty(&dep->request_list)
					 && list_empty(&dep->req_queued)) {
		dev_dbg(mdwc->dev,
			"%s: request is not queued, disable DBM ep for ep %s\n",
			__func__, ep->name);
		/* Unconfigure dbm ep */
		dbm_ep_unconfig(mdwc->dbm, dep->number);

		/*
		 * If this is the last endpoint we unconfigured, than reset also
		 * the event buffers; unless unconfiguring the ep due to lpm,
		 * in which case the event buffer only gets reset during the
		 * block reset.
		 */
		if (0 == dbm_get_num_of_eps_configured(mdwc->dbm) &&
				!dbm_reset_ep_after_lpm(mdwc->dbm))
			dbm_event_buffer_config(mdwc->dbm, 0, 0, 0);
	}

	spin_unlock_irqrestore(&dwc->lock, flags);

	return 0;
}
EXPORT_SYMBOL(msm_ep_unconfig);
static void dwc3_resume_work(struct work_struct *w);

static void dwc3_restart_usb_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
						restart_usb_work);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	enum dwc3_chg_type chg_type;
	unsigned timeout = 50;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (atomic_read(&dwc->in_lpm) || !dwc->is_drd) {
		dev_dbg(mdwc->dev, "%s failed!!!\n", __func__);
		return;
	}

	/* guard against concurrent VBUS handling */
	mdwc->in_restart = true;

	if (!mdwc->vbus_active) {
		dev_dbg(mdwc->dev, "%s bailing out in disconnect\n", __func__);
		dwc->err_evt_seen = false;
		mdwc->in_restart = false;
		return;
	}

	dbg_event(0xFF, "RestartUSB", 0);
	chg_type = mdwc->chg_type;

	/* Reset active USB connection */
	dwc3_resume_work(&mdwc->resume_work.work);

	/* Make sure disconnect is processed before sending connect */
	while (--timeout && !pm_runtime_suspended(mdwc->dev))
		msleep(20);

	if (!timeout) {
		dev_dbg(mdwc->dev,
			"Not in LPM after disconnect, forcing suspend...\n");
		dbg_event(0xFF, "ReStart:RT SUSP",
			atomic_read(&mdwc->dev->power.usage_count));
		pm_runtime_suspend(mdwc->dev);
	}

	mdwc->in_restart = false;
	/* Force reconnect only if cable is still connected */
	if (mdwc->vbus_active) {
		mdwc->chg_type = chg_type;
		dwc3_resume_work(&mdwc->resume_work.work);
	}

	dwc->err_evt_seen = false;
	flush_delayed_work(&mdwc->sm_work);
}

static int msm_dwc3_usbdev_notify(struct notifier_block *self,
			unsigned long action, void *priv)
{
	struct dwc3_msm *mdwc = container_of(self, struct dwc3_msm, usbdev_nb);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	struct usb_bus *bus = priv;

	/* Interested only in recovery when HC dies */
	if (action != USB_BUS_DIED)
		return 0;

	dev_dbg(mdwc->dev, "%s initiate recovery from hc_died\n", __func__);
	/* Recovery already under process */
	if (mdwc->hc_died)
		return 0;

	if (bus->controller != &dwc->xhci->dev) {
		dev_dbg(mdwc->dev, "%s event for diff HCD\n", __func__);
		return 0;
	}

	mdwc->hc_died = true;
	queue_delayed_work(mdwc->sm_usb_wq, &mdwc->sm_work, 0);
	return 0;
}


/*
 * Check whether the DWC3 requires resetting the ep
 * after going to Low Power Mode (lpm)
 */
bool msm_dwc3_reset_ep_after_lpm(struct usb_gadget *gadget)
{
	struct dwc3 *dwc = container_of(gadget, struct dwc3, gadget);
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	return dbm_reset_ep_after_lpm(mdwc->dbm);
}
EXPORT_SYMBOL(msm_dwc3_reset_ep_after_lpm);

/*
 * Config Global Distributed Switch Controller (GDSC)
 * to support controller power collapse
 */
static int dwc3_msm_config_gdsc(struct dwc3_msm *mdwc, int on)
{
	int ret;

	if (IS_ERR_OR_NULL(mdwc->dwc3_gdsc))
		return -EPERM;

	if (on) {
		ret = regulator_enable(mdwc->dwc3_gdsc);
		if (ret) {
			dev_err(mdwc->dev, "unable to enable usb3 gdsc\n");
			return ret;
		}
	} else {
		ret = regulator_disable(mdwc->dwc3_gdsc);
		if (ret) {
			dev_err(mdwc->dev, "unable to disable usb3 gdsc\n");
			return ret;
		}
	}

	return ret;
}

static int dwc3_msm_link_clk_reset(struct dwc3_msm *mdwc, bool assert)
{
	int ret = 0;

	if (assert) {
		disable_irq(mdwc->pwr_event_irq);
		/* Using asynchronous block reset to the hardware */
		dev_dbg(mdwc->dev, "block_reset ASSERT\n");
		clk_disable_unprepare(mdwc->utmi_clk);
		clk_disable_unprepare(mdwc->sleep_clk);
		clk_disable_unprepare(mdwc->core_clk);
		clk_disable_unprepare(mdwc->iface_clk);
		ret = clk_reset(mdwc->core_clk, CLK_RESET_ASSERT);
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk assert failed\n");
	} else {
		dev_dbg(mdwc->dev, "block_reset DEASSERT\n");
		ret = clk_reset(mdwc->core_clk, CLK_RESET_DEASSERT);
		ndelay(200);
		clk_prepare_enable(mdwc->iface_clk);
		clk_prepare_enable(mdwc->core_clk);
		clk_prepare_enable(mdwc->sleep_clk);
		clk_prepare_enable(mdwc->utmi_clk);
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk deassert failed\n");
		enable_irq(mdwc->pwr_event_irq);
	}

	return ret;
}

static void dwc3_msm_update_ref_clk(struct dwc3_msm *mdwc)
{
	u32 guctl, gfladj = 0;

	guctl = dwc3_msm_read_reg(mdwc->base, DWC3_GUCTL);
	guctl &= ~DWC3_GUCTL_REFCLKPER;

	/* GFLADJ register is used starting with revision 2.50a */
	if (dwc3_msm_read_reg(mdwc->base, DWC3_GSNPSID) >= DWC3_REVISION_250A) {
		gfladj = dwc3_msm_read_reg(mdwc->base, DWC3_GFLADJ);
		gfladj &= ~DWC3_GFLADJ_REFCLK_240MHZDECR_PLS1;
		gfladj &= ~DWC3_GFLADJ_REFCLK_240MHZ_DECR;
		gfladj &= ~DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj &= ~DWC3_GFLADJ_REFCLK_FLADJ;
	}

	/* Refer to SNPS Databook Table 6-55 for calculations used */
	switch (mdwc->utmi_clk_rate) {
	case 19200000:
		guctl |= 52 << __ffs(DWC3_GUCTL_REFCLKPER);
		gfladj |= 12 << __ffs(DWC3_GFLADJ_REFCLK_240MHZ_DECR);
		gfladj |= DWC3_GFLADJ_REFCLK_240MHZDECR_PLS1;
		gfladj |= DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj |= 200 << __ffs(DWC3_GFLADJ_REFCLK_FLADJ);
		break;
	case 24000000:
		guctl |= 41 << __ffs(DWC3_GUCTL_REFCLKPER);
		gfladj |= 10 << __ffs(DWC3_GFLADJ_REFCLK_240MHZ_DECR);
		gfladj |= DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj |= 2032 << __ffs(DWC3_GFLADJ_REFCLK_FLADJ);
		break;
	default:
		dev_warn(mdwc->dev, "Unsupported utmi_clk_rate: %u\n",
				mdwc->utmi_clk_rate);
		break;
	}

	dwc3_msm_write_reg(mdwc->base, DWC3_GUCTL, guctl);
	if (gfladj)
		dwc3_msm_write_reg(mdwc->base, DWC3_GFLADJ, gfladj);
}

/* Initialize QSCRATCH registers for HSPHY and SSPHY operation */
static void dwc3_msm_qscratch_reg_init(struct dwc3_msm *mdwc)
{
	if (dwc3_msm_read_reg(mdwc->base, DWC3_GSNPSID) < DWC3_REVISION_250A)
		/* On older cores set XHCI_REV bit to specify revision 1.0 */
		dwc3_msm_write_reg_field(mdwc->base, QSCRATCH_GENERAL_CFG,
					 BIT(2), 1);

	/*
	 * Enable master clock for RAMs to allow BAM to access RAMs when
	 * RAM clock gating is enabled via DWC3's GCTL. Otherwise issues
	 * are seen where RAM clocks get turned OFF in SS mode
	 */
	dwc3_msm_write_reg(mdwc->base, CGCTL_REG,
		dwc3_msm_read_reg(mdwc->base, CGCTL_REG) | 0x18);

}

static void dwc3_msm_notify_event(struct dwc3 *dwc, unsigned event,
							unsigned value)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	u32 reg;

	if (dwc->revision < DWC3_REVISION_230A)
		return;

	switch (event) {
	case DWC3_CONTROLLER_ERROR_EVENT:
		dev_info(mdwc->dev,
			"DWC3_CONTROLLER_ERROR_EVENT received, irq cnt %lu\n",
			dwc->irq_cnt);

		dwc3_gadget_disable_irq(dwc);

		/* prevent core from generating interrupts until recovery */
		reg = dwc3_msm_read_reg(mdwc->base, DWC3_GCTL);
		reg |= DWC3_GCTL_CORESOFTRESET;
		dwc3_msm_write_reg(mdwc->base, DWC3_GCTL, reg);

		/* restart USB which performs full reset and reconnect */
		schedule_work(&mdwc->restart_usb_work);
		break;
	case DWC3_CONTROLLER_RESET_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_RESET_EVENT received\n");
		/* HS & SSPHYs get reset as part of core soft reset */
		dwc3_msm_qscratch_reg_init(mdwc);
		break;
	case DWC3_CONTROLLER_POST_RESET_EVENT:
		dev_dbg(mdwc->dev,
				"DWC3_CONTROLLER_POST_RESET_EVENT received\n");

		/*
		 * Below sequence is used when controller is working without
		 * having ssphy and only USB high/full speed is supported.
		 */
		if (dwc->maximum_speed == USB_SPEED_HIGH ||
					dwc->maximum_speed == USB_SPEED_FULL) {
			dwc3_msm_write_reg(mdwc->base, QSCRATCH_GENERAL_CFG,
				dwc3_msm_read_reg(mdwc->base,
				QSCRATCH_GENERAL_CFG)
				| PIPE_UTMI_CLK_DIS);

			usleep_range(2, 5);


			dwc3_msm_write_reg(mdwc->base, QSCRATCH_GENERAL_CFG,
				dwc3_msm_read_reg(mdwc->base,
				QSCRATCH_GENERAL_CFG)
				| PIPE_UTMI_CLK_SEL
				| PIPE3_PHYSTATUS_SW);

			usleep_range(2, 5);

			dwc3_msm_write_reg(mdwc->base, QSCRATCH_GENERAL_CFG,
				dwc3_msm_read_reg(mdwc->base,
				QSCRATCH_GENERAL_CFG)
				& ~PIPE_UTMI_CLK_DIS);
		}

		dwc3_msm_update_ref_clk(mdwc);
		dwc->tx_fifo_size = mdwc->tx_fifo_size;
		break;
	case DWC3_CONTROLLER_CONNDONE_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_CONNDONE_EVENT received\n");
		/*
		 * Add power event if the dbm indicates coming out of L1 by
		 * interrupt
		 */
		if (mdwc->dbm && dbm_l1_lpm_interrupt(mdwc->dbm))
			dwc3_msm_write_reg_field(mdwc->base,
					PWR_EVNT_IRQ_MASK_REG,
					PWR_EVNT_LPM_OUT_L1_MASK, 1);

		atomic_set(&dwc->in_lpm, 0);
		break;
	case DWC3_CONTROLLER_NOTIFY_OTG_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_NOTIFY_OTG_EVENT received\n");
		if (dwc->enable_bus_suspend) {
			mdwc->suspend = dwc->b_suspend;
			queue_delayed_work(mdwc->dwc3_resume_wq,
					&mdwc->resume_work, 0);
		}
		break;
	case DWC3_CONTROLLER_SET_CURRENT_DRAW_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_SET_CURRENT_DRAW_EVENT received\n");
		dwc3_msm_gadget_vbus_draw(mdwc, dwc->vbus_draw);
		break;
	case DWC3_CONTROLLER_RESTART_USB_SESSION:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_RESTART_USB_SESSION received\n");
		schedule_work(&mdwc->restart_usb_work);
		break;
	case DWC3_CONTROLLER_NOTIFY_DISABLE_UPDXFER:
		dwc3_msm_dbm_disable_updxfer(dwc, value);
		break;
		/* for notify otg from gadget, 7/8 */
	case DWC3_CONTROLLER_GADGET_EXTRA_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_GADGET_EXTRA_EVENT received\n");
		dwc3_extra_event_from_gadget(mdwc, dwc->extra_event);
		break;
	/* end */

	default:
		dev_dbg(mdwc->dev, "unknown dwc3 event\n");
		break;
	}
}

static void dwc3_msm_block_reset(struct dwc3_msm *mdwc, bool core_reset)
{
	int ret  = 0;

	if (core_reset) {
		ret = dwc3_msm_link_clk_reset(mdwc, 1);
		if (ret)
			return;

		usleep_range(1000, 1200);
		ret = dwc3_msm_link_clk_reset(mdwc, 0);
		if (ret)
			return;

		usleep_range(10000, 12000);
	}

	if (mdwc->dbm) {
		/* Reset the DBM */
		dbm_soft_reset(mdwc->dbm, 1);
		usleep_range(1000, 1200);
		dbm_soft_reset(mdwc->dbm, 0);

		/*enable DBM*/
		dwc3_msm_write_reg_field(mdwc->base, QSCRATCH_GENERAL_CFG,
			DBM_EN_MASK, 0x1);
		dbm_enable(mdwc->dbm);
	}
}

static const char *chg_to_string(enum dwc3_chg_type chg_type)
{
	switch (chg_type) {
	case DWC3_SDP_CHARGER:		return "USB_SDP_CHARGER";
	case DWC3_DCP_CHARGER:		return "USB_DCP_CHARGER";
	case DWC3_CDP_CHARGER:		return "USB_CDP_CHARGER";
	case DWC3_PROPRIETARY_CHARGER:	return "USB_PROPRIETARY_CHARGER";
	default:			return "UNKNOWN_CHARGER";
	}
}

static void dwc3_msm_power_collapse_por(struct dwc3_msm *mdwc)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	u32 val;

	/* Configure AHB2PHY for one wait state read/write */
	if (mdwc->ahb2phy_base) {
		clk_prepare_enable(mdwc->cfg_ahb_clk);
		val = readl_relaxed(mdwc->ahb2phy_base +
				PERIPH_SS_AHB2PHY_TOP_CFG);
		if (val != ONE_READ_WRITE_WAIT) {
			writel_relaxed(ONE_READ_WRITE_WAIT,
				mdwc->ahb2phy_base + PERIPH_SS_AHB2PHY_TOP_CFG);
			/* complete above write before configuring USB PHY. */
			mb();
		}
		clk_disable_unprepare(mdwc->cfg_ahb_clk);
	}

	dwc3_core_init(dwc);
	/* Re-configure event buffers */
	dwc3_event_buffers_setup(dwc);
	dwc3_msm_notify_event(dwc,
				DWC3_CONTROLLER_POST_INITIALIZATION_EVENT, 0);
}

static int dwc3_msm_prepare_suspend(struct dwc3_msm *mdwc)
{
	unsigned long timeout;
	u32 reg = 0;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	if ((mdwc->in_host_mode || (mdwc->vbus_active
			&& mdwc->otg_state == OTG_STATE_B_SUSPEND))
			&& dwc3_msm_is_superspeed(mdwc) && !mdwc->in_restart) {
		if (!atomic_read(&mdwc->in_p3)) {
			dev_err(mdwc->dev, "Not in P3,aborting LPM sequence\n");
			return -EBUSY;
		}
	}

	/* Clear previous L2 events */
	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG,
		PWR_EVNT_LPM_IN_L2_MASK | PWR_EVNT_LPM_OUT_L2_MASK);

	/* Prepare HSPHY for suspend */
	reg = dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0));
	dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
		reg | DWC3_GUSB2PHYCFG_ENBLSLPM | DWC3_GUSB2PHYCFG_SUSPHY);

	/* Wait for PHY to go into L2 */
	timeout = jiffies + msecs_to_jiffies(5);
	while (!time_after(jiffies, timeout)) {
		reg = dwc3_msm_read_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG);
		if (reg & PWR_EVNT_LPM_IN_L2_MASK)
			break;
	}

	if (!(reg & PWR_EVNT_LPM_IN_L2_MASK)) {
		dev_err(mdwc->dev, "could not transition HS PHY to L2\n");
		dbg_event(0xFF, "PWR_EVNT_LPM",
			dwc3_msm_read_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG));
		dbg_event(0xFF, "QUSB_STS",
			dwc3_msm_read_reg(mdwc->base, QSCRATCH_USB30_STS_REG));
		/* Mark fatal error for host mode or USB bus suspend case */
		if (mdwc->in_host_mode || (mdwc->vbus_active
			&& mdwc->otg_state == OTG_STATE_B_SUSPEND)) {
			queue_delayed_work(mdwc->dwc3_resume_wq,
					&mdwc->resume_work, 0);
			return -EBUSY;
		}
	}

	/* Clear L2 event bit */
	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG,
		PWR_EVNT_LPM_IN_L2_MASK);

	return 0;
}

static void dwc3_msm_perf_vote_update(struct dwc3_msm *mdwc,
					enum dwc3_perf_mode mode);

static void dwc3_set_phy_speed_flags(struct dwc3_msm *mdwc)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	int i, num_ports;
	u32 reg;

	mdwc->hs_phy->flags &= ~(PHY_HSFS_MODE | PHY_LS_MODE);
	if (mdwc->in_host_mode) {
		reg = dwc3_msm_read_reg(mdwc->base, USB3_HCSPARAMS1);
		num_ports = HCS_MAX_PORTS(reg);
		for (i = 0; i < num_ports; i++) {
			reg = dwc3_msm_read_reg(mdwc->base,
					USB3_PORTSC + i*0x10);
			if (reg & PORT_PE) {
				if (DEV_HIGHSPEED(reg) || DEV_FULLSPEED(reg))
					mdwc->hs_phy->flags |= PHY_HSFS_MODE;
				else if (DEV_LOWSPEED(reg))
					mdwc->hs_phy->flags |= PHY_LS_MODE;
			}
		}
	} else {
		if (dwc->gadget.speed == USB_SPEED_HIGH ||
			dwc->gadget.speed == USB_SPEED_FULL)
			mdwc->hs_phy->flags |= PHY_HSFS_MODE;
		else if (dwc->gadget.speed == USB_SPEED_LOW)
			mdwc->hs_phy->flags |= PHY_LS_MODE;
	}
}


static int dwc3_msm_suspend(struct dwc3_msm *mdwc)
{
	int ret, i;
	bool can_suspend_ssphy;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dbg_event(0xFF, "Ctl Sus", atomic_read(&dwc->in_lpm));

	if (atomic_read(&dwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already suspended\n", __func__);
		dbg_event(0xFF, "AlreadySUS", 0);
		return 0;
	}

	if (!mdwc->in_host_mode) {
		/* pending device events unprocessed */
		for (i = 0; i < dwc->num_event_buffers; i++) {
			struct dwc3_event_buffer *evt = dwc->ev_buffs[i];
			if ((evt->flags & DWC3_EVENT_PENDING)) {
				dev_dbg(mdwc->dev,
				"%s: %d device events pending, abort suspend\n",
				__func__, evt->count / 4);
				dbg_print_reg("PENDING DEVICE EVENT",
						*(u32 *)(evt->buf + evt->lpos));
				return -EBUSY;
			}
		}
	}

	if (!mdwc->vbus_active && dwc->is_drd &&
		mdwc->otg_state == OTG_STATE_B_PERIPHERAL) {
		/*
		 * In some cases, the pm_runtime_suspend may be called by
		 * usb_bam when there is pending lpm flag. However, if this is
		 * done when cable was disconnected and otg state has not
		 * yet changed to IDLE, then it means OTG state machine
		 * is running and we race against it. So cancel LPM for now,
		 * and OTG state machine will go for LPM later, after completing
		 * transition to IDLE state.
		*/
		dev_dbg(mdwc->dev,
			"%s: cable disconnected while not in idle otg state\n",
			__func__);
		return -EBUSY;
	}

	/*
	 * Check if device is not in CONFIGURED state
	 * then check controller state of L2 and break
	 * LPM sequence. Check this for device bus suspend case.
	 */
	if ((dwc->is_drd && mdwc->otg_state == OTG_STATE_B_SUSPEND) &&
		(dwc->gadget.state != USB_STATE_CONFIGURED)) {
		pr_err("%s(): Trying to go in LPM with state:%d\n",
					__func__, dwc->gadget.state);
		pr_err("%s(): LPM is not performed.\n", __func__);
		return -EBUSY;
	}

	if (!mdwc->in_host_mode && (mdwc->vbus_active && !mdwc->suspend)) {
		dev_dbg(mdwc->dev,
			"Received wakeup event before the core suspend\n");
		return -EBUSY;
	}

	ret = dwc3_msm_prepare_suspend(mdwc);
	if (ret)
		return ret;

	/* Initialize variables here */
	can_suspend_ssphy = !(mdwc->in_host_mode &&
				dwc3_msm_is_host_superspeed(mdwc));

	/* Disable core irq */
	if (dwc->irq)
		disable_irq(dwc->irq);

	/* disable power event irq, hs and ss phy irq is used as wake up src */
	disable_irq(mdwc->pwr_event_irq);

	dwc3_set_phy_speed_flags(mdwc);
	/* Suspend HS PHY */
	usb_phy_set_suspend(mdwc->hs_phy, 1);

	/* Suspend SS PHY */
	if (can_suspend_ssphy) {
		/* indicate phy about SS mode */
		if (dwc3_msm_is_superspeed(mdwc))
			mdwc->ss_phy->flags |= DEVICE_IN_SS_MODE;
		usb_phy_set_suspend(mdwc->ss_phy, 1);
		mdwc->lpm_flags |= MDWC3_SS_PHY_SUSPEND;
	}

	/* make sure above writes are completed before turning off clocks */
	wmb();

	/* Disable clocks */
	if (mdwc->bus_aggr_clk)
		clk_disable_unprepare(mdwc->bus_aggr_clk);
	clk_disable_unprepare(mdwc->utmi_clk);

	clk_set_rate(mdwc->core_clk, 19200000);
	clk_disable_unprepare(mdwc->core_clk);
	/*
	 * Disable iface_clk only after core_clk as core_clk has FSM
	 * depedency on iface_clk. Hence iface_clk should be turned off
	 * after core_clk is turned off.
	 */
	clk_disable_unprepare(mdwc->iface_clk);
	/* USB PHY no more requires TCXO */
	clk_disable_unprepare(mdwc->xo_clk);

	/* Perform controller power collapse */
	if (!mdwc->in_host_mode && (!mdwc->vbus_active ||
				    mdwc->otg_state == OTG_STATE_B_IDLE ||
				    mdwc->in_restart)) {
		mdwc->lpm_flags |= MDWC3_POWER_COLLAPSE;
		dev_dbg(mdwc->dev, "%s: power collapse\n", __func__);
		dwc3_msm_config_gdsc(mdwc, 0);
		clk_disable_unprepare(mdwc->sleep_clk);
	}

	cancel_delayed_work_sync(&mdwc->perf_vote_work);
	dwc3_msm_perf_vote_update(mdwc, DWC3_PERF_OFF);

	/*
	 * release wakeup source with timeout to defer system suspend to
	 * handle case where on USB cable disconnect, SUSPEND and DISCONNECT
	 * event is received.
	 */
	if (mdwc->lpm_to_suspend_delay) {
		dev_dbg(mdwc->dev, "defer suspend with %d(msecs)\n",
					mdwc->lpm_to_suspend_delay);
		pm_wakeup_event(mdwc->dev, mdwc->lpm_to_suspend_delay);
	} else {
		pm_relax(mdwc->dev);
	}

	atomic_set(&dwc->in_lpm, 1);

	/*
	 * with DCP or during cable disconnect, we dont require wakeup
	 * using HS_PHY_IRQ or SS_PHY_IRQ. Hence enable wakeup only in
	 * case of host bus suspend and device bus suspend.
	 */
	if ((mdwc->vbus_active && mdwc->otg_state == OTG_STATE_B_SUSPEND)
			|| mdwc->in_host_mode) {
		enable_irq_wake(mdwc->hs_phy_irq);
		enable_irq(mdwc->hs_phy_irq);
		if (mdwc->ss_phy_irq) {
			enable_irq_wake(mdwc->ss_phy_irq);
			enable_irq(mdwc->ss_phy_irq);
		}
		mdwc->lpm_flags |= MDWC3_ASYNC_IRQ_WAKE_CAPABILITY;
	}

	dev_info(mdwc->dev, "DWC3 in low power mode\n");
	dbg_event(0xFF, "SUSComplete", mdwc->lpm_to_suspend_delay);
	return 0;
}

static int dwc3_msm_resume(struct dwc3_msm *mdwc)
{
	int ret;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(mdwc->dev, "%s: exiting lpm\n", __func__);
	dbg_event(0xFF, "msmresume", atomic_read(&dwc->in_lpm));

	if (!atomic_read(&dwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already resumed\n", __func__);
		dbg_event(0xFF, "AlreadyRES", 0);
		return 0;
	}

	pm_stay_awake(mdwc->dev);

	/* Vote for TCXO while waking up USB HSPHY */
	ret = clk_prepare_enable(mdwc->xo_clk);
	if (ret)
		dev_err(mdwc->dev, "%s failed to vote TCXO buffer%d\n",
						__func__, ret);

	/* Restore controller power collapse */
	if (mdwc->lpm_flags & MDWC3_POWER_COLLAPSE) {
		dev_dbg(mdwc->dev, "%s: exit power collapse\n", __func__);
		dwc3_msm_config_gdsc(mdwc, 1);

		clk_reset(mdwc->core_clk, CLK_RESET_ASSERT);
		/* HW requires a short delay for reset to take place properly */
		usleep_range(1000, 1200);
		clk_reset(mdwc->core_clk, CLK_RESET_DEASSERT);
		clk_prepare_enable(mdwc->sleep_clk);
	}

	/*
	 * Enable clocks
	 * Turned ON iface_clk before core_clk due to FSM depedency.
	 */
	clk_prepare_enable(mdwc->iface_clk);
	clk_set_rate(mdwc->core_clk, mdwc->core_clk_rate);
	clk_prepare_enable(mdwc->core_clk);
	clk_prepare_enable(mdwc->utmi_clk);
	if (mdwc->bus_aggr_clk)
		clk_prepare_enable(mdwc->bus_aggr_clk);

	/* Resume SS PHY */
	if (mdwc->lpm_flags & MDWC3_SS_PHY_SUSPEND) {
		usb_phy_set_suspend(mdwc->ss_phy, 0);
		mdwc->ss_phy->flags &= ~DEVICE_IN_SS_MODE;
		mdwc->lpm_flags &= ~MDWC3_SS_PHY_SUSPEND;
	}

	mdwc->hs_phy->flags &= ~(PHY_HSFS_MODE | PHY_LS_MODE);
	/* Resume HS PHY */
	usb_phy_set_suspend(mdwc->hs_phy, 0);

	/* Recover from controller power collapse */
	if (mdwc->lpm_flags & MDWC3_POWER_COLLAPSE) {
		dev_dbg(mdwc->dev, "%s: exit power collapse\n", __func__);

		dwc3_msm_power_collapse_por(mdwc);

		/* Re-enable IN_P3 event */
		dwc3_msm_write_reg_field(mdwc->base, PWR_EVNT_IRQ_MASK_REG,
					PWR_EVNT_POWERDOWN_IN_P3_MASK, 1);

		mdwc->lpm_flags &= ~MDWC3_POWER_COLLAPSE;
	}

	atomic_set(&dwc->in_lpm, 0);

	/* enable power evt irq for IN P3 detection */
	enable_irq(mdwc->pwr_event_irq);

	/* Disable HSPHY auto suspend */
	dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
		dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) &
				~(DWC3_GUSB2PHYCFG_ENBLSLPM |
					DWC3_GUSB2PHYCFG_SUSPHY));

	/* Disable wakeup capable for HS_PHY IRQ & SS_PHY_IRQ if enabled */
	if (mdwc->lpm_flags & MDWC3_ASYNC_IRQ_WAKE_CAPABILITY) {
		disable_irq_wake(mdwc->hs_phy_irq);
		disable_irq_nosync(mdwc->hs_phy_irq);
		if (mdwc->ss_phy_irq) {
			disable_irq_wake(mdwc->ss_phy_irq);
			disable_irq_nosync(mdwc->ss_phy_irq);
		}
		mdwc->lpm_flags &= ~MDWC3_ASYNC_IRQ_WAKE_CAPABILITY;
	}

	dev_info(mdwc->dev, "DWC3 exited from low power mode\n");

	/* Enable core irq */
	if (dwc->irq)
		enable_irq(dwc->irq);

	dwc3_msm_perf_vote_update(mdwc, DWC3_PERF_NOM);
	if (mdwc->in_host_mode) {
		schedule_delayed_work(&mdwc->perf_vote_work,
			msecs_to_jiffies(1000 * PM_QOS_SAMPLE_SEC));
	}

	/*
	 * Handle other power events that could not have been handled during
	 * Low Power Mode
	 */
	dwc3_pwr_event_handler(mdwc);

	dbg_event(0xFF, "Ctl Res", atomic_read(&dwc->in_lpm));

	return 0;
}

/**
 * dwc3_ext_event_notify - callback to handle events from external transceiver
 *
 * Returns 0 on success
 */
static void dwc3_ext_event_notify(struct dwc3_msm *mdwc)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dbg_event(0xFF, "ext_event", 0);

	/*
	 * Flush processing any pending events before handling new ones except
	 * for initial notification during bootup.
	 */
	if (mdwc->init)
		flush_delayed_work(&mdwc->sm_work);

	if (mdwc->id_state == DWC3_ID_FLOAT) {
		dbg_event(0xFF, "ID set", 0);
		set_bit(ID, &mdwc->inputs);
	} else {
		dbg_event(0xFF, "ID clear", 0);
		clear_bit(ID, &mdwc->inputs);
	}

	if (mdwc->vbus_active && !mdwc->in_restart) {
		dbg_event(0xFF, "BSV set", 0);
		set_bit(B_SESS_VLD, &mdwc->inputs);
	} else {
		dbg_event(0xFF, "BSV clear", 0);
		clear_bit(B_SESS_VLD, &mdwc->inputs);
	}

	if (mdwc->suspend) {
		dbg_event(0xFF, "SUSP set", 0);
		set_bit(B_SUSPEND, &mdwc->inputs);
	} else {
		dbg_event(0xFF, "SUSP clear", 0);
		clear_bit(B_SUSPEND, &mdwc->inputs);
	}

	if (!mdwc->init) {
		mdwc->init = true;
		pm_runtime_set_autosuspend_delay(mdwc->dev, 1000);
		pm_runtime_use_autosuspend(mdwc->dev);
		if (!work_busy(&mdwc->sm_work.work))
			queue_delayed_work(mdwc->sm_usb_wq, &mdwc->sm_work, 0);
		return;
	}

	pm_stay_awake(mdwc->dev);
	queue_delayed_work(mdwc->sm_usb_wq, &mdwc->sm_work, 0);
}

static void dwc3_resume_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
							resume_work.work);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(mdwc->dev, "%s: dwc3 resume work\n", __func__);

	/*
	 * exit LPM first to meet resume timeline from device side.
	 * resume_pending flag would prevent calling
	 * dwc3_msm_resume() in case we are here due to system
	 * wide resume without usb cable connected. This flag is set
	 * only in case of power event irq in lpm.
	 */
	if (mdwc->resume_pending) {
		dwc3_msm_resume(mdwc);
		mdwc->resume_pending = false;
	}

	if (atomic_read(&mdwc->pm_suspended)) {
		dbg_event(0xFF, "RWrk PMSus", 0);
		/* let pm resume kick in resume work later */
		return;
	}

	dbg_event(0xFF, "RWrk", dwc->is_drd);
	dwc3_ext_event_notify(mdwc);
}

static void dwc3_pwr_event_handler(struct dwc3_msm *mdwc)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	u32 irq_stat, irq_clear = 0;

	irq_stat = dwc3_msm_read_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG);
	dev_dbg(mdwc->dev, "%s irq_stat=%X\n", __func__, irq_stat);

	/* Check for P3 events */
	if ((irq_stat & PWR_EVNT_POWERDOWN_OUT_P3_MASK) &&
			(irq_stat & PWR_EVNT_POWERDOWN_IN_P3_MASK)) {
		/* Can't tell if entered or exit P3, so check LINKSTATE */
		u32 ls = dwc3_msm_read_reg_field(mdwc->base,
				DWC3_GDBGLTSSM, DWC3_GDBGLTSSM_LINKSTATE_MASK);
		dev_dbg(mdwc->dev, "%s link state = 0x%04x\n", __func__, ls);
		atomic_set(&mdwc->in_p3, ls == DWC3_LINK_STATE_U3);

		irq_stat &= ~(PWR_EVNT_POWERDOWN_OUT_P3_MASK |
				PWR_EVNT_POWERDOWN_IN_P3_MASK);
		irq_clear |= (PWR_EVNT_POWERDOWN_OUT_P3_MASK |
				PWR_EVNT_POWERDOWN_IN_P3_MASK);
	} else if (irq_stat & PWR_EVNT_POWERDOWN_OUT_P3_MASK) {
		atomic_set(&mdwc->in_p3, 0);
		irq_stat &= ~PWR_EVNT_POWERDOWN_OUT_P3_MASK;
		irq_clear |= PWR_EVNT_POWERDOWN_OUT_P3_MASK;
	} else if (irq_stat & PWR_EVNT_POWERDOWN_IN_P3_MASK) {
		atomic_set(&mdwc->in_p3, 1);
		irq_stat &= ~PWR_EVNT_POWERDOWN_IN_P3_MASK;
		irq_clear |= PWR_EVNT_POWERDOWN_IN_P3_MASK;
	}

	/* Clear L2 exit */
	if (irq_stat & PWR_EVNT_LPM_OUT_L2_MASK) {
		irq_stat &= ~PWR_EVNT_LPM_OUT_L2_MASK;
		irq_stat |= PWR_EVNT_LPM_OUT_L2_MASK;
	}

	/* Handle exit from L1 events */
	if (irq_stat & PWR_EVNT_LPM_OUT_L1_MASK) {
		dev_dbg(mdwc->dev, "%s: handling PWR_EVNT_LPM_OUT_L1_MASK\n",
				__func__);
		if (usb_gadget_wakeup(&dwc->gadget))
			dev_err(mdwc->dev, "%s failed to take dwc out of L1\n",
					__func__);
		irq_stat &= ~PWR_EVNT_LPM_OUT_L1_MASK;
		irq_clear |= PWR_EVNT_LPM_OUT_L1_MASK;
	}

	/* Unhandled events */
	if (irq_stat)
		dev_dbg(mdwc->dev, "%s: unexpected PWR_EVNT, irq_stat=%X\n",
			__func__, irq_stat);

	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG, irq_clear);
}

static irqreturn_t msm_dwc3_pwr_irq_thread(int irq, void *_mdwc)
{
	struct dwc3_msm *mdwc = _mdwc;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (atomic_read(&dwc->in_lpm))
		dwc3_resume_work(&mdwc->resume_work.work);
	else
		dwc3_pwr_event_handler(mdwc);

	dbg_event(0xFF, "PIRQThread", atomic_read(&dwc->in_lpm));

	return IRQ_HANDLED;
}

static irqreturn_t msm_dwc3_pwr_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dwc->t_pwr_evt_irq = ktime_get();
	dev_dbg(mdwc->dev, "%s received\n", __func__);
	/*
	 * When in Low Power Mode, can't read PWR_EVNT_IRQ_STAT_REG to acertain
	 * which interrupts have been triggered, as the clocks are disabled.
	 * Resume controller by waking up pwr event irq thread.After re-enabling
	 * clocks, dwc3_msm_resume will call dwc3_pwr_event_handler to handle
	 * all other power events.
	 */
	if (atomic_read(&dwc->in_lpm)) {
		if (!mdwc->no_wakeup_src_in_hostmode || !mdwc->in_host_mode)
			pm_stay_awake(mdwc->dev);

		/* set this to call dwc3_msm_resume() */
		mdwc->resume_pending = true;
		return IRQ_WAKE_THREAD;
	}

	dwc3_pwr_event_handler(mdwc);
	return IRQ_HANDLED;
}

static int dwc3_msm_power_get_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = mdwc->voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = mdwc->current_max;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
		val->intval = mdwc->typec_current_max;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = mdwc->vbus_active;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = mdwc->online;
		break;
	case POWER_SUPPLY_PROP_REAL_TYPE:
		val->intval = mdwc->usb_supply_type;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = psy->type;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = mdwc->health_status;
		break;
	case POWER_SUPPLY_PROP_USB_OTG:
		val->intval = !mdwc->id_state;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int dwc3_msm_power_set_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  const union power_supply_propval *val)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	int ret;
	enum dwc3_id_state id;

	switch (psp) {
	case POWER_SUPPLY_PROP_USB_OTG:
		id = val->intval ? DWC3_ID_GROUND : DWC3_ID_FLOAT;
		if (mdwc->id_state == id)
			break;

		if (disable_host_mode && !id) {
			dev_dbg(mdwc->dev, "%s: Ignoring ID change event :%d\n",
						__func__, mdwc->id_state);
			break;
		}

		/* Let OTG know about ID detection */
		mdwc->id_state = id;
		dbg_event(0xFF, "id_state", mdwc->id_state);
		if (dwc->is_drd) {
			dbg_event(0xFF, "stayID", 0);
			pm_stay_awake(mdwc->dev);
			queue_delayed_work(mdwc->dwc3_resume_wq,
					&mdwc->resume_work, 0);
		}
		break;
	/* PMIC notification for DP_DM state */
	case POWER_SUPPLY_PROP_DP_DM:
		ret = usb_phy_change_dpdm(mdwc->hs_phy, val->intval);
		if (ret) {
			dev_dbg(mdwc->dev, "%s: error in phy dpdm update :%d\n",
								__func__, ret);
			return ret;
		}
		break;
	/* Process PMIC notification in PRESENT prop */
	case POWER_SUPPLY_PROP_PRESENT:
		dev_dbg(mdwc->dev, "%s: notify xceiv event with val:%d\n",
							__func__, val->intval);
		/*
		 * Now otg_sm_work() state machine waits for USB cable status.
		 * Hence here it makes sure that schedule resume work only if
		 * there is change in USB cable also if there is no USB cable
		 * notification.
		 */
		if (mdwc->otg_state == OTG_STATE_UNDEFINED) {
			mdwc->vbus_active = val->intval;
			queue_delayed_work(mdwc->dwc3_resume_wq,
					&mdwc->resume_work, 0);
			break;
		}

		if (mdwc->vbus_active == val->intval)
			break;

		mdwc->vbus_active = val->intval;
		if (dwc->is_drd && !mdwc->in_restart) {
			dbg_event(0xFF, "Q RW (vbus)", val->intval);
			dbg_event(0xFF, "stayVbus", 0);
			/* Ignore !vbus on stop_host */
			if (mdwc->vbus_active || test_bit(ID, &mdwc->inputs)) {
				pm_stay_awake(mdwc->dev);
				queue_delayed_work(mdwc->dwc3_resume_wq,
					&mdwc->resume_work, 0);
			}
		}
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		mdwc->online = val->intval;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		mdwc->voltage_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		mdwc->current_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
		mdwc->typec_current_max = val->intval;

		dbg_event(0xFF, "TYPE C CURMAX)", val->intval);
		/* Update chg_current as per type-c charger detection on VBUS */
		if (mdwc->chg_type != DWC3_INVALID_CHARGER) {
			dev_dbg(mdwc->dev, "update type-c charger\n");
			dwc3_msm_gadget_vbus_draw(mdwc,
						mdwc->bc1p2_current_max);
		}
		break;
	case POWER_SUPPLY_PROP_REAL_TYPE:
		mdwc->usb_supply_type = val->intval;
		/*
		 * Update TYPE property to DCP for HVDCP/HVDCP3 charger types
		 * so that they can be recongized as AC chargers by healthd.
		 * Don't report UNKNOWN charger type to prevent healthd missing
		 * detecting this power_supply status change.
		 */
		if (mdwc->usb_supply_type == POWER_SUPPLY_TYPE_USB_HVDCP_3
			|| mdwc->usb_supply_type == POWER_SUPPLY_TYPE_USB_HVDCP)
			psy->type = POWER_SUPPLY_TYPE_USB_DCP;
		else if (mdwc->usb_supply_type == POWER_SUPPLY_TYPE_UNKNOWN)
			psy->type = POWER_SUPPLY_TYPE_USB;
		else
			psy->type = mdwc->usb_supply_type;
		switch (mdwc->usb_supply_type) {
		case POWER_SUPPLY_TYPE_USB:
			mdwc->chg_type = DWC3_SDP_CHARGER;
			mdwc->voltage_max = MICRO_5V;
			break;
		case POWER_SUPPLY_TYPE_USB_DCP:
			mdwc->chg_type = DWC3_DCP_CHARGER;
			mdwc->voltage_max = MICRO_5V;
			break;
		case POWER_SUPPLY_TYPE_USB_HVDCP:
			mdwc->chg_type = DWC3_DCP_CHARGER;
			mdwc->voltage_max = MICRO_9V;
			dwc3_msm_gadget_vbus_draw(mdwc, hvdcp_max_current);
			break;
		case POWER_SUPPLY_TYPE_USB_CDP:
			mdwc->chg_type = DWC3_CDP_CHARGER;
			mdwc->voltage_max = MICRO_5V;
			break;
		case POWER_SUPPLY_TYPE_USB_ACA:
			mdwc->chg_type = DWC3_PROPRIETARY_CHARGER;
			break;
		default:
			mdwc->chg_type = DWC3_INVALID_CHARGER;
			break;
		}

		if (mdwc->chg_type != DWC3_INVALID_CHARGER)
			mdwc->chg_state = USB_CHG_STATE_DETECTED;

		dev_dbg(mdwc->dev, "%s: charger type: %s\n", __func__,
				chg_to_string(mdwc->chg_type));
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		mdwc->health_status = val->intval;
		break;
	default:
		return -EINVAL;
	}

	power_supply_changed(&mdwc->usb_psy);
	return 0;
}

static int
dwc3_msm_property_is_writeable(struct power_supply *psy,
				enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_USB_OTG:
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_MAX:
	case POWER_SUPPLY_PROP_REAL_TYPE:
		return 1;
	default:
		break;
	}

	return 0;
}


static char *dwc3_msm_pm_power_supplied_to[] = {
	"battery",
	"bms",
};

static enum power_supply_property dwc3_msm_pm_power_props_usb[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_USB_OTG,
	POWER_SUPPLY_PROP_REAL_TYPE,
};

static irqreturn_t dwc3_pmic_id_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	enum dwc3_id_state id;

	/* If we can't read ID line state for some reason, treat it as float */
	id = !!irq_read_line(irq);
	if (mdwc->id_state != id) {
		mdwc->id_state = id;
		dbg_event(0xFF, "stayIDIRQ", 0);
		pm_stay_awake(mdwc->dev);
		queue_delayed_work(mdwc->dwc3_resume_wq, &mdwc->resume_work, 0);
	}

	return IRQ_HANDLED;
}

static int dwc3_cpu_notifier_cb(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	uint32_t cpu = (uintptr_t)hcpu;
	struct dwc3_msm *mdwc =
			container_of(nfb, struct dwc3_msm, dwc3_cpu_notifier);

	if (cpu == cpu_to_affin && action == CPU_ONLINE) {
		pr_debug("%s: cpu online:%u irq:%d\n", __func__,
				cpu_to_affin, mdwc->irq_to_affin);
		irq_set_affinity(mdwc->irq_to_affin, get_cpu_mask(cpu));
	}

	return NOTIFY_OK;
}

static void dwc3_msm_otg_sm_work(struct work_struct *w);
static void dwc3_msm_otg_perf_vote_work(struct work_struct *w);

static int dwc3_msm_get_clk_gdsc(struct dwc3_msm *mdwc)
{
	int ret;

	mdwc->dwc3_gdsc = devm_regulator_get(mdwc->dev, "USB3_GDSC");
	if (IS_ERR(mdwc->dwc3_gdsc))
		mdwc->dwc3_gdsc = NULL;

	mdwc->xo_clk = devm_clk_get(mdwc->dev, "xo");
	if (IS_ERR(mdwc->xo_clk)) {
		dev_err(mdwc->dev, "%s unable to get TCXO buffer handle\n",
								__func__);
		ret = PTR_ERR(mdwc->xo_clk);
		return ret;
	}
	clk_set_rate(mdwc->xo_clk, 19200000);

	mdwc->iface_clk = devm_clk_get(mdwc->dev, "iface_clk");
	if (IS_ERR(mdwc->iface_clk)) {
		dev_err(mdwc->dev, "failed to get iface_clk\n");
		ret = PTR_ERR(mdwc->iface_clk);
		return ret;
	}

	/*
	 * DWC3 Core requires its CORE CLK (aka master / bus clk) to
	 * run at 125Mhz in SSUSB mode and >60MHZ for HSUSB mode.
	 * On newer platform it can run at 150MHz as well.
	 */
	mdwc->core_clk = devm_clk_get(mdwc->dev, "core_clk");
	if (IS_ERR(mdwc->core_clk)) {
		dev_err(mdwc->dev, "failed to get core_clk\n");
		ret = PTR_ERR(mdwc->core_clk);
		return ret;
	}

	if (!of_property_read_u32(mdwc->dev->of_node, "qcom,core-clk-rate",
				(u32 *)&mdwc->core_clk_rate)) {
		mdwc->core_clk_rate = clk_round_rate(mdwc->core_clk,
							mdwc->core_clk_rate);
	} else {
		/*
		 * Get Max supported clk frequency for USB Core CLK and request
		 * to set the same.
		 */
		mdwc->core_clk_rate = clk_round_rate(mdwc->core_clk, LONG_MAX);
	}

	if (IS_ERR_VALUE(mdwc->core_clk_rate)) {
		dev_err(mdwc->dev, "fail to get core clk max freq.\n");
	} else {
		dev_dbg(mdwc->dev, "USB core frequency = %ld\n",
							mdwc->core_clk_rate);
		ret = clk_set_rate(mdwc->core_clk, mdwc->core_clk_rate);
		if (ret)
			dev_err(mdwc->dev, "fail to set core_clk freq:%d\n",
									ret);
	}

	if (!of_property_read_u32(mdwc->dev->of_node, "qcom,core-clk-svs-rate",
				(u32 *)&mdwc->core_clk_svs_rate)) {
		mdwc->core_clk_svs_rate = clk_round_rate(mdwc->core_clk,
						mdwc->core_clk_svs_rate);
	} else {
		mdwc->core_clk_svs_rate = mdwc->core_clk_rate;
	}

	mdwc->sleep_clk = devm_clk_get(mdwc->dev, "sleep_clk");
	if (IS_ERR(mdwc->sleep_clk)) {
		dev_err(mdwc->dev, "failed to get sleep_clk\n");
		ret = PTR_ERR(mdwc->sleep_clk);
		return ret;
	}

	clk_set_rate(mdwc->sleep_clk, 32000);
	mdwc->utmi_clk_rate = 19200000;
	mdwc->utmi_clk = devm_clk_get(mdwc->dev, "utmi_clk");
	if (IS_ERR(mdwc->utmi_clk)) {
		dev_err(mdwc->dev, "failed to get utmi_clk\n");
		ret = PTR_ERR(mdwc->utmi_clk);
		return ret;
	}

	clk_set_rate(mdwc->utmi_clk, mdwc->utmi_clk_rate);
	mdwc->bus_aggr_clk = devm_clk_get(mdwc->dev, "bus_aggr_clk");
	if (IS_ERR(mdwc->bus_aggr_clk))
		mdwc->bus_aggr_clk = NULL;

	mdwc->cfg_ahb_clk = devm_clk_get(mdwc->dev, "cfg_ahb_clk");
	if (IS_ERR(mdwc->cfg_ahb_clk)) {
		dev_err(mdwc->dev, "failed to get cfg_ahb_clk\n");
		ret = PTR_ERR(mdwc->cfg_ahb_clk);
		return ret;
	}

	return 0;
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	if (mdwc->vbus_active)
		return snprintf(buf, PAGE_SIZE, "peripheral\n");
	if (mdwc->id_state == DWC3_ID_GROUND)
		return snprintf(buf, PAGE_SIZE, "host\n");

	return snprintf(buf, PAGE_SIZE, "none\n");
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	if (sysfs_streq(buf, "peripheral")) {
		mdwc->vbus_active = true;
		mdwc->id_state = DWC3_ID_FLOAT;
		mdwc->chg_type = DWC3_SDP_CHARGER;
	} else if (sysfs_streq(buf, "host")) {
		mdwc->vbus_active = false;
		mdwc->id_state = DWC3_ID_GROUND;
	} else {
		mdwc->vbus_active = false;
		mdwc->id_state = DWC3_ID_FLOAT;
	}

	dwc3_ext_event_notify(mdwc);

	return count;
}

static DEVICE_ATTR_RW(mode);

static ssize_t xhci_link_compliance_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	if (mdwc->xhci_ss_compliance_enable)
		return snprintf(buf, PAGE_SIZE, "y\n");
	else
		return snprintf(buf, PAGE_SIZE, "n\n");
}

static ssize_t xhci_link_compliance_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	bool value;
	int ret;

	ret = strtobool(buf, &value);
	if (!ret) {
		mdwc->xhci_ss_compliance_enable = value;
		return count;
	}

	return ret;
}

static DEVICE_ATTR_RW(xhci_link_compliance);

static int dwc3_msm_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node, *dwc3_node;
	struct device	*dev = &pdev->dev;
	struct dwc3_msm *mdwc;
	struct dwc3	*dwc;
	struct resource *res;
	void __iomem *tcsr;
	unsigned long flags;
	bool host_mode;
	int ret = 0;
	int ext_hub_reset_gpio;
	u32 val;

	mdwc = devm_kzalloc(&pdev->dev, sizeof(*mdwc), GFP_KERNEL);
	if (!mdwc)
		return -ENOMEM;

	mdwc->curr_mode = DWC3_PERF_INVALID;

	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))) {
		dev_err(&pdev->dev, "setting DMA mask to 64 failed.\n");
		if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))) {
			dev_err(&pdev->dev, "setting DMA mask to 32 failed.\n");
			return -EOPNOTSUPP;
		}
	}

	platform_set_drvdata(pdev, mdwc);
	mdwc->dev = &pdev->dev;

	INIT_LIST_HEAD(&mdwc->req_complete_list);
	INIT_DELAYED_WORK(&mdwc->resume_work, dwc3_resume_work);
	INIT_WORK(&mdwc->restart_usb_work, dwc3_restart_usb_work);
	INIT_DELAYED_WORK(&mdwc->sm_work, dwc3_msm_otg_sm_work);
	INIT_DELAYED_WORK(&mdwc->perf_vote_work, dwc3_msm_otg_perf_vote_work);
	/* wall charger in which D+/D- is disconnected
		would be recognized as usb cable, 5/5 */
	INIT_DELAYED_WORK(&mdwc->invalid_chg_work, dwc3_invalid_chg_work);
	/* end */


	mdwc->sm_usb_wq = create_freezable_workqueue("k_sm_usb");
	 if (!mdwc->sm_usb_wq) {
		pr_err("Failed to create workqueue for sm_usb");
		return -ENOMEM;
	}

	mdwc->dwc3_resume_wq = alloc_ordered_workqueue("dwc3_resume_wq", 0);
	if (!mdwc->dwc3_resume_wq) {
		pr_err("%s: Unable to create dwc3_resume_wq\n", __func__);
		return -ENOMEM;
	}

	/* Get all clks and gdsc reference */
	ret = dwc3_msm_get_clk_gdsc(mdwc);
	if (ret) {
		dev_err(&pdev->dev, "error getting clock or gdsc.\n");
		return ret;
	}

	mdwc->id_state = DWC3_ID_FLOAT;
	set_bit(ID, &mdwc->inputs);

	mdwc->charging_disabled = of_property_read_bool(node,
				"qcom,charging-disabled");

	ret = of_property_read_u32(node, "qcom,lpm-to-suspend-delay-ms",
				&mdwc->lpm_to_suspend_delay);
	if (ret) {
		dev_dbg(&pdev->dev, "setting lpm_to_suspend_delay to zero.\n");
		mdwc->lpm_to_suspend_delay = 0;
	}

	/*
	 * DWC3 has separate IRQ line for OTG events (ID/BSV) and for
	 * DP and DM linestate transitions during low power mode.
	 */
	mdwc->hs_phy_irq = platform_get_irq_byname(pdev, "hs_phy_irq");
	if (mdwc->hs_phy_irq < 0) {
		dev_err(&pdev->dev, "pget_irq for hs_phy_irq failed\n");
		ret = -EINVAL;
		goto err;
	} else {
		irq_set_status_flags(mdwc->hs_phy_irq, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(&pdev->dev, mdwc->hs_phy_irq,
					msm_dwc3_pwr_irq,
					msm_dwc3_pwr_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_EARLY_RESUME
					| IRQF_ONESHOT, "hs_phy_irq", mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq hs_phy_irq failed: %d\n",
					ret);
			goto err;
		}
	}

	mdwc->ss_phy_irq = platform_get_irq_byname(pdev, "ss_phy_irq");
	if (mdwc->ss_phy_irq < 0) {
		dev_dbg(&pdev->dev, "pget_irq for ss_phy_irq failed\n");
	} else {
		irq_set_status_flags(mdwc->ss_phy_irq, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(&pdev->dev, mdwc->ss_phy_irq,
					msm_dwc3_pwr_irq,
					msm_dwc3_pwr_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_EARLY_RESUME
					| IRQF_ONESHOT, "ss_phy_irq", mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq ss_phy_irq failed: %d\n",
					ret);
			goto err;
		}
	}

	/*
	 * Some platforms have a special interrupt line for indicating resume
	 * while in low power mode, when clocks are disabled.
	 */
	mdwc->pwr_event_irq = platform_get_irq_byname(pdev, "pwr_event_irq");
	if (mdwc->pwr_event_irq < 0) {
		dev_err(&pdev->dev, "pget_irq for pwr_event_irq failed\n");
		ret = -EINVAL;
		goto err;
	} else {
		/*
		 * enable pwr event irq early during PM resume to meet bus
		 * resume timeline from usb device
		 */
		ret = devm_request_threaded_irq(&pdev->dev, mdwc->pwr_event_irq,
					msm_dwc3_pwr_irq,
					msm_dwc3_pwr_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_EARLY_RESUME,
					"msm_dwc3", mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq pwr_event_irq failed: %d\n",
					ret);
			goto err;
		}
	}

	mdwc->pmic_id_irq = platform_get_irq_byname(pdev, "pmic_id_irq");
	if (mdwc->pmic_id_irq > 0) {
		irq_set_status_flags(mdwc->pmic_id_irq, IRQ_NOAUTOEN);
		ret = devm_request_irq(&pdev->dev,
				       mdwc->pmic_id_irq,
				       dwc3_pmic_id_irq,
				       IRQF_TRIGGER_RISING |
				       IRQF_TRIGGER_FALLING,
				       "dwc3_msm_pmic_id",
				       mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq IDINT failed\n");
			goto err;
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tcsr_base");
	if (!res) {
		dev_dbg(&pdev->dev, "missing TCSR memory resource\n");
	} else {
		tcsr = devm_ioremap_nocache(&pdev->dev, res->start,
			resource_size(res));
		if (IS_ERR_OR_NULL(tcsr)) {
			dev_dbg(&pdev->dev, "tcsr ioremap failed\n");
		} else {
			/* Enable USB3 on the primary USB port. */
			writel_relaxed(0x1, tcsr);
			/*
			 * Ensure that TCSR write is completed before
			 * USB registers initialization.
			 */
			mb();
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "core_base");
	if (!res) {
		dev_err(&pdev->dev, "missing memory base resource\n");
		ret = -ENODEV;
		goto err;
	}

	mdwc->base = devm_ioremap_nocache(&pdev->dev, res->start,
			resource_size(res));
	if (!mdwc->base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENODEV;
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"ahb2phy_base");
	if (res) {
		mdwc->ahb2phy_base = devm_ioremap_nocache(&pdev->dev,
					res->start, resource_size(res));
		if (IS_ERR_OR_NULL(mdwc->ahb2phy_base)) {
			dev_err(dev, "couldn't find ahb2phy_base addr.\n");
			mdwc->ahb2phy_base = NULL;
		} else {
			/*
			 * On some targets cfg_ahb_clk depends upon usb gdsc
			 * regulator. If cfg_ahb_clk is enabled without
			 * turning on usb gdsc regulator clk is stuck off.
			 */
			dwc3_msm_config_gdsc(mdwc, 1);
			clk_prepare_enable(mdwc->iface_clk);
			clk_prepare_enable(mdwc->core_clk);
			clk_prepare_enable(mdwc->cfg_ahb_clk);
			/* Configure AHB2PHY for one wait state read/write*/
			val = readl_relaxed(mdwc->ahb2phy_base +
					PERIPH_SS_AHB2PHY_TOP_CFG);
			if (val != ONE_READ_WRITE_WAIT) {
				writel_relaxed(ONE_READ_WRITE_WAIT,
					mdwc->ahb2phy_base +
					PERIPH_SS_AHB2PHY_TOP_CFG);
				/* complete above write before using USB PHY */
				mb();
			}
			clk_disable_unprepare(mdwc->cfg_ahb_clk);
			clk_disable_unprepare(mdwc->core_clk);
			clk_disable_unprepare(mdwc->iface_clk);
			dwc3_msm_config_gdsc(mdwc, 0);
		}
	}

	if (of_get_property(pdev->dev.of_node, "qcom,usb-dbm", NULL)) {
		mdwc->dbm = usb_get_dbm_by_phandle(&pdev->dev, "qcom,usb-dbm");
		if (IS_ERR(mdwc->dbm)) {
			dev_err(&pdev->dev, "unable to get dbm device\n");
			ret = -EPROBE_DEFER;
			goto err;
		}
		/*
		 * Add power event if the dbm indicates coming out of L1
		 * by interrupt
		 */
		if (dbm_l1_lpm_interrupt(mdwc->dbm)) {
			if (!mdwc->pwr_event_irq) {
				dev_err(&pdev->dev,
					"need pwr_event_irq exiting L1\n");
				ret = -EINVAL;
				goto err;
			}
		}
	}

	ext_hub_reset_gpio = of_get_named_gpio(node,
					"qcom,ext-hub-reset-gpio", 0);

	if (gpio_is_valid(ext_hub_reset_gpio)
		&& (!devm_gpio_request(&pdev->dev, ext_hub_reset_gpio,
			"qcom,ext-hub-reset-gpio"))) {
		/* reset external hub */
		gpio_direction_output(ext_hub_reset_gpio, 1);
		/*
		 * Hub reset should be asserted for minimum 5microsec
		 * before deasserting.
		 */
		usleep_range(5, 1000);
		gpio_direction_output(ext_hub_reset_gpio, 0);
	}

	if (of_property_read_u32(node, "qcom,pm-qos-latency",
				&mdwc->pm_qos_latency))
		dev_dbg(&pdev->dev, "Unable to read qcom,pm-qos-latency");

	if (of_property_read_u32(node, "qcom,dwc-usb3-msm-tx-fifo-size",
				 &mdwc->tx_fifo_size))
		dev_err(&pdev->dev,
			"unable to read platform data tx fifo size\n");

	mdwc->disable_host_mode_pm = of_property_read_bool(node,
				"qcom,disable-host-mode-pm");

	mdwc->no_wakeup_src_in_hostmode = of_property_read_bool(node,
				"qcom,no-wakeup-src-in-hostmode");
	if (mdwc->no_wakeup_src_in_hostmode)
		dev_dbg(&pdev->dev, "dwc3 host not using wakeup source\n");

	mdwc->psy_not_used = of_property_read_bool(node, "qcom,psy-not-used");

	mdwc->detect_dpdm_floating = of_property_read_bool(node,
				"qcom,detect-dpdm-floating");

	dwc3_set_notifier(&dwc3_msm_notify_event);

	/* Assumes dwc3 is the only DT child of dwc3-msm */
	dwc3_node = of_get_next_available_child(node, NULL);
	if (!dwc3_node) {
		dev_err(&pdev->dev, "failed to find dwc3 child\n");
		ret = -ENODEV;
		goto err;
	}

	host_mode = of_usb_get_dr_mode(dwc3_node) == USB_DR_MODE_HOST;
	/* usb_psy required only for vbus_notifications */
	if (!host_mode && !mdwc->psy_not_used) {
		mdwc->usb_psy.name = "usb";
		mdwc->usb_psy.type = POWER_SUPPLY_TYPE_USB;
		mdwc->usb_psy.supplied_to = dwc3_msm_pm_power_supplied_to;
		mdwc->usb_psy.num_supplicants = ARRAY_SIZE(
						dwc3_msm_pm_power_supplied_to);
		mdwc->usb_psy.properties = dwc3_msm_pm_power_props_usb;
		mdwc->usb_psy.num_properties =
					ARRAY_SIZE(dwc3_msm_pm_power_props_usb);
		mdwc->usb_psy.get_property = dwc3_msm_power_get_property_usb;
		mdwc->usb_psy.set_property = dwc3_msm_power_set_property_usb;
		mdwc->usb_psy.property_is_writeable =
				dwc3_msm_property_is_writeable;

		ret = power_supply_register(&pdev->dev, &mdwc->usb_psy);
		if (ret < 0) {
			dev_err(&pdev->dev,
					"%s:power_supply_register usb failed\n",
						__func__);
			goto err;
		}
	}

	ret = of_platform_populate(node, NULL, NULL, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to add create dwc3 core\n");
		of_node_put(dwc3_node);
		goto put_psupply;
	}

	mdwc->dwc3 = of_find_device_by_node(dwc3_node);
	of_node_put(dwc3_node);
	if (!mdwc->dwc3) {
		dev_err(&pdev->dev, "failed to get dwc3 platform device\n");
		goto put_dwc3;
	}

	mdwc->hs_phy = devm_usb_get_phy_by_phandle(&mdwc->dwc3->dev,
							"usb-phy", 0);
	if (IS_ERR(mdwc->hs_phy)) {
		dev_err(&pdev->dev, "unable to get hsphy device\n");
		ret = PTR_ERR(mdwc->hs_phy);
		goto put_dwc3;
	}
	mdwc->ss_phy = devm_usb_get_phy_by_phandle(&mdwc->dwc3->dev,
							"usb-phy", 1);
	if (IS_ERR(mdwc->ss_phy)) {
		dev_err(&pdev->dev, "unable to get ssphy device\n");
		ret = PTR_ERR(mdwc->ss_phy);
		goto put_dwc3;
	}

	mdwc->bus_scale_table = msm_bus_cl_get_pdata(pdev);
	if (!mdwc->bus_scale_table) {
		dev_err(&pdev->dev, "bus scaling is disabled\n");
	} else {
		mdwc->bus_perf_client =
			msm_bus_scale_register_client(mdwc->bus_scale_table);
		ret = msm_bus_scale_client_update_request(
						mdwc->bus_perf_client, 1);
		if (ret)
			dev_err(&pdev->dev, "Failed to vote for bus scaling\n");
	}

	dwc = platform_get_drvdata(mdwc->dwc3);
	if (!dwc) {
		dev_err(&pdev->dev, "Failed to get dwc3 device\n");
		goto put_dwc3;
	}

	mdwc->irq_to_affin = platform_get_irq(mdwc->dwc3, 0);
	mdwc->dwc3_cpu_notifier.notifier_call = dwc3_cpu_notifier_cb;

	if (cpu_to_affin)
		register_cpu_notifier(&mdwc->dwc3_cpu_notifier);

	device_init_wakeup(mdwc->dev, 1);
	pm_stay_awake(mdwc->dev);

	if (of_property_read_bool(node, "qcom,disable-dev-mode-pm"))
		pm_runtime_get_noresume(mdwc->dev);

	/* Update initial ID state */
	if (mdwc->pmic_id_irq) {
		enable_irq(mdwc->pmic_id_irq);
		local_irq_save(flags);
		mdwc->id_state = !!irq_read_line(mdwc->pmic_id_irq);
		if (mdwc->id_state == DWC3_ID_GROUND)
			dwc3_ext_event_notify(mdwc);
		local_irq_restore(flags);
		enable_irq_wake(mdwc->pmic_id_irq);
	}

	if (dwc->is_drd) {
		device_create_file(&pdev->dev, &dev_attr_mode);
		device_create_file(&pdev->dev, &dev_attr_xhci_link_compliance);
	}

	if (!dwc->is_drd && host_mode) {
		dev_dbg(&pdev->dev, "DWC3 in host only mode\n");
		mdwc->host_only_mode = true;
		mdwc->id_state = DWC3_ID_GROUND;
		device_create_file(&pdev->dev, &dev_attr_xhci_link_compliance);
		dwc3_ext_event_notify(mdwc);
	}

	/* If the controller is in DRD mode and USB power supply
	 * is not used make the default mode of contoller as HOST
	 * mode. User can change it later using sysfs
	 */
	if (dwc->is_drd && mdwc->psy_not_used) {
		dev_dbg(&pdev->dev, "DWC3 in host mode\n");
		mdwc->id_state = DWC3_ID_GROUND;
		dwc3_ext_event_notify(mdwc);
	}

	return 0;

put_dwc3:
	platform_device_put(mdwc->dwc3);
	if (mdwc->bus_perf_client)
		msm_bus_scale_unregister_client(mdwc->bus_perf_client);
put_psupply:
	if (mdwc->usb_psy.dev)
		power_supply_unregister(&mdwc->usb_psy);
err:
	return ret;
}

static int dwc3_msm_remove_children(struct device *dev, void *data)
{
	device_unregister(dev);
	return 0;
}

static int dwc3_msm_remove(struct platform_device *pdev)
{
	struct dwc3_msm	*mdwc = platform_get_drvdata(pdev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	int ret_pm;

	if (dwc->is_drd) {
		device_remove_file(&pdev->dev, &dev_attr_mode);
		device_remove_file(&pdev->dev, &dev_attr_xhci_link_compliance);
	} else if (mdwc->host_only_mode) {
		device_remove_file(&pdev->dev, &dev_attr_xhci_link_compliance);
	}

	if (cpu_to_affin)
		unregister_cpu_notifier(&mdwc->dwc3_cpu_notifier);

	/*
	 * In case of system suspend, pm_runtime_get_sync fails.
	 * Hence turn ON the clocks manually.
	 */
	ret_pm = pm_runtime_get_sync(mdwc->dev);
	dbg_event(0xFF, "Remov gsyn", ret_pm);
	if (ret_pm < 0) {
		dev_err(mdwc->dev,
			"pm_runtime_get_sync failed with %d\n", ret_pm);
		clk_prepare_enable(mdwc->utmi_clk);
		clk_prepare_enable(mdwc->core_clk);
		clk_prepare_enable(mdwc->iface_clk);
		clk_prepare_enable(mdwc->sleep_clk);
		if (mdwc->bus_aggr_clk)
			clk_prepare_enable(mdwc->bus_aggr_clk);
		clk_prepare_enable(mdwc->xo_clk);
	}

	cancel_delayed_work_sync(&mdwc->sm_work);

	if (mdwc->usb_psy.dev)
		power_supply_unregister(&mdwc->usb_psy);
	if (mdwc->hs_phy)
		mdwc->hs_phy->flags &= ~PHY_HOST_MODE;

	dbg_event(0xFF, "Remov put", 0);
	platform_device_put(mdwc->dwc3);
	device_for_each_child(&pdev->dev, NULL, dwc3_msm_remove_children);

	pm_runtime_disable(mdwc->dev);
	pm_runtime_barrier(mdwc->dev);
	pm_runtime_put_sync(mdwc->dev);
	pm_runtime_set_suspended(mdwc->dev);
	device_wakeup_disable(mdwc->dev);

	if (mdwc->bus_perf_client)
		msm_bus_scale_unregister_client(mdwc->bus_perf_client);

	if (!IS_ERR_OR_NULL(mdwc->vbus_reg))
		regulator_disable(mdwc->vbus_reg);

	disable_irq(mdwc->hs_phy_irq);
	if (mdwc->ss_phy_irq)
		disable_irq(mdwc->ss_phy_irq);
	disable_irq(mdwc->pwr_event_irq);

	clk_disable_unprepare(mdwc->utmi_clk);
	clk_set_rate(mdwc->core_clk, 19200000);
	clk_disable_unprepare(mdwc->core_clk);
	clk_disable_unprepare(mdwc->iface_clk);
	clk_disable_unprepare(mdwc->sleep_clk);
	clk_disable_unprepare(mdwc->xo_clk);
	clk_put(mdwc->xo_clk);

	dwc3_msm_config_gdsc(mdwc, 0);

	return 0;
}

#define VBUS_REG_CHECK_DELAY	(msecs_to_jiffies(1000))

static void dwc3_pm_qos_update_latency(struct dwc3_msm *mdwc, s32 latency)
{
	static s32 last_vote;
#ifdef CONFIG_SMP
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
#endif

	if (latency == last_vote || !mdwc->pm_qos_latency)
		return;

	pr_debug("%s: latency updated to: %d\n", __func__, latency);
	if (latency == PM_QOS_DEFAULT_VALUE) {
		pm_qos_update_request(&mdwc->pm_qos_req_dma,
					PM_QOS_DEFAULT_VALUE);
		last_vote = latency;
		pm_qos_remove_request(&mdwc->pm_qos_req_dma);
	} else {
		if (!pm_qos_request_active(&mdwc->pm_qos_req_dma)) {
			/*
			 * The default request type PM_QOS_REQ_ALL_CORES is
			 * applicable to all CPU cores that are online and
			 * would have a power impact when there are more
			 * number of CPUs. PM_QOS_REQ_AFFINE_IRQ request
			 * type shall update/apply the vote only to that CPU to
			 * which IRQ's affinity is set to.
			 */
#ifdef CONFIG_SMP
			mdwc->pm_qos_req_dma.type = PM_QOS_REQ_AFFINE_IRQ;
			mdwc->pm_qos_req_dma.irq = dwc->irq;
#endif
			pm_qos_add_request(&mdwc->pm_qos_req_dma,
				PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
		}
		pm_qos_update_request(&mdwc->pm_qos_req_dma, latency);
		last_vote = latency;
	}
}

static void dwc3_msm_perf_vote_update(struct dwc3_msm *mdwc,
					enum dwc3_perf_mode mode)
{
	int latency = mdwc->pm_qos_latency, ret;
	long core_clk_rate = mdwc->core_clk_rate;

	if (mdwc->curr_mode == mode)
		return;

	mdwc->curr_mode = mode;
	switch (mode) {
	case DWC3_PERF_NOM:
		latency = mdwc->pm_qos_latency;
		mdwc->bus_vote = 2;
		core_clk_rate = mdwc->core_clk_rate;
		break;
	case DWC3_PERF_SVS:
		latency = PM_QOS_DEFAULT_VALUE;
		mdwc->bus_vote = 1;
		core_clk_rate = mdwc->core_clk_svs_rate;
		break;
	case DWC3_PERF_OFF:
		latency = PM_QOS_DEFAULT_VALUE;
		mdwc->bus_vote = 0;
		break;
	default:
		dev_dbg(mdwc->dev, "perf mode incorrect hit!\n");
		break;
	}

	/* update dma latency */
	dwc3_pm_qos_update_latency(mdwc, latency);

	 /* Vote for bus bandwidth */
	dev_dbg(mdwc->dev, "bus vote for index %d\n", mdwc->bus_vote);
	ret = msm_bus_scale_client_update_request(mdwc->bus_perf_client,
							mdwc->bus_vote);
	if (ret)
		dev_err(mdwc->dev, "Fail to vote for bus. error code: %d\n",
			ret);

	/* Set USB clock */
	if (mode != DWC3_PERF_OFF)
		clk_set_rate(mdwc->core_clk, core_clk_rate);
}

/*
 * Execute this work periodically, check USB interrupt load to set NOM/SVS mode,
 * set pm_qos latency, clockrate and bus bandwidth accordingly.
 */
static void dwc3_msm_otg_perf_vote_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
					perf_vote_work.work);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	static unsigned long last_irq_count;
	enum dwc3_perf_mode mode = DWC3_PERF_INVALID;

	if (dwc->irq_cnt - last_irq_count >= PM_QOS_THRESHOLD_NOM)
		mode = DWC3_PERF_NOM;
	else
		mode = DWC3_PERF_SVS;

	dev_dbg(mdwc->dev, "irq_cnt: %lu, last_irq_count: %lu, mode: %d\n",
		dwc->irq_cnt, last_irq_count, (int) mode);
	last_irq_count = dwc->irq_cnt;

	dwc3_msm_perf_vote_update(mdwc, mode);
	schedule_delayed_work(&mdwc->perf_vote_work,
				msecs_to_jiffies(1000 * PM_QOS_SAMPLE_SEC));
}

/**
 * dwc3_otg_start_host -  helper function for starting/stoping the host controller driver.
 *
 * @mdwc: Pointer to the dwc3_msm structure.
 * @on: start / stop the host controller driver.
 *
 * Returns 0 on success otherwise negative errno.
 */
static int dwc3_otg_start_host(struct dwc3_msm *mdwc, int on)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	int ret = 0;

	if (!dwc->xhci)
		return -EINVAL;

	/*
	 * The vbus_reg pointer could have multiple values
	 * NULL: regulator_get() hasn't been called, or was previously deferred
	 * IS_ERR: regulator could not be obtained, so skip using it
	 * Valid pointer otherwise
	 */
	if (!mdwc->vbus_reg) {
		mdwc->vbus_reg = devm_regulator_get_optional(mdwc->dev,
					"vbus_dwc3");
		if (IS_ERR(mdwc->vbus_reg) &&
				PTR_ERR(mdwc->vbus_reg) == -EPROBE_DEFER) {
			/* regulators may not be ready, so retry again later */
			mdwc->vbus_reg = NULL;
			return -EPROBE_DEFER;
		}
	}

	if (on) {
		dev_dbg(mdwc->dev, "%s: turn on host\n", __func__);

		pm_runtime_get_sync(mdwc->dev);
		dbg_event(0xFF, "StrtHost gync",
			atomic_read(&mdwc->dev->power.usage_count));
		mdwc->hs_phy->flags |= PHY_HOST_MODE;
		mdwc->ss_phy->flags |= PHY_HOST_MODE;
		usb_phy_notify_connect(mdwc->hs_phy, USB_SPEED_HIGH);
		if (!IS_ERR(mdwc->vbus_reg))
			ret = regulator_enable(mdwc->vbus_reg);
		if (ret) {
			dev_err(mdwc->dev, "unable to enable vbus_reg\n");
			mdwc->hs_phy->flags &= ~PHY_HOST_MODE;
			mdwc->ss_phy->flags &= ~PHY_HOST_MODE;
			pm_runtime_put_sync(mdwc->dev);
			dbg_event(0xFF, "vregerr psync",
				atomic_read(&mdwc->dev->power.usage_count));
			return ret;
		}

		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_HOST);

		mdwc->usbdev_nb.notifier_call = msm_dwc3_usbdev_notify;
		usb_register_atomic_notify(&mdwc->usbdev_nb);
		/*
		 * FIXME If micro A cable is disconnected during system suspend,
		 * xhci platform device will be removed before runtime pm is
		 * enabled for xhci device. Due to this, disable_depth becomes
		 * greater than one and runtimepm is not enabled for next microA
		 * connect. Fix this by calling pm_runtime_init for xhci device.
		 */
		pm_runtime_init(&dwc->xhci->dev);
		ret = platform_device_add(dwc->xhci);
		if (ret) {
			dev_err(mdwc->dev,
				"%s: failed to add XHCI pdev ret=%d\n",
				__func__, ret);
			if (!IS_ERR(mdwc->vbus_reg))
				regulator_disable(mdwc->vbus_reg);
			mdwc->hs_phy->flags &= ~PHY_HOST_MODE;
			mdwc->ss_phy->flags &= ~PHY_HOST_MODE;
			pm_runtime_put_sync(mdwc->dev);
			dbg_event(0xFF, "pdeverr psync",
				atomic_read(&mdwc->dev->power.usage_count));
			return ret;
		}

		/*
		 * If the Compliance Transition Capability(CTC) flag of
		 * HCCPARAMS2 register is set and xhci_link_compliance sysfs
		 * param has been enabled by the user for the SuperSpeed host
		 * controller, then write 10 (Link in Compliance Mode State)
		 * onto the Port Link State(PLS) field of the PORTSC register
		 * for 3.0 host controller which is at an offset of USB3_PORTSC
		 * + 0x10 from the DWC3 base address. Also, disable the runtime
		 * PM of 3.0 root hub (root hub of shared_hcd of xhci device)
		 */
		if (HCC_CTC(dwc3_msm_read_reg(mdwc->base, USB3_HCCPARAMS2))
				&& mdwc->xhci_ss_compliance_enable
				&& dwc->maximum_speed == USB_SPEED_SUPER) {
			dwc3_msm_write_reg(mdwc->base, USB3_PORTSC + 0x10,
					0x10340);
			pm_runtime_disable(&hcd_to_xhci(platform_get_drvdata(
				dwc->xhci))->shared_hcd->self.root_hub->dev);
		}

		/*
		 * In some cases it is observed that USB PHY is not going into
		 * suspend with host mode suspend functionality. Hence disable
		 * XHCI's runtime PM here if disable_host_mode_pm is set.
		 */
		if (mdwc->disable_host_mode_pm)
			pm_runtime_disable(&dwc->xhci->dev);

		mdwc->in_host_mode = true;
		dwc3_gadget_usb3_phy_suspend(dwc, true);

		/* xHCI should have incremented child count as necessary */
		dbg_event(0xFF, "StrtHost psync",
			atomic_read(&mdwc->dev->power.usage_count));
		pm_runtime_mark_last_busy(mdwc->dev);
		pm_runtime_put_sync_autosuspend(mdwc->dev);
		dwc3_msm_perf_vote_update(mdwc, DWC3_PERF_NOM);
		schedule_delayed_work(&mdwc->perf_vote_work,
			msecs_to_jiffies(1000 * PM_QOS_SAMPLE_SEC));
	} else {
		dev_dbg(mdwc->dev, "%s: turn off host\n", __func__);

		usb_unregister_atomic_notify(&mdwc->usbdev_nb);
		if (!IS_ERR(mdwc->vbus_reg))
			ret = regulator_disable(mdwc->vbus_reg);
		if (ret) {
			dev_err(mdwc->dev, "unable to disable vbus_reg\n");
			return ret;
		}

		cancel_delayed_work_sync(&mdwc->perf_vote_work);
		dwc3_msm_perf_vote_update(mdwc, DWC3_PERF_OFF);
		pm_runtime_get_sync(mdwc->dev);
		dbg_event(0xFF, "StopHost gsync",
			atomic_read(&mdwc->dev->power.usage_count));
		usb_phy_notify_disconnect(mdwc->hs_phy, USB_SPEED_HIGH);
		mdwc->hs_phy->flags &= ~PHY_HOST_MODE;
		mdwc->ss_phy->flags &= ~PHY_HOST_MODE;
		platform_device_del(dwc->xhci);

		/*
		 * Perform USB hardware RESET (both core reset and DBM reset)
		 * when moving from host to peripheral. This is required for
		 * peripheral mode to work.
		 */
		dwc3_msm_block_reset(mdwc, true);

		dwc3_gadget_usb3_phy_suspend(dwc, false);
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);

		mdwc->in_host_mode = false;

		/* re-init core and OTG registers as block reset clears these */
		if (!mdwc->host_only_mode)
			dwc3_post_host_reset_core_init(dwc);

		pm_runtime_mark_last_busy(mdwc->dev);
		pm_runtime_put_sync_autosuspend(mdwc->dev);

		dbg_event(0xFF, "StopHost psync",
			atomic_read(&mdwc->dev->power.usage_count));
	}

	return 0;
}

static void dwc3_override_vbus_status(struct dwc3_msm *mdwc, bool vbus_present)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	/* Update OTG VBUS Valid from HSPHY to controller */
	dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
		vbus_present ? UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL :
		UTMI_OTG_VBUS_VALID,
		vbus_present ? UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL : 0);

	/* Update only if Super Speed is supported */
	if (dwc->maximum_speed == USB_SPEED_SUPER) {
		/* Update VBUS Valid from SSPHY to controller */
		dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG,
			LANE0_PWR_PRESENT,
			vbus_present ? LANE0_PWR_PRESENT : 0);
	}
}

/**
 * dwc3_otg_start_peripheral -  bind/unbind the peripheral controller.
 *
 * @mdwc: Pointer to the dwc3_msm structure.
 * @on:   Turn ON/OFF the gadget.
 *
 * Returns 0 on success otherwise negative errno.
 */
static int dwc3_otg_start_peripheral(struct dwc3_msm *mdwc, int on)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	pm_runtime_get_sync(mdwc->dev);
	dbg_event(0xFF, "StrtGdgt gsync",
		atomic_read(&mdwc->dev->power.usage_count));

	if (on) {
		dev_dbg(mdwc->dev, "%s: turn on gadget %s\n",
					__func__, dwc->gadget.name);

		dwc3_override_vbus_status(mdwc, true);
		usb_phy_notify_connect(mdwc->hs_phy, USB_SPEED_HIGH);
		usb_phy_notify_connect(mdwc->ss_phy, USB_SPEED_SUPER);

		/* Core reset is not required during start peripheral. Only
		 * DBM reset is required, hence perform only DBM reset here */
		dwc3_msm_block_reset(mdwc, false);

		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);
		usb_gadget_vbus_connect(&dwc->gadget);

		dwc3_msm_perf_vote_update(mdwc, DWC3_PERF_NOM);
	} else {
		dev_dbg(mdwc->dev, "%s: turn off gadget %s\n",
					__func__, dwc->gadget.name);
		usb_gadget_vbus_disconnect(&dwc->gadget);
		usb_phy_notify_disconnect(mdwc->hs_phy, USB_SPEED_HIGH);
		usb_phy_notify_disconnect(mdwc->ss_phy, USB_SPEED_SUPER);
		dwc3_override_vbus_status(mdwc, false);
		dwc3_gadget_usb3_phy_suspend(dwc, false);
		cancel_delayed_work_sync(&mdwc->perf_vote_work);
		dwc3_msm_perf_vote_update(mdwc, DWC3_PERF_OFF);
	}

	pm_runtime_put_sync(mdwc->dev);
	dbg_event(0xFF, "StopGdgt psync",
		atomic_read(&mdwc->dev->power.usage_count));

	return 0;
}

static int dwc3_msm_gadget_vbus_draw(struct dwc3_msm *mdwc, unsigned mA)
{
	enum power_supply_type power_supply_type;
	union power_supply_propval propval;

	if (mdwc->charging_disabled)
		return 0;

	if (mdwc->chg_type != DWC3_INVALID_CHARGER) {
		dev_dbg(mdwc->dev,
			"SKIP setting power supply type again,chg_type = %d\n",
			mdwc->chg_type);
		goto skip_psy_type;
	}

	dev_dbg(mdwc->dev, "Requested curr from USB = %u, max-type-c:%u\n",
					mA, mdwc->typec_current_max);

	if (mdwc->chg_type == DWC3_SDP_CHARGER)
		power_supply_type = POWER_SUPPLY_TYPE_USB;
	else if (mdwc->chg_type == DWC3_CDP_CHARGER)
		power_supply_type = POWER_SUPPLY_TYPE_USB_CDP;
	else if (mdwc->chg_type == DWC3_DCP_CHARGER ||
			mdwc->chg_type == DWC3_PROPRIETARY_CHARGER)
		power_supply_type = POWER_SUPPLY_TYPE_USB_DCP;
	else
		power_supply_type = POWER_SUPPLY_TYPE_UNKNOWN;

	propval.intval = power_supply_type;
	mdwc->usb_psy.set_property(&mdwc->usb_psy,
			POWER_SUPPLY_PROP_REAL_TYPE, &propval);

skip_psy_type:

	if (mdwc->chg_type == DWC3_CDP_CHARGER)
		mA = DWC3_IDEV_CHG_MAX;

	/* Save bc1.2 max_curr if type-c charger later moves to diff mode */
	mdwc->bc1p2_current_max = mA;

	/* Override mA if type-c charger used (use hvdcp/bc1.2 if it is 500) */
	if (mdwc->typec_current_max > 500 && mA < mdwc->typec_current_max)
		mA = mdwc->typec_current_max;

	if (mdwc->max_power == mA)
		return 0;

	dev_info(mdwc->dev, "Avail curr from USB = %u\n", mA);

	if (mdwc->max_power <= 2 && mA > 2) {
		/* Enable Charging */
		if (power_supply_set_online(&mdwc->usb_psy, true))
			goto psy_error;
		if (power_supply_set_current_limit(&mdwc->usb_psy, 1000*mA))
			goto psy_error;
	} else if (mdwc->max_power > 0 && (mA == 0 || mA == 2)) {
		/* Disable charging */
		if (power_supply_set_online(&mdwc->usb_psy, false))
			goto psy_error;
	} else {
		/* Enable charging */
		if (power_supply_set_online(&mdwc->usb_psy, true))
			goto psy_error;
	}

	/* Set max current limit in uA */
	if (power_supply_set_current_limit(&mdwc->usb_psy, 1000*mA))
		goto psy_error;

	power_supply_changed(&mdwc->usb_psy);
	mdwc->max_power = mA;
	return 0;

psy_error:
	dev_dbg(mdwc->dev, "power supply error when setting property\n");
	return -ENXIO;
}

static void dwc3_check_float_lines(struct dwc3_msm *mdwc)
{
	int dpdm;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(mdwc->dev, "%s: Check linestate\n", __func__);
	dwc3_msm_gadget_vbus_draw(mdwc, 0);

	/* Get linestate with Idp_src enabled */
	dpdm = usb_phy_dpdm_with_idp_src(mdwc->hs_phy);
	if (dpdm == 0x2) {
		/* DP is HIGH = lines are floating */
		mdwc->chg_type = DWC3_PROPRIETARY_CHARGER;
		mdwc->otg_state = OTG_STATE_B_IDLE;
		pm_runtime_put_sync(mdwc->dev);
		dbg_event(0xFF, "FLT psync",
				atomic_read(&mdwc->dev->power.usage_count));
	} else if (dpdm) {
		dev_dbg(mdwc->dev, "%s:invalid linestate:%x\n", __func__, dpdm);
	}
}

static void dwc3_initialize(struct dwc3_msm *mdwc)
{
	u32 tmp;
	int ret;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dbg_event(0xFF, "Initialized Start",
			atomic_read(&mdwc->dev->power.usage_count));

	/* enable USB GDSC */
	dwc3_msm_config_gdsc(mdwc, 1);

	/* enable all clocks */
	ret = clk_prepare_enable(mdwc->xo_clk);
	clk_prepare_enable(mdwc->iface_clk);
	clk_prepare_enable(mdwc->core_clk);
	clk_prepare_enable(mdwc->sleep_clk);
	clk_prepare_enable(mdwc->utmi_clk);
	if (mdwc->bus_aggr_clk)
		clk_prepare_enable(mdwc->bus_aggr_clk);

	/* Perform controller GCC reset */
	dwc3_msm_link_clk_reset(mdwc, 1);
	msleep(20);
	dwc3_msm_link_clk_reset(mdwc, 0);

	/*
	 * Get core configuration and initialized
	 * Set Event buffers
	 * Reset both USB PHYs and initialized
	 */
	dwc3_core_pre_init(dwc);

	/* Get initial P3 status and enable IN_P3 event */
	tmp = dwc3_msm_read_reg_field(mdwc->base,
		DWC3_GDBGLTSSM, DWC3_GDBGLTSSM_LINKSTATE_MASK);
	atomic_set(&mdwc->in_p3, tmp == DWC3_LINK_STATE_U3);
	dwc3_msm_write_reg_field(mdwc->base, PWR_EVNT_IRQ_MASK_REG,
			PWR_EVNT_POWERDOWN_IN_P3_MASK, 1);
}

/**
 * dwc3_msm_otg_sm_work - workqueue function.
 *
 * @w: Pointer to the dwc3 otg workqueue
 *
 * NOTE: After any change in otg_state, we must reschdule the state machine.
 */
static void dwc3_msm_otg_sm_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, sm_work.work);
	struct dwc3 *dwc = NULL;
	bool work = 0;
	int ret = 0;
	unsigned long delay = 0;
	const char *state;

	if (mdwc->dwc3)
		dwc = platform_get_drvdata(mdwc->dwc3);

	if (!dwc) {
		dev_err(mdwc->dev, "dwc is NULL.\n");
		return;
	}

	state = usb_otg_state_string(mdwc->otg_state);
	dev_dbg(mdwc->dev, "%s state\n", state);
	dbg_event(0xFF, state, 0);

	/* Check OTG state */
	switch (mdwc->otg_state) {
	case OTG_STATE_UNDEFINED:
		if (!test_bit(ID, &mdwc->inputs)) {
			dbg_event(0xFF, "undef_host", 0);
			atomic_set(&dwc->in_lpm, 0);
			pm_runtime_set_active(mdwc->dev);
			pm_runtime_enable(mdwc->dev);
			pm_runtime_get_noresume(mdwc->dev);
			dwc3_initialize(mdwc);
			mdwc->otg_state = OTG_STATE_A_HOST;
			ret = dwc3_otg_start_host(mdwc, 1);
			pm_runtime_put_noidle(mdwc->dev);
			/*
			 * call pm_relax to match with pm_stay_wake
			 * in probe for host mode.
			 */
			if (ret != -EPROBE_DEFER) {
				if (mdwc->no_wakeup_src_in_hostmode
						&& mdwc->in_host_mode)
					pm_relax(mdwc->dev);
				return;
			}
			/*
			 * regulator_get for VBUS may fail with -EPROBE_DEFER.
			 * Set the state as A_IDLE which will re-schedule
			 * sm_work after 1sec and call start_host again.
			 */
			mdwc->otg_state = OTG_STATE_A_IDLE;
			work = 1;
			break;
		}

		if (test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "b_sess_vld\n");
			dbg_event(0xFF, "undef_b_sess_vld", 0);
			switch (mdwc->chg_type) {
			case DWC3_DCP_CHARGER:
			case DWC3_PROPRIETARY_CHARGER:
				dev_dbg(mdwc->dev, "DCP charger\n");
				dwc3_msm_gadget_vbus_draw(mdwc,
						dcp_max_current);
				atomic_set(&dwc->in_lpm, 1);
				dbg_event(0xFF, "RelaxDCP", 0);
				pm_relax(mdwc->dev);
				break;
			case DWC3_CDP_CHARGER:
			case DWC3_SDP_CHARGER:
				atomic_set(&dwc->in_lpm, 0);
				pm_runtime_set_active(mdwc->dev);
				pm_runtime_enable(mdwc->dev);
				pm_runtime_get_noresume(mdwc->dev);
				dwc3_initialize(mdwc);
				/* check dp/dm for SDP & runtime_put if !SDP */
				if (mdwc->detect_dpdm_floating &&
					mdwc->chg_type == DWC3_SDP_CHARGER) {
					dwc3_check_float_lines(mdwc);
					if (mdwc->chg_type != DWC3_SDP_CHARGER)
						break;
				}
				/* wall charger in which D+/D- is disconnected
				would be recognized as an usb cable, 4.1/5 */
				schedule_delayed_work(&mdwc->invalid_chg_work, 5*HZ);
				/* end */
				dwc3_otg_start_peripheral(mdwc, 1);
				mdwc->otg_state = OTG_STATE_B_PERIPHERAL;
				dbg_event(0xFF, "Undef SDP",
					atomic_read(
					&mdwc->dev->power.usage_count));
				break;
			default:
				WARN_ON(1);
				break;
			}
		}

		if (!test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dbg_event(0xFF, "undef_!b_sess_vld", 0);
			atomic_set(&dwc->in_lpm, 0);
			pm_runtime_set_active(mdwc->dev);
			pm_runtime_enable(mdwc->dev);
			pm_runtime_get_noresume(mdwc->dev);
			dwc3_initialize(mdwc);
			pm_runtime_put_sync(mdwc->dev);
			dbg_event(0xFF, "Undef NoUSB",
				atomic_read(&mdwc->dev->power.usage_count));
			mdwc->otg_state = OTG_STATE_B_IDLE;
		}
		break;

	case OTG_STATE_B_IDLE:
		if (!test_bit(ID, &mdwc->inputs)) {
			dbg_event(0xFF, "!id", 0);
			mdwc->otg_state = OTG_STATE_A_IDLE;
			work = 1;
			mdwc->chg_type = DWC3_INVALID_CHARGER;
		} else if (test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dbg_event(0xFF, "b_sess_vld", 0);
			switch (mdwc->chg_type) {
			case DWC3_DCP_CHARGER:
			case DWC3_PROPRIETARY_CHARGER:
				dbg_event(0xFF, "DCPCharger", 0);
				dwc3_msm_gadget_vbus_draw(mdwc,
						dcp_max_current);
				dbg_event(0xFF, "RelDCPBIDLE", 0);
				pm_relax(mdwc->dev);
				break;
			case DWC3_CDP_CHARGER:
				dbg_event(0xFF, "CDPCharger", 0);
				dwc3_msm_gadget_vbus_draw(mdwc,
						DWC3_IDEV_CHG_MAX);
				/* fall through */
			case DWC3_SDP_CHARGER:
				dbg_event(0xFF, "SDPCharger", 0);
				/*
				 * Increment pm usage count upon cable
				 * connect. Count is decremented in
				 * OTG_STATE_B_PERIPHERAL state on cable
				 * disconnect or in bus suspend.
				 */
				pm_runtime_get_sync(mdwc->dev);
				dbg_event(0xFF, "CHG gsync",
					atomic_read(
						&mdwc->dev->power.usage_count));
				/* check dp/dm for SDP & runtime_put if !SDP */
				if (mdwc->detect_dpdm_floating &&
				    mdwc->chg_type == DWC3_SDP_CHARGER) {
					dwc3_check_float_lines(mdwc);
					if (mdwc->chg_type != DWC3_SDP_CHARGER)
						break;
				}
				dwc3_otg_start_peripheral(mdwc, 1);
				mdwc->otg_state = OTG_STATE_B_PERIPHERAL;
				work = 1;
				/* wall charger in which D+/D- is disconnected
				would be recognized as an usb cable, 4.2/5 */
				schedule_delayed_work(&mdwc->invalid_chg_work, 5*HZ);
				/* end */
				break;
			/* fall through */
			default:
				break;
			}
		} else {
			mdwc->typec_current_max = 0;
			/* wall charger in which D+/D- is disconnected
				would be recognized as an usb cable, 5/5 */
			dev_dbg(mdwc->dev,  "cancel invalid_chg_work\n");
			cancel_delayed_work_sync(&mdwc->invalid_chg_work);
			skip_invalid_chg_work = 0;
			/* end */
			dwc3_msm_gadget_vbus_draw(mdwc, 0);
			dev_dbg(mdwc->dev, "No device, allowing suspend\n");
			dbg_event(0xFF, "RelNodev", 0);
			pm_relax(mdwc->dev);
		}
		break;

	case OTG_STATE_B_PERIPHERAL:
		if (!test_bit(B_SESS_VLD, &mdwc->inputs) ||
				!test_bit(ID, &mdwc->inputs)) {
			dbg_event(0xFF, "!id || !bsv", 0);
			mdwc->otg_state = OTG_STATE_B_IDLE;
			dwc3_otg_start_peripheral(mdwc, 0);
			/*
			 * Decrement pm usage count upon cable disconnect
			 * which was incremented upon cable connect in
			 * OTG_STATE_B_IDLE state
			 */
			pm_runtime_put_sync(mdwc->dev);
			dbg_event(0xFF, "BPER psync",
				atomic_read(&mdwc->dev->power.usage_count));
			mdwc->chg_type = DWC3_INVALID_CHARGER;
			work = 1;
		} else if (test_bit(B_SUSPEND, &mdwc->inputs) &&
			test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dbg_event(0xFF, "BPER susp", 0);
			mdwc->otg_state = OTG_STATE_B_SUSPEND;
			/*
			 * Decrement pm usage count upon bus suspend.
			 * Count was incremented either upon cable
			 * connect in OTG_STATE_B_IDLE or host
			 * initiated resume after bus suspend in
			 * OTG_STATE_B_SUSPEND state
			 */
			pm_runtime_mark_last_busy(mdwc->dev);
			pm_runtime_put_autosuspend(mdwc->dev);
			dbg_event(0xFF, "SUSP put",
				atomic_read(&mdwc->dev->power.usage_count));
		}
		break;

	case OTG_STATE_B_SUSPEND:
		if (!test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dbg_event(0xFF, "BSUSP: !bsv", 0);
			mdwc->otg_state = OTG_STATE_B_IDLE;
			dwc3_otg_start_peripheral(mdwc, 0);
		} else if (!test_bit(B_SUSPEND, &mdwc->inputs)) {
			dbg_event(0xFF, "BSUSP: !susp", 0);
			mdwc->otg_state = OTG_STATE_B_PERIPHERAL;
			/*
			 * Increment pm usage count upon host
			 * initiated resume. Count was decremented
			 * upon bus suspend in
			 * OTG_STATE_B_PERIPHERAL state.
			 */
			pm_runtime_get_sync(mdwc->dev);
			dbg_event(0xFF, "SUSP gsync",
				atomic_read(&mdwc->dev->power.usage_count));
		} else {
			/*
			 * Release PM Wakelock if PM resume had happened from
			 * peripheral mode bus suspend case.
			 */
			pm_relax(mdwc->dev);
		}
		break;

	case OTG_STATE_A_IDLE:
		/* Switch to A-Device*/
		if (test_bit(ID, &mdwc->inputs)) {
			dbg_event(0xFF, "id", 0);
			mdwc->otg_state = OTG_STATE_B_IDLE;
			mdwc->vbus_retry_count = 0;
			work = 1;
		} else {
			mdwc->otg_state = OTG_STATE_A_HOST;
			ret = dwc3_otg_start_host(mdwc, 1);
			if ((ret == -EPROBE_DEFER) &&
						mdwc->vbus_retry_count < 3) {
				/*
				 * Get regulator failed as regulator driver is
				 * not up yet. Will try to start host after 1sec
				 */
				mdwc->otg_state = OTG_STATE_A_IDLE;
				dev_dbg(mdwc->dev, "Unable to get vbus regulator. Retrying...\n");
				delay = VBUS_REG_CHECK_DELAY;
				work = 1;
				mdwc->vbus_retry_count++;
			} else if (ret) {
				dev_err(mdwc->dev, "unable to start host\n");
				mdwc->otg_state = OTG_STATE_A_IDLE;
				goto ret;
			}
			if (mdwc->no_wakeup_src_in_hostmode &&
						mdwc->in_host_mode)
				pm_wakeup_event(mdwc->dev,
						DWC3_WAKEUP_SRC_TIMEOUT);
		}
		break;

	case OTG_STATE_A_HOST:
		if (test_bit(ID, &mdwc->inputs) || mdwc->hc_died
				|| mdwc->stop_host) {
			dbg_event(0xFF, "id || hc_died || stop_host", 0);
			dev_dbg(mdwc->dev, "%s state id || hc_died\n", state);
			dwc3_otg_start_host(mdwc, 0);
			mdwc->otg_state = OTG_STATE_B_IDLE;
			mdwc->vbus_retry_count = 0;
			mdwc->hc_died = false;
			if (!mdwc->stop_host)
				work = 1;
			mdwc->stop_host = false;
		} else {
			dev_dbg(mdwc->dev, "still in a_host state. Resuming root hub.\n");
			dbg_event(0xFF, "XHCIResume", 0);
			if (dwc)
				pm_runtime_resume(&dwc->xhci->dev);
			if (mdwc->no_wakeup_src_in_hostmode)
				pm_wakeup_event(mdwc->dev,
						DWC3_WAKEUP_SRC_TIMEOUT);
		}
		break;

	default:
		dev_err(mdwc->dev, "%s: invalid otg-state\n", __func__);

	}

	if (work)
		queue_delayed_work(mdwc->sm_usb_wq, &mdwc->sm_work, delay);

ret:
	return;
}

static int dwc3_msm_pm_prepare(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	struct usb_hcd	*hcd;
	struct xhci_hcd	*xhci;
	bool stop_ss_host = false;

	dev_dbg(dev, "dwc3-msm PM prepare,lpm:%u\n", atomic_read(&dwc->in_lpm));

	if (!mdwc->in_host_mode || !mdwc->no_wakeup_src_in_hostmode)
		return 0;

	hcd = dev_get_drvdata(&dwc->xhci->dev);
	xhci = hcd_to_xhci(hcd);
	/*
	 * If SS PHY is not suspended then we need to stop host before
	 * suspending ssphy
	 */
	flush_workqueue(mdwc->sm_usb_wq);
	if (atomic_read(&dwc->in_lpm)) {
		if (!(mdwc->lpm_flags & MDWC3_SS_PHY_SUSPEND))
			stop_ss_host = true;
	} else {
		stop_ss_host = dwc3_msm_is_superspeed(mdwc);
	}
	if (stop_ss_host) {
		dbg_event(0xFF, "pm_prep stop_host", mdwc->lpm_flags);
		mdwc->stop_host = true;
		queue_delayed_work(mdwc->dwc3_resume_wq, &mdwc->resume_work, 0);
		/* pm_relax will happen at the end of stop_host */
		pm_stay_awake(mdwc->dev);
		return -EBUSY;
	}
	/* If in lpm then prevent usb core to runtime_resume from pm_suspend */
	if (atomic_read(&dwc->in_lpm)) {
		hcd_to_bus(hcd)->skip_resume = true;
		hcd_to_bus(xhci->shared_hcd)->skip_resume = true;
	} else {
		hcd_to_bus(hcd)->skip_resume = false;
		hcd_to_bus(xhci->shared_hcd)->skip_resume = false;
	}

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int dwc3_msm_pm_suspend(struct device *dev)
{
	int ret = 0;
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(dev, "dwc3-msm PM suspend\n");
	dbg_event(0xFF, "PM Sus", 0);

	flush_workqueue(mdwc->dwc3_resume_wq);
	if (!atomic_read(&dwc->in_lpm) && !mdwc->no_wakeup_src_in_hostmode) {
		dev_err(mdwc->dev, "Abort PM suspend!! (USB is outside LPM)\n");
		return -EBUSY;
	}

	ret = dwc3_msm_suspend(mdwc);
	if (!ret)
		atomic_set(&mdwc->pm_suspended, 1);

	dbg_event(0xFF, "vbus_active", mdwc->vbus_active);
	dbg_event(0xFF, "otg_state", mdwc->otg_state);
	return ret;
}

static int dwc3_msm_pm_resume(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(dev, "dwc3-msm PM resume\n");

	dbg_event(0xFF, "PM Res", 0);

	/* flush to avoid race in read/write of pm_suspended */
	flush_workqueue(mdwc->dwc3_resume_wq);
	atomic_set(&mdwc->pm_suspended, 0);

	/* Resume h/w in host mode as it may not be runtime suspended */
	if (mdwc->no_wakeup_src_in_hostmode && !test_bit(ID, &mdwc->inputs))
		dwc3_msm_resume(mdwc);

	/* kick in otg state machine */
	queue_delayed_work(mdwc->dwc3_resume_wq, &mdwc->resume_work, 0);

	dbg_event(0xFF, "PMResComplete", 0);
	return 0;
}
#endif

#ifdef CONFIG_PM_RUNTIME
static int dwc3_msm_runtime_idle(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(dev, "DWC3-msm runtime idle\n");
	dbg_event(0xFF, "RT Idle", 0);

	return 0;
}

static int dwc3_msm_runtime_suspend(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(dev, "DWC3-msm runtime suspend\n");
	dbg_event(0xFF, "RT Sus", 0);

	return dwc3_msm_suspend(mdwc);
}

static int dwc3_msm_runtime_resume(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(dev, "DWC3-msm runtime resume\n");
	dbg_event(0xFF, "RT Res", 0);

	return dwc3_msm_resume(mdwc);
}
#endif

static const struct dev_pm_ops dwc3_msm_dev_pm_ops = {
	.prepare =	dwc3_msm_pm_prepare,
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_msm_pm_suspend, dwc3_msm_pm_resume)
	SET_RUNTIME_PM_OPS(dwc3_msm_runtime_suspend, dwc3_msm_runtime_resume,
				dwc3_msm_runtime_idle)
};

static const struct of_device_id of_dwc3_matach[] = {
	{
		.compatible = "qcom,dwc-usb3-msm",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_dwc3_matach);

static struct platform_driver dwc3_msm_driver = {
	.probe		= dwc3_msm_probe,
	.remove		= dwc3_msm_remove,
	.driver		= {
		.name	= "msm-dwc3",
		.pm	= &dwc3_msm_dev_pm_ops,
		.of_match_table	= of_dwc3_matach,
	},
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 MSM Glue Layer");

static int dwc3_msm_init(void)
{
	return platform_driver_register(&dwc3_msm_driver);
}
module_init(dwc3_msm_init);

static void __exit dwc3_msm_exit(void)
{
	platform_driver_unregister(&dwc3_msm_driver);
}
module_exit(dwc3_msm_exit);
