#include "kvm/virtio-blk.h"

#include "kvm/virtio-pci-dev.h"
#include "kvm/irq.h"
#include "kvm/disk-image.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/ioeventfd.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>

#include <linux/list.h>
#include <linux/types.h>
#include <pthread.h>

#define VIRTIO_BLK_MAX_DEV		4
#define NUM_VIRT_QUEUES			1

#define VIRTIO_BLK_QUEUE_SIZE		128
/*
 * the header and status consume too entries
 */
#define DISK_SEG_MAX			(VIRTIO_BLK_QUEUE_SIZE - 2)

struct blk_dev_job {
	struct virt_queue		*vq;
	struct blk_dev			*bdev;
	void				*job_id;
};

struct blk_dev {
	pthread_mutex_t			mutex;
	struct list_head		list;

	struct virtio_blk_config	blk_config;
	struct disk_image		*disk;
	u64				base_addr;
	u32				host_features;
	u32				guest_features;
	u16				config_vector;
	u8				status;
	u8				isr;

	/* virtio queue */
	u16				queue_selector;

	struct virt_queue		vqs[NUM_VIRT_QUEUES];
	struct blk_dev_job		jobs[NUM_VIRT_QUEUES];
	struct pci_device_header	pci_hdr;
};

static LIST_HEAD(bdevs);

static bool virtio_blk_dev_in(struct blk_dev *bdev, void *data, unsigned long offset, int size, u32 count)
{
	u8 *config_space = (u8 *) &bdev->blk_config;

	if (size != 1 || count != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_MSI_CONFIG_VECTOR]);

	return true;
}

static bool virtio_blk_pci_io_in(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	struct blk_dev *bdev;
	u16 offset;
	bool ret = true;

	bdev	= ioport->priv;
	offset	= port - bdev->base_addr;

	mutex_lock(&bdev->mutex);

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, bdev->host_features);
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
		ret		= false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, bdev->vqs[bdev->queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_BLK_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret		= false;
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, bdev->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, bdev->isr);
		kvm__irq_line(kvm, bdev->pci_hdr.irq_line, VIRTIO_IRQ_LOW);
		bdev->isr = VIRTIO_IRQ_LOW;
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		ioport__write16(data, bdev->config_vector);
		break;
	default:
		ret = virtio_blk_dev_in(bdev, data, offset, size, count);
		break;
	};

	mutex_unlock(&bdev->mutex);

	return ret;
}

static bool virtio_blk_do_io_request(struct kvm *kvm,
					struct blk_dev *bdev,
					struct virt_queue *queue)
{
	struct iovec iov[VIRTIO_BLK_QUEUE_SIZE];
	struct virtio_blk_outhdr *req;
	ssize_t block_cnt = -1;
	u16 out, in, head;
	u8 *status;

	head			= virt_queue__get_iov(queue, iov, &out, &in, kvm);

	/* head */
	req			= iov[0].iov_base;

	switch (req->type) {
	case VIRTIO_BLK_T_IN:
		block_cnt	= disk_image__read(bdev->disk, req->sector, iov + 1, in + out - 2);
		break;
	case VIRTIO_BLK_T_OUT:
		block_cnt	= disk_image__write(bdev->disk, req->sector, iov + 1, in + out - 2);
		break;
	case VIRTIO_BLK_T_FLUSH:
		block_cnt       = disk_image__flush(bdev->disk);
		break;
	default:
		pr_warning("request type %d", req->type);
		block_cnt	= -1;
		break;
	}

	/* status */
	status			= iov[out + in - 1].iov_base;
	*status			= (block_cnt < 0) ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

	virt_queue__set_used_elem(queue, head, block_cnt);

	return true;
}

static void virtio_blk_do_io(struct kvm *kvm, void *param)
{
	struct blk_dev_job *job	= param;
	struct virt_queue *vq;
	struct blk_dev *bdev;

	vq			= job->vq;
	bdev			= job->bdev;

	while (virt_queue__available(vq))
		virtio_blk_do_io_request(kvm, bdev, vq);

	virt_queue__trigger_irq(vq, bdev->pci_hdr.irq_line, &bdev->isr, kvm);
}

static bool virtio_blk_pci_io_out(struct ioport *ioport, struct kvm *kvm, u16 port, void *data, int size, u32 count)
{
	struct blk_dev *bdev;
	u16 offset;
	bool ret = true;

	bdev	= ioport->priv;
	offset	= port - bdev->base_addr;

	mutex_lock(&bdev->mutex);

	switch (offset) {
	case VIRTIO_PCI_GUEST_FEATURES:
		bdev->guest_features	= ioport__read32(data);
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		struct virt_queue *queue;
		struct blk_dev_job *job;
		void *p;

		job = &bdev->jobs[bdev->queue_selector];

		queue			= &bdev->vqs[bdev->queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_pfn_to_host(kvm, queue->pfn);

		vring_init(&queue->vring, VIRTIO_BLK_QUEUE_SIZE, p, VIRTIO_PCI_VRING_ALIGN);

		*job			= (struct blk_dev_job) {
			.vq			= queue,
			.bdev			= bdev,
		};

		job->job_id = thread_pool__add_job(kvm, virtio_blk_do_io, job);

		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		bdev->queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index;

		queue_index		= ioport__read16(data);
		thread_pool__do_job(bdev->jobs[queue_index].job_id);

		break;
	}
	case VIRTIO_PCI_STATUS:
		bdev->status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		bdev->config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	case VIRTIO_MSI_QUEUE_VECTOR:
		break;
	default:
		ret			= false;
		break;
	};

	mutex_unlock(&bdev->mutex);

	return ret;
}

static struct ioport_operations virtio_blk_io_ops = {
	.io_in	= virtio_blk_pci_io_in,
	.io_out	= virtio_blk_pci_io_out,
};

static void ioevent_callback(struct kvm *kvm, void *param)
{
	struct blk_dev_job *job = param;

	thread_pool__do_job(job->job_id);
}

void virtio_blk__init(struct kvm *kvm, struct disk_image *disk)
{
	u16 blk_dev_base_addr;
	u8 dev, pin, line, i;
	struct blk_dev *bdev;
	struct ioevent ioevent;

	if (!disk)
		return;

	bdev = calloc(1, sizeof(struct blk_dev));
	if (bdev == NULL)
		die("Failed allocating bdev");

	blk_dev_base_addr	= ioport__register(IOPORT_EMPTY, &virtio_blk_io_ops, IOPORT_SIZE, bdev);

	*bdev			= (struct blk_dev) {
		.mutex				= PTHREAD_MUTEX_INITIALIZER,
		.disk				= disk,
		.base_addr			= blk_dev_base_addr,
		.blk_config			= (struct virtio_blk_config) {
			.capacity		= disk->size / SECTOR_SIZE,
			.seg_max		= DISK_SEG_MAX,
		},
		.pci_hdr = (struct pci_device_header) {
			.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
			.device_id		= PCI_DEVICE_ID_VIRTIO_BLK,
			.header_type		= PCI_HEADER_TYPE_NORMAL,
			.revision_id		= 0,
			.class			= 0x010000,
			.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
			.subsys_id		= VIRTIO_ID_BLOCK,
			.bar[0]			= blk_dev_base_addr | PCI_BASE_ADDRESS_SPACE_IO,
		},
		/*
		 * Note we don't set VIRTIO_BLK_F_GEOMETRY here so the
		 * guest kernel will compute disk geometry by own, the
		 * same applies to VIRTIO_BLK_F_BLK_SIZE
		 */
		.host_features			= (1UL << VIRTIO_BLK_F_SEG_MAX | 1UL << VIRTIO_BLK_F_FLUSH),
	};

	list_add_tail(&bdev->list, &bdevs);

	if (irq__register_device(VIRTIO_ID_BLOCK, &dev, &pin, &line) < 0)
		return;

	bdev->pci_hdr.irq_pin	= pin;
	bdev->pci_hdr.irq_line	= line;

	pci__register(&bdev->pci_hdr, dev);

	for (i = 0; i < NUM_VIRT_QUEUES; i++) {
		ioevent = (struct ioevent) {
			.io_addr		= blk_dev_base_addr + VIRTIO_PCI_QUEUE_NOTIFY,
			.io_len			= sizeof(u16),
			.fn			= ioevent_callback,
			.datamatch		= i,
			.fn_ptr			= &bdev->jobs[i],
			.fn_kvm			= kvm,
			.fd			= eventfd(0, 0),
		};

		ioeventfd__add_event(&ioevent);
	}
}

void virtio_blk__init_all(struct kvm *kvm)
{
	int i;

	for (i = 0; i < kvm->nr_disks; i++)
		virtio_blk__init(kvm, kvm->disks[i]);
}

void virtio_blk__delete_all(struct kvm *kvm)
{
	while (!list_empty(&bdevs)) {
		struct blk_dev *bdev;

		bdev = list_first_entry(&bdevs, struct blk_dev, list);
		ioeventfd__del_event(bdev->base_addr + VIRTIO_PCI_QUEUE_NOTIFY, 0);
		list_del(&bdev->list);
		free(bdev);
	}
}
