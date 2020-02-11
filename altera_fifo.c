#include <linux/device.h>
#include <linux/ioport.h>
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

static struct platform_driver altera_driver = {
	.driver                 = {
		.name           = "altera-fifo",
		.owner          = THIS_MODULE,
		.of_match_table = altera_of_ids,
	},
	.probe                  = altera_probe,
	.remove                 = altera_remove,
};

static int altera_probe(struct platform_device *pdev)
{
	struct property *prop;
	u32 i;

	printk(KERN_DEBUG "altera_probe: "
			"name %s\n", pdev->name);
	printk(KERN_DEBUG "altera_probe: "
			"id %u\n", pdev->id);
	printk(KERN_DEBUG "altera_probe: device "
			"init_name %s\n", pdev->dev.init_name);
	printk(KERN_DEBUG "altera_probe: device "
			"platform_data %p\n", pdev->dev.platform_data);
	printk(KERN_DEBUG "altera_probe: device of_node "
			"name %s\n", pdev->dev.of_node->name);
	printk(KERN_DEBUG "altera_probe: device of_node "
			"type %s\n", pdev->dev.of_node->type);
	printk(KERN_DEBUG "altera_probe: device of_node "
			"full_name %s\n", pdev->dev.of_node->full_name);
	for (prop = pdev->dev.of_node->properties; prop != NULL;
			prop = prop->next)
		printk(KERN_DEBUG "altera_probe: device of_node properties "
				"name %s\n", prop->name);
	printk(KERN_DEBUG "altera_probe: "
			"num_resources %u\n", pdev->num_resources);
	for (i = 0; i < pdev->num_resources; i++) {
		printk(KERN_DEBUG "altera_probe: resource "
				"start %08x\n", pdev->resource[i].start);
		printk(KERN_DEBUG "altera_probe: resource "
				"end %08x\n", pdev->resource[i].end);
		printk(KERN_DEBUG "altera_probe: resource "
				"name %p\n", pdev->resource[i].name);
	}
	return -ENODEV;
}

static int altera_remove(struct platform_device *pdev)
{
	return 0;
}

static int __init altera_init(void)
{
	int rc;

	if ((rc = platform_driver_register(&altera_driver))) {
		printk(KERN_ERR "altera_fifo: platform_driver_register "
				"returned %d", rc);
		return -ENODEV;
	}
	return 0;
}

#if 0
static irqreturn_t avalon_handler(int irq, struct uio_info *dev_info)
{
	void __iomem *csr_base = dev_info->mem[1].internal_addr;
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

static int __init avalon_init(void)
{
	struct uio_info *info;

	info = kzalloc(sizeof(struct uio_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->name = "avalon_fifo_fifo_f2h_in";
	info->version = "0.1";
	info->mem[0].name = "out";
	info->mem[0].memtype = UIO_MEM_PHYS;
	/* Hardcoded ALT_LWFPGASLVS_OFST and HW_REGS_MASK */
	info->mem[0].addr = 0xff200000 +
			(FIFO_F2H_OUT_OUT_BASE & 0x03ffffff);
	info->mem[0].size = FIFO_F2H_OUT_OUT_SPAN;
	info->mem[0].internal_addr = ioremap(info->mem[0].addr,
			info->mem[0].size);
	if (!info->mem[0].internal_addr) {
		printk(KERN_ERR "ioremap0 failed.\n");
		goto out_free;
	}
	info->mem[1].name = "in_csr";
	info->mem[1].memtype = UIO_MEM_PHYS;
	/* Hardcoded ALT_LWFPGASLVS_OFST and HW_REGS_MASK */
	info->mem[1].addr = 0xff200000 +
			(FIFO_F2H_OUT_IN_CSR_BASE & 0x03ffffff);
	info->mem[1].size = FIFO_F2H_OUT_IN_CSR_SPAN;
	info->mem[1].internal_addr = ioremap(info->mem[1].addr,
			info->mem[1].size);
	if (!info->mem[1].internal_addr) {
		printk(KERN_ERR "ioremap1 failed.\n");
		goto out_unmap0;
	}
	info->irq = FIFO_F2H_OUT_IN_CSR_IRQ;
	info->irq_flags = IRQF_SHARED;
	info->handler = avalon_handler;

	/* FIXME: Parent device can't be NULL */
	if (uio_register_device(NULL, info)) {
		printk(KERN_ERR "uio_register failed.\n");
		goto out_unmap1;
	}

	return 0;
out_unmap1:
	iounmap(info->mem[1].internal_addr);
out_unmap0:
	iounmap(info->mem[0].internal_addr);
out_free:
	kfree (info);
	return -ENODEV;
}
#endif

static void __exit altera_exit(void)
{
#if 0
	uio_unregister_device(info);
	iounmap(info->mem[1].internal_addr);
	iounmap(info->mem[0].internal_addr);
	kfree (info);
#endif
	platform_driver_unregister(&altera_driver);
}

MODULE_DEVICE_TABLE(of, altera_of_ids);
module_init(altera_init);
module_exit(altera_exit);
