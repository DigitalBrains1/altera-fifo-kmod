#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>

#include <asm/io.h>

/* Register definitions based on Intel's altera_avalon_fifo_regs.h */

#define FIFO_EVENT_REG    8
#define FIFO_IENABLE_REG  12

#define FIFO_EVENT_F    (0x01)
#define FIFO_EVENT_E    (0x02)
#define FIFO_EVENT_AF   (0x04)
#define FIFO_EVENT_AE   (0x08)
#define FIFO_EVENT_OVF  (0x10)
#define FIFO_EVENT_UDF  (0x20)
#define FIFO_EVENT_ALL  (0x3F)

#define FIFO_IENABLE_F    (0x01)
#define FIFO_IENABLE_E    (0x02)
#define FIFO_IENABLE_AF   (0x04)
#define FIFO_IENABLE_AE   (0x08)
#define FIFO_IENABLE_OVF  (0x10)
#define FIFO_IENABLE_UDF  (0x20)
#define FIFO_IENABLE_ALL  (0x3F)

static const char altera_version[] = "0.1";
MODULE_LICENSE("GPL");
MODULE_AUTHOR("QBayLogic B.V.");
MODULE_DESCRIPTION("Platform driver for an Altera Avalon FIFO.");
MODULE_VERSION("0.1");

static int altera_probe(struct platform_device *pdev);
static int altera_remove(struct platform_device *pdev);

static const struct of_device_id altera_of_ids[] = {
	{ .compatible = "ALTR,fifo-1.0" },
	{ .compatible = "altr,fifo-1.0" },
	{ }
};

MODULE_DEVICE_TABLE(of, altera_of_ids);

static struct platform_driver altera_driver = {
	.driver                 = {
		.name           = "altera_fifo",
		.owner          = THIS_MODULE,
		.of_match_table = altera_of_ids,
	},
	.probe                  = altera_probe,
	.remove                 = altera_remove,
};

static const char altera_in_name[] = "altera_fifo_in_irq";
static const char altera_out_name[] = "altera_fifo_out_irq";
static const char altera_poll_name[] = "altera_fifo_no_irq";

static irqreturn_t altera_handler(int irq, struct uio_info *dev_info)
{
	void __iomem *csr_base = dev_info->mem[0].internal_addr +
			dev_info->mem[0].offs;
	u32 ien;

	ien = ioread32(csr_base + FIFO_IENABLE_REG);
	if (ien & (1 << 6))
		/* Poorly documented "enable all" flag */
		ien = FIFO_IENABLE_ALL;
	if (!(ioread32(csr_base + FIFO_EVENT_REG) & ien))
		return IRQ_NONE;

	/* Disable interrupt */
	iowrite32(0, csr_base + FIFO_IENABLE_REG);
	return IRQ_HANDLED;
}

static resource_size_t align_resource_size(const struct resource *r) {
	return PAGE_ALIGN(r->end - (r->start & PAGE_MASK) + 1);
}

static void add_csr(struct platform_device *pdev, struct uio_info *info,
		const struct resource *r)
{
	if (info->mem[0].size) {
		dev_warn(&pdev->dev, "multiple CSRs; falling back to "
				"polling");
		info->name = NULL;
		return;
	}
	if (!strcmp(r->name, "in_csr"))
		info->name = altera_in_name;
	else
		info->name = altera_out_name;
	info->mem[0].memtype = UIO_MEM_PHYS;
	info->mem[0].addr = r->start & PAGE_MASK;
	info->mem[0].offs = r->start & ~PAGE_MASK;
	info->mem[0].size = align_resource_size(r);
	info->mem[0].name = r->name;
}

static int add_region(struct platform_device *pdev, struct uio_info *info,
		size_t *mem, const struct resource *r)
{
	if (*mem >= MAX_UIO_MAPS) {
		dev_err(&pdev->dev, "too many memory regions");
		return -1;
	}
	info->mem[*mem].memtype = UIO_MEM_PHYS;
	info->mem[*mem].addr = r->start & PAGE_MASK;
	info->mem[*mem].offs = r->start & ~PAGE_MASK;
	info->mem[*mem].size = align_resource_size(r);
	info->mem[*mem].name = r->name;
	(*mem)++;
	return 0;
}

static int altera_probe(struct platform_device *pdev)
{
	struct uio_info uio_irq, uio_poll;
	struct uio_info *uio_final;
	size_t irq_mem = 1;
	size_t poll_mem = 0;
	u32 i;
	int ret, nr_irqs;

	memset(&uio_irq, 0, sizeof(uio_irq));
	memset(&uio_poll, 0, sizeof(uio_poll));
	uio_irq.version = altera_version;
	uio_poll.name = altera_poll_name;
	uio_poll.version = altera_version;
	uio_final = devm_kzalloc(&pdev->dev, sizeof(*uio_final), GFP_KERNEL);
	if (!uio_final) {
		dev_err(&pdev->dev, "unable to kmalloc\n");
		return -ENOMEM;
	}

	for (i = 0; i < pdev->num_resources; i++) {
		const struct resource *r = &pdev->resource[i];

		if (r->flags != IORESOURCE_MEM)
			continue;
		if (!strcmp(r->name, "in_csr") ||
				!strcmp(r->name, "out_csr")) {
			add_csr(pdev, &uio_irq, r);
			if (add_region(pdev, &uio_poll, &poll_mem, r))
				return -ENODEV;
		} else if (add_region(pdev, &uio_irq, &irq_mem, r) ||
				add_region(pdev, &uio_poll, &poll_mem, r)) {
				return -ENODEV;
		}
	}

	nr_irqs = platform_irq_count(pdev);
	if (nr_irqs > 1) {
		dev_warn(&pdev->dev, "multiple interrupt lines; falling "
				"back to polling");
		uio_irq.name = NULL;
	} else if (nr_irqs < 1) {
		uio_irq.name = NULL;
	}
	if (uio_irq.name != NULL) {
		ret = platform_get_irq(pdev, 0);
		if (ret < 0) {
			dev_warn(&pdev->dev, "failed to get IRQ; falling "
					"back to polling");
			uio_irq.name = NULL;
		} else {
			uio_irq.irq = ret;
		}
	}
	if (uio_irq.name != NULL) {
		uio_irq.irq_flags = IRQF_SHARED;
		uio_irq.handler = altera_handler;
		uio_irq.mem[0].internal_addr = ioremap(
				uio_irq.mem[0].addr,
				uio_irq.mem[0].size);
		if (!uio_irq.mem[0].internal_addr) {
			dev_err(&pdev->dev, "failed to map registers\n");
			return -ENODEV;
		}
		memcpy(uio_final, &uio_irq, sizeof(uio_irq));
	} else {
		memcpy(uio_final, &uio_poll, sizeof(uio_poll));
	}

	if ((ret = uio_register_device(&pdev->dev, uio_final))) {
		dev_err(&pdev->dev, "unable to register uio device\n");
		goto out_unmap;
	}

	platform_set_drvdata(pdev, uio_final);
	return 0;
out_unmap:
	if (uio_final->mem[0].internal_addr)
		iounmap(uio_final->mem[0].internal_addr);
	return ret;
}

static int altera_remove(struct platform_device *pdev)
{
	struct uio_info *info = platform_get_drvdata(pdev);
	uio_unregister_device(info);
	iounmap(info->mem[0].internal_addr);
	return 0;
}

module_platform_driver(altera_driver);
