// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>

#include <sof/drivers/interrupt.h>
#include <sof/drivers/ipc.h>
#include <sof/lib/alloc.h>
#include <sof/lib/dma.h>
#include <sof/lib/mailbox.h>
#include <sof/lib/memory.h>
#include <sof/lib/shim.h>
#include <sof/lib/wait.h>
#include <sof/list.h>
#include <sof/platform.h>
#include <sof/schedule/edf_schedule.h>
#include <sof/schedule/schedule.h>
#include <sof/schedule/task.h>
#include <sof/spinlock.h>
#include <ipc/header.h>
#include <ipc/topology.h>
#include <stdint.h>

/* private data for IPC */
struct ipc_data {
	struct ipc_data_host_buffer dh_buffer;
};

static void irq_handler(void *arg)
{
	struct ipc *ipc = arg;
	uint32_t isr, imrd;

	/* Interrupt arrived, check src */
	isr = shim_read(SHIM_ISRD);
	imrd = shim_read(SHIM_IMRD);

	tracev_ipc("ipc: irq isr 0x%x", isr);

	if (isr & SHIM_ISRD_DONE) {

		/* Mask Done interrupt before return */
		shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) | SHIM_IMRD_DONE);

		/* clear DONE bit - tell Host we have completed */
		shim_write(SHIM_IPCD, 0);

		/* unmask Done interrupt */
		shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) & ~SHIM_IMRD_DONE);
	}

	if (isr & SHIM_ISRD_BUSY && !(imrd & SHIM_IMRD_BUSY)) {

		/* Mask Busy interrupt before return */
		shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) | SHIM_IMRD_BUSY);

		ipc_schedule_process(ipc);
	}
}

enum task_state ipc_platform_do_cmd(void *data)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_cmd_hdr *hdr;
	/* Use struct ipc_data *iipc = ipc_get_drvdata(ipc); if needed */

	/* perform command */
	hdr = mailbox_validate();
	ipc_cmd(hdr);

	platform_shared_commit(ipc, sizeof(*ipc));

	return SOF_TASK_STATE_COMPLETED;
}

void ipc_platform_complete_cmd(void *data)
{
	struct ipc *ipc = data;

	/* clear BUSY bit and set DONE bit - accept new messages */
	shim_write(SHIM_IPCX, SHIM_IPCX_DONE);

	/* unmask busy interrupt */
	shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) & ~SHIM_IMRD_BUSY);

	// TODO: signal audio work to enter D3 in normal context
	/* are we about to enter D3 ? */
	if (ipc->pm_prepare_D3) {
		platform_shared_commit(ipc, sizeof(*ipc));

		while (1)
			wait_for_interrupt(0);
	}

	platform_shared_commit(ipc, sizeof(*ipc));
}

void ipc_platform_send_msg(void)
{
	struct ipc *ipc = ipc_get();
	struct ipc_msg *msg;
	uint32_t flags;

	spin_lock_irq(&ipc->lock, flags);

	/* any messages to send ? */
	if (list_is_empty(&ipc->msg_list))
		goto out;

	/* can't send nofication when one is in progress */
	if (shim_read(SHIM_IPCD) & (SHIM_IPCD_BUSY | SHIM_IPCD_DONE))
		goto out;

	/* now send the message */
	msg = list_first_item(&ipc->msg_list, struct ipc_msg,
			      list);
	mailbox_dspbox_write(0, msg->tx_data, msg->tx_size);
	list_item_del(&msg->list);
	tracev_ipc("ipc: msg tx -> 0x%x", msg->header);

	/* now interrupt host to tell it we have message sent */
	shim_write(SHIM_IPCD, SHIM_IPCD_BUSY);

	list_item_append(&msg->list, &ipc->empty_list);

	platform_shared_commit(msg, sizeof(*msg));

out:
	platform_shared_commit(ipc, sizeof(*ipc));

	spin_unlock_irq(&ipc->lock, flags);
}

struct ipc_data_host_buffer *ipc_platform_get_host_buffer(struct ipc *ipc)
{
	struct ipc_data *iipc = ipc_get_drvdata(ipc);

	platform_shared_commit(ipc, sizeof(*ipc));

	return &iipc->dh_buffer;
}

int platform_ipc_init(struct ipc *ipc)
{
	struct ipc_data *iipc;
	uint32_t imrd, dir, caps, dev;

	/* init ipc data */
	iipc = rzalloc(SOF_MEM_ZONE_SYS, 0, SOF_MEM_CAPS_RAM,
		       sizeof(struct ipc_data));
	ipc_set_drvdata(ipc, iipc);

	/* schedule */
	schedule_task_init_edf(&ipc->ipc_task, &ipc_task_ops, ipc, 0, 0);

#if CONFIG_HOST_PTABLE
	/* allocate page table buffer */
	iipc->dh_buffer.page_table = rzalloc(SOF_MEM_ZONE_SYS, 0,
					     SOF_MEM_CAPS_RAM,
					     PLATFORM_PAGE_TABLE_SIZE);
	if (iipc->dh_buffer.page_table)
		bzero(iipc->dh_buffer.page_table, PLATFORM_PAGE_TABLE_SIZE);
#endif

	/* request GP DMA with shared access privilege */
	caps = 0;
	dir = DMA_DIR_HMEM_TO_LMEM;
	dev = DMA_DEV_HOST;
	iipc->dh_buffer.dmac = dma_get(dir, caps, dev, DMA_ACCESS_SHARED);

	/* configure interrupt */
	interrupt_register(PLATFORM_IPC_INTERRUPT, irq_handler, ipc);
	interrupt_enable(PLATFORM_IPC_INTERRUPT, ipc);

	/* Unmask Busy and Done interrupts */
	imrd = shim_read(SHIM_IMRD);
	imrd &= ~(SHIM_IMRD_BUSY | SHIM_IMRD_DONE);
	shim_write(SHIM_IMRD, imrd);

	platform_shared_commit(ipc, sizeof(*ipc));

	return 0;
}

#if CONFIG_IPC_POLLING

int ipc_platform_poll_init(void)
{
	return 0;
}

/* tell host we have completed command */
void ipc_platform_poll_set_cmd_done(void)
{
	/* clear BUSY bit and set DONE bit - accept new messages */
	shim_write(SHIM_IPCX, SHIM_IPCX_DONE);

	/* unmask busy interrupt */
	shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) & ~SHIM_IMRD_BUSY);
}

/* read the IPC register for any new command messages */
int ipc_platform_poll_is_cmd_pending(void)
{
	uint32_t isr, imrd;

	/* Interrupt arrived, check src */
	isr = shim_read(SHIM_ISRD);
	imrd = shim_read(SHIM_IMRD);

	if (isr & SHIM_ISRD_BUSY && !(imrd & SHIM_IMRD_BUSY)) {

		/* Mask Busy interrupt before return */
		shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) | SHIM_IMRD_BUSY);

		/* new message */
		return 1;
	}

	/* no new message */
	return 0;
}

int ipc_platform_poll_is_host_ready(void)
{
	uint32_t isr;

	/* Interrupt arrived, check src */
	isr = shim_read(SHIM_ISRD);

	if (isr & SHIM_ISRD_DONE) {

		/* Mask Done interrupt before return */
		shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) | SHIM_IMRD_DONE);

		/* clear DONE bit - tell Host we have completed */
		shim_write(SHIM_IPCD, 0);

		/* unmask Done interrupt */
		shim_write(SHIM_IMRD, shim_read(SHIM_IMRD) & ~SHIM_IMRD_DONE);

		/* host done */
		return 1;
	}

	/* host still pending */
	return 0;
}

int ipc_platform_poll_tx_host_msg(struct ipc_msg *msg)
{
	/* can't send nofication when one is in progress */
	if (shim_read(SHIM_IPCD) & (SHIM_IPCD_BUSY | SHIM_IPCD_DONE))
		return 0;

	/* now send the message */
	mailbox_dspbox_write(0, msg->tx_data, msg->tx_size);

	/* now interrupt host to tell it we have message sent */
	shim_write(SHIM_IPCD, SHIM_IPCD_BUSY);

	/* message sent */
	platform_shared_commit(msg, sizeof(*msg));
	return 1;
}

#endif
