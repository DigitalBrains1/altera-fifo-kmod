#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
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

static irqreturn_t altera_handler(int irq, struct uio_info *dev_info)
{
	void __iomem *csr_base = dev_info->mem[0].internal_addr;

	if (!(ioread32(csr_base + FIFO_EVENT_REG) &
				ioread32(csr_base + FIFO_IENABLE_REG)))
		return IRQ_NONE;

	/* Disable interrupt */
	iowrite32(0, csr_base + FIFO_IENABLE_REG);
	return IRQ_HANDLED;
}

enum altera_mode {
	MODE_IN_IRQ,
	MODE_OUT_IRQ,
	MODE_POLLED
};

struct probe_ctx {
	const struct platform_device *pdev;
	enum altera_mode mode;
	struct uio_info uio_irq, uio_poll;
	size_t irq_mem;
	size_t poll_mem;
	const struct resource *csr;
};

static resource_size_t align_resource_size(const struct resource *r) {
	return PAGE_ALIGN(r->end - (r->start & PAGE_MASK) + 1);
}

static int add_uio_region(const struct platform_device *pdev,
		struct uio_info *info, size_t *mem, const struct resource *r)
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

static int add_mem(struct probe_ctx *ctx, const struct resource *r)
{
	if (!strcmp(r->name, "in_csr") ||
			!strcmp(r->name, "out_csr")) {
		if (ctx->csr != NULL) {
			dev_warn(&ctx->pdev->dev, "multiple CSRs; falling "
					"back to polling");
			ctx->mode = MODE_POLLED;
		}
		ctx->csr = r;
		if (!strcmp(r->name, "in_csr"))
			ctx->mode = MODE_IN_IRQ;
		else
			ctx->mode = MODE_OUT_IRQ;
		ctx->uio_irq.mem[0].memtype = UIO_MEM_PHYS;
		ctx->uio_irq.mem[0].addr = r->start & PAGE_MASK;
		ctx->uio_irq.mem[0].offs = r->start & ~PAGE_MASK;
		ctx->uio_irq.mem[0].size = align_resource_size(r);
		ctx->uio_irq.mem[0].name = r->name;
		if (add_uio_region(ctx->pdev, &ctx->uio_poll, &ctx->poll_mem, r))
			return -1;
	} else if (add_uio_region(ctx->pdev, &ctx->uio_irq, &ctx->irq_mem, r)
			|| add_uio_region(ctx->pdev, &ctx->uio_poll,
				&ctx->poll_mem, r)) {
		return -1;
	}
	return 0;
}

static int altera_probe(struct platform_device *pdev)
{
	struct probe_ctx ctx;
	struct uio_info *uio_final;
	int nr_irqs;
	u32 i;
	int ret;

	ctx.pdev = pdev;
	ctx.mode = MODE_POLLED;
	ctx.irq_mem = 1;
	ctx.poll_mem = 0;
	ctx.csr = NULL;
	memset(&ctx.uio_irq, 0, sizeof(ctx.uio_irq));
	memset(&ctx.uio_poll, 0, sizeof(ctx.uio_poll));
	ctx.uio_irq.version = altera_version;
	ctx.uio_poll.version = altera_version;

	uio_final = devm_kmalloc(&pdev->dev, sizeof(*uio_final), GFP_KERNEL);
	if (!uio_final) {
		dev_err(&pdev->dev, "unable to kmalloc\n");
		return -ENOMEM;
	}

	for (i = 0; i < pdev->num_resources; i++) {
		const struct resource *r = &pdev->resource[i];

		if (r->flags != IORESOURCE_MEM)
			continue;
		if (add_mem(&ctx, r))
			return -ENODEV;
	}

	nr_irqs = platform_irq_count(pdev);
	if (nr_irqs > 1) {
		dev_warn(&pdev->dev, "multiple interrupt lines; falling "
				"back to polling");
		ctx.mode = MODE_POLLED;
	} else if (nr_irqs < 1) {
		ctx.mode = MODE_POLLED;
	}
	if (ctx.mode != MODE_POLLED) {
		ret = platform_get_irq(pdev, 0);
		if (ret < 0) {
			dev_warn(&pdev->dev, "failed to get IRQ; falling "
					"back to polling");
			ctx.mode = MODE_POLLED;
		} else {
			ctx.uio_irq.irq = ret;
		}
	}
	if (ctx.mode != MODE_POLLED) {
		ctx.uio_irq.irq_flags = IRQF_SHARED;
		ctx.uio_irq.handler = altera_handler;
		ctx.uio_irq.mem[0].internal_addr = devm_ioremap(
				&pdev->dev,
				ctx.csr->start,
				resource_size(ctx.csr));
		if (!ctx.uio_irq.mem[0].internal_addr) {
			dev_err(&pdev->dev, "failed to map registers\n");
			return -ENODEV;
		}
		memcpy(uio_final, &ctx.uio_irq, sizeof(ctx.uio_irq));
		if (ctx.mode == MODE_IN_IRQ)
			uio_final->name = "altera_fifo_in_irq";
		else
			uio_final->name = "altera_fifo_out_irq";
	} else {
		memcpy(uio_final, &ctx.uio_poll, sizeof(ctx.uio_poll));
		uio_final->name = "altera_fifo_no_irq";
	}
	if ((ret = uio_register_device(&pdev->dev, uio_final))) {
		dev_err(&pdev->dev, "unable to register uio device\n");
		return ret;
	}

	platform_set_drvdata(pdev, uio_final);
	return 0;
}

static int altera_remove(struct platform_device *pdev)
{
	struct uio_info *info = platform_get_drvdata(pdev);
	uio_unregister_device(info);
	return 0;
}

module_platform_driver(altera_driver);
