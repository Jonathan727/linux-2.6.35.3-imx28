/*
 * Fast Ethernet Controller (FEC) driver for Motorola MPC8xx.
 * Copyright (c) 1997 Dan Malek (dmalek@jlc.net)
 *
 * Right now, I am very wasteful with the buffers.  I allocate memory
 * pages and then divide them into 2K frame buffers.  This way I know I
 * have buffers large enough to hold one frame within one buffer descriptor.
 * Once I get this working, I will use 64 or 128 byte CPM buffers, which
 * will be much more memory efficient and will easily handle lots of
 * small packets.
 *
 * Much better multiple PHY support by Magnus Damm.
 * Copyright (c) 2000 Ericsson Radio Systems AB.
 *
 * Support for FEC controller of ColdFire processors.
 * Copyright (c) 2001-2005 Greg Ungerer (gerg@snapgear.com)
 *
 * Bug fixes and cleanup by Philippe De Muyter (phdm@macqel.be)
 * Copyright (c) 2004-2006 Macq Electronique SA.
 *
 * Support for FEC IEEE 1588.
 * Copyright (C) 2012 Freescale Semiconductor, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/swab.h>
#include <linux/phy.h>
#include <linux/fec.h>
#include <linux/suspend.h>

#include <asm/cacheflush.h>

#ifndef CONFIG_ARCH_MXC
#ifndef CONFIG_ARCH_MXS
#include <asm/coldfire.h>
#include <asm/mcfsim.h>
#endif
#endif

#include "fec.h"
#include "fec_1588.h"

#if defined(CONFIG_ARCH_MXC) || defined(CONFIG_ARCH_MXS)
#define FEC_ALIGNMENT	0xf
#else
#define FEC_ALIGNMENT	0x3
#endif

#if defined(CONFIG_M5272)
/*
 * Some hardware gets it MAC address out of local flash memory.
 * if this is non-zero then assume it is the address to get MAC from.
 */
#if defined(CONFIG_NETtel)
#define	FEC_FLASHMAC	0xf0006006
#elif defined(CONFIG_GILBARCONAP) || defined(CONFIG_SCALES)
#define	FEC_FLASHMAC	0xf0006000
#elif defined(CONFIG_CANCam)
#define	FEC_FLASHMAC	0xf0020000
#elif defined (CONFIG_M5272C3)
#define	FEC_FLASHMAC	(0xffe04000 + 4)
#elif defined(CONFIG_MOD5272)
#define FEC_FLASHMAC 	0xffc0406b
#else
#define	FEC_FLASHMAC	0
#endif
#endif /* CONFIG_M5272 */

/* The number of Tx and Rx buffers.  These are allocated from the page
 * pool.  The code may assume these are power of two, so it it best
 * to keep them that size.
 * We don't need to allocate pages for the transmitter.  We just use
 * the skbuffer directly.
 */
#define FEC_ENET_RX_PAGES	8
#define FEC_ENET_RX_FRSIZE	2048
#define FEC_ENET_RX_FRPPG	(PAGE_SIZE / FEC_ENET_RX_FRSIZE)
#define RX_RING_SIZE		(FEC_ENET_RX_FRPPG * FEC_ENET_RX_PAGES)
#define FEC_ENET_TX_FRSIZE	2048
#define FEC_ENET_TX_FRPPG	(PAGE_SIZE / FEC_ENET_TX_FRSIZE)
#define TX_RING_SIZE		16	/* Must be power of two */
#define TX_RING_MOD_MASK	15	/*   for this to work */

#if (((RX_RING_SIZE + TX_RING_SIZE) * 8) > PAGE_SIZE)
#error "FEC: descriptor ring size constants too large"
#endif

/* Interrupt events/masks. */
#define FEC_ENET_HBERR	((uint)0x80000000)	/* Heartbeat error */
#define FEC_ENET_BABR	((uint)0x40000000)	/* Babbling receiver */
#define FEC_ENET_BABT	((uint)0x20000000)	/* Babbling transmitter */
#define FEC_ENET_GRA	((uint)0x10000000)	/* Graceful stop complete */
#define FEC_ENET_TXF	((uint)0x08000000)	/* Full frame transmitted */
#define FEC_ENET_TXB	((uint)0x04000000)	/* A buffer was transmitted */
#define FEC_ENET_RXF	((uint)0x02000000)	/* Full frame received */
#define FEC_ENET_RXB	((uint)0x01000000)	/* A buffer was received */
#define FEC_ENET_MII	((uint)0x00800000)	/* MII interrupt */
#define FEC_ENET_EBERR	((uint)0x00400000)	/* SDMA bus error */
#define FEC_ENET_TS_AVAIL	((uint)0x00010000)
#define FEC_ENET_TS_TIMER	((uint)0x00008000)

#if defined(CONFIG_FEC_1588) && defined(CONFIG_ARCH_MX28)
#define FEC_DEFAULT_IMASK (FEC_ENET_TXF | FEC_ENET_RXF | FEC_ENET_MII | \
				FEC_ENET_TS_AVAIL | FEC_ENET_TS_TIMER)
#else
#define FEC_DEFAULT_IMASK (FEC_ENET_TXF | FEC_ENET_RXF | FEC_ENET_MII)
#endif

/* The FEC stores dest/src/type, data, and checksum for receive packets.
 */
#define PKT_MAXBUF_SIZE		1518
#define PKT_MINBUF_SIZE		64
#define PKT_MAXBLR_SIZE		1536


/*
 * The 5270/5271/5280/5282/532x RX control register also contains maximum frame
 * size bits. Other FEC hardware does not, so we need to take that into
 * account when setting it.
 */
#if defined(CONFIG_M523x) || defined(CONFIG_M527x) || defined(CONFIG_M528x) || \
    defined(CONFIG_M520x) || defined(CONFIG_M532x) || \
    defined(CONFIG_ARCH_MXC) || defined(CONFIG_ARCH_MXS)
#define	OPT_FRAME_SIZE	(PKT_MAXBUF_SIZE << 16)
#else
#define	OPT_FRAME_SIZE	0
#endif

/* The FEC buffer descriptors track the ring buffers.  The rx_bd_base and
 * tx_bd_base always point to the base of the buffer descriptors.  The
 * cur_rx and cur_tx point to the currently available buffer.
 * The dirty_tx tracks the current buffer that is being sent by the
 * controller.  The cur_tx and dirty_tx are equal under both completely
 * empty and completely full conditions.  The empty/ready indicator in
 * the buffer descriptor determines the actual condition.
 */
struct fec_enet_private {
	/* Hardware registers of the FEC device */
	void __iomem *hwp;

	struct net_device *netdev;

	struct clk *clk;

	/* The saved address of a sent-in-place packet/buffer, for skfree(). */
	unsigned char *tx_bounce[TX_RING_SIZE];
	struct	sk_buff* tx_skbuff[TX_RING_SIZE];
	struct	sk_buff* rx_skbuff[RX_RING_SIZE];
	ushort	skb_cur;
	ushort	skb_dirty;

	/* CPM dual port RAM relative addresses */
	dma_addr_t	bd_dma;
	/* Address of Rx and Tx buffers */
	struct bufdesc	*rx_bd_base;
	struct bufdesc	*tx_bd_base;
	/* The next free ring entry */
	struct bufdesc	*cur_rx, *cur_tx; 
	/* The ring entries to be free()ed */
	struct bufdesc	*dirty_tx;

	uint	tx_full;
	/* hold while accessing the HW like ringbuffer for tx/rx but not MAC */
	spinlock_t hw_lock;

	struct  platform_device *pdev;

	int	opened;

	/* Phylib and MDIO interface */
	struct  mii_bus *mii_bus;
	struct  phy_device *phy_dev;
	int     mii_timeout;
	uint    phy_speed;
	phy_interface_t	phy_interface;
	int	index;
	int	link;
	int	full_duplex;
	struct  completion mdio_done;

	struct	fec_ptp_private *ptp_priv;
	uint	ptimer_present;
};

/*
 * Define the fixed address of the FEC hardware.
 */
static unsigned char	fec_mac_default[ETH_ALEN];
static struct mii_bus *fec_mii_bus;

static irqreturn_t fec_enet_interrupt(int irq, void * dev_id);
static void fec_enet_tx(struct net_device *dev);
static void fec_enet_rx(struct net_device *dev);
static int fec_enet_close(struct net_device *dev);
static void fec_restart(struct net_device *dev, int duplex);
static void fec_stop(struct net_device *dev);

int hasphy = 1;

/* FEC MII MMFR bits definition */
#define FEC_MMFR_ST		(1 << 30)
#define FEC_MMFR_OP_READ	(2 << 28)
#define FEC_MMFR_OP_WRITE	(1 << 28)
#define FEC_MMFR_PA(v)		((v & 0x1f) << 23)
#define FEC_MMFR_RA(v)		((v & 0x1f) << 18)
#define FEC_MMFR_TA		(2 << 16)
#define FEC_MMFR_DATA(v)	(v & 0xffff)

#define FEC_MII_TIMEOUT		1000

/* Transmitter timeout */
#define TX_TIMEOUT (2 * HZ)

#ifdef CONFIG_ARCH_MXS
static void *swap_buffer(void *bufaddr, int len)
{
	int i;
	unsigned int *buf = bufaddr;

	for (i = 0; i < (len + 3) / 4; i++, buf++)
		*buf = __swab32(*buf);

	return bufaddr;
}
#endif

static int
fec_enet_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct bufdesc *bdp;
	void *bufaddr;
	unsigned short	status;
	unsigned long	estatus;
	unsigned long flags;

	if (!fep->link) {
		/* Link is down or autonegotiation is in progress. */
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}

	spin_lock_irqsave(&fep->hw_lock, flags);
	/* Fill in a Tx ring entry */
	bdp = fep->cur_tx;

	status = bdp->cbd_sc;

	if (status & BD_ENET_TX_READY) {
		/* Ooops.  All transmit buffers are full.  Bail out.
		 * This should not happen, since dev->tbusy should be set.
		 */
		printk("%s: tx queue full!.\n", dev->name);
		spin_unlock_irqrestore(&fep->hw_lock, flags);
		return NETDEV_TX_BUSY;
	}

	/* Clear all of the status flags */
	status &= ~BD_ENET_TX_STATS;

	/* Set buffer length and buffer pointer */
	bufaddr = skb->data;
	bdp->cbd_datlen = skb->len;

	/*
	 * On some FEC implementations data must be aligned on
	 * 4-byte boundaries. Use bounce buffers to copy data
	 * and get it aligned. Ugh.
	 */
	if (((unsigned long) bufaddr) & FEC_ALIGNMENT) {
		unsigned int index;
		index = bdp - fep->tx_bd_base;
		memcpy(fep->tx_bounce[index], (void *)skb->data, skb->len);
		bufaddr = fep->tx_bounce[index];
	}

	if (fep->ptimer_present) {
		if (fec_ptp_do_txstamp(skb)) {
			estatus = BD_ENET_TX_TS;
			status |= BD_ENET_TX_PTP;
		} else
			estatus = 0;
#ifdef CONFIG_ENHANCED_BD
		bdp->cbd_esc = (estatus | BD_ENET_TX_INT);
		bdp->cbd_bdu = 0;
#endif
	}

#ifdef CONFIG_ARCH_MXS
	swap_buffer(bufaddr, skb->len);
#endif
	/* Save skb pointer */
	fep->tx_skbuff[fep->skb_cur] = skb;

	dev->stats.tx_bytes += skb->len;
	fep->skb_cur = (fep->skb_cur+1) & TX_RING_MOD_MASK;

	/* Push the data cache so the CPM does not get stale memory
	 * data.
	 */
	bdp->cbd_bufaddr = dma_map_single(&dev->dev, bufaddr,
			FEC_ENET_TX_FRSIZE, DMA_TO_DEVICE);

	/* Send it on its way.  Tell FEC it's ready, interrupt when done,
	 * it's the last BD of the frame, and to put the CRC on the end.
	 */
	status |= (BD_ENET_TX_READY | BD_ENET_TX_INTR
			| BD_ENET_TX_LAST | BD_ENET_TX_TC);
	bdp->cbd_sc = status;

	/* Trigger transmission start */
	writel(0, fep->hwp + FEC_X_DES_ACTIVE);

	/* If this was the last BD in the ring, start at the beginning again. */
	if (status & BD_ENET_TX_WRAP)
		bdp = fep->tx_bd_base;
	else
		bdp++;

	if (bdp == fep->dirty_tx) {
		fep->tx_full = 1;
		netif_stop_queue(dev);
	}

	fep->cur_tx = bdp;

	spin_unlock_irqrestore(&fep->hw_lock, flags);

	return NETDEV_TX_OK;
}

static void
fec_timeout(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);

	dev->stats.tx_errors++;

	fec_restart(dev, fep->full_duplex);
	netif_wake_queue(dev);
}

static irqreturn_t
fec_enet_interrupt(int irq, void * dev_id)
{
	struct	net_device *dev = dev_id;
	struct fec_enet_private *fep = netdev_priv(dev);
	struct fec_ptp_private *fpp = fep->ptp_priv;
	uint	int_events;
	irqreturn_t ret = IRQ_NONE;

	do {
		int_events = readl(fep->hwp + FEC_IEVENT);
		writel(int_events, fep->hwp + FEC_IEVENT);

		if (int_events & FEC_ENET_RXF) {
			ret = IRQ_HANDLED;
			fec_enet_rx(dev);
		}

		/* Transmit OK, or non-fatal error. Update the buffer
		 * descriptors. FEC handles all errors, we just discover
		 * them as part of the transmit process.
		 */
		if (int_events & FEC_ENET_TXF) {
			ret = IRQ_HANDLED;
			fec_enet_tx(dev);
		}

		if (int_events & FEC_ENET_TS_TIMER) {
			ret = IRQ_HANDLED;
			if (fep->ptimer_present && fpp)
				fpp->prtc++;
		}

		if (int_events & FEC_ENET_MII) {
			ret = IRQ_HANDLED;
			complete(&fep->mdio_done);
		}
	} while (int_events);

	return ret;
}


static void
fec_enet_tx(struct net_device *dev)
{
	struct	fec_enet_private *fep;
	struct  fec_ptp_private *fpp;
	struct bufdesc *bdp;
	unsigned short status;
	unsigned long estatus;
	struct	sk_buff	*skb;

	fep = netdev_priv(dev);
	fpp = fep->ptp_priv;
	spin_lock(&fep->hw_lock);
	bdp = fep->dirty_tx;

	while (((status = bdp->cbd_sc) & BD_ENET_TX_READY) == 0) {
		if (bdp == fep->cur_tx && fep->tx_full == 0)
			break;

		dma_unmap_single(&dev->dev, bdp->cbd_bufaddr, FEC_ENET_TX_FRSIZE, DMA_TO_DEVICE);
		bdp->cbd_bufaddr = 0;

		skb = fep->tx_skbuff[fep->skb_dirty];
		/* Check for errors. */
		if (status & (BD_ENET_TX_HB | BD_ENET_TX_LC |
				   BD_ENET_TX_RL | BD_ENET_TX_UN |
				   BD_ENET_TX_CSL)) {
			dev->stats.tx_errors++;
			if (status & BD_ENET_TX_HB)  /* No heartbeat */
				dev->stats.tx_heartbeat_errors++;
			if (status & BD_ENET_TX_LC)  /* Late collision */
				dev->stats.tx_window_errors++;
			if (status & BD_ENET_TX_RL)  /* Retrans limit */
				dev->stats.tx_aborted_errors++;
			if (status & BD_ENET_TX_UN)  /* Underrun */
				dev->stats.tx_fifo_errors++;
			if (status & BD_ENET_TX_CSL) /* Carrier lost */
				dev->stats.tx_carrier_errors++;
		} else {
			dev->stats.tx_packets++;
		}

		if (status & BD_ENET_TX_READY)
			printk("HEY! Enet xmit interrupt and TX_READY.\n");

		/* Deferred means some collisions occurred during transmit,
		 * but we eventually sent the packet OK.
		 */
		if (status & BD_ENET_TX_DEF)
			dev->stats.collisions++;

#if defined(CONFIG_ENHANCED_BD)
		if (fep->ptimer_present) {
			estatus = bdp->cbd_esc;
			if (estatus & BD_ENET_TX_TS)
				fec_ptp_store_txstamp(fpp, skb, bdp);
		}
#elif defined(CONFIG_IN_BAND)
		if (fep->ptimer_present) {
			if (status & BD_ENET_TX_PTP)
				fec_ptp_store_txstamp(fpp, skb, bdp);
		}
#endif

		/* Free the sk buffer associated with this last transmit */
		dev_kfree_skb_any(skb);
		fep->tx_skbuff[fep->skb_dirty] = NULL;
		fep->skb_dirty = (fep->skb_dirty + 1) & TX_RING_MOD_MASK;

		/* Update pointer to next buffer descriptor to be transmitted */
		if (status & BD_ENET_TX_WRAP)
			bdp = fep->tx_bd_base;
		else
			bdp++;

		/* Since we have freed up a buffer, the ring is no longer full
		 */
		if (fep->tx_full) {
			fep->tx_full = 0;
			if (netif_queue_stopped(dev))
				netif_wake_queue(dev);
		}
	}
	fep->dirty_tx = bdp;
	spin_unlock(&fep->hw_lock);
}


/* During a receive, the cur_rx points to the current incoming buffer.
 * When we update through the ring, if the next incoming buffer has
 * not been given to the system, we just set the empty indicator,
 * effectively tossing the packet.
 */
static void
fec_enet_rx(struct net_device *dev)
{
	struct	fec_enet_private *fep = netdev_priv(dev);
	struct  fec_ptp_private *fpp = fep->ptp_priv;
	struct bufdesc *bdp;
	unsigned short status;
	struct	sk_buff	*skb;
	ushort	pkt_len;
	__u8 *data;

#ifdef CONFIG_M532x
	flush_cache_all();
#endif

	spin_lock(&fep->hw_lock);

	/* First, grab all of the stats for the incoming packet.
	 * These get messed up if we get called due to a busy condition.
	 */
	bdp = fep->cur_rx;

	while (!((status = bdp->cbd_sc) & BD_ENET_RX_EMPTY)) {

		/* Since we have allocated space to hold a complete frame,
		 * the last indicator should be set.
		 */
		if ((status & BD_ENET_RX_LAST) == 0)
			printk("FEC ENET: rcv is not +last\n");

		if (!fep->opened)
			goto rx_processing_done;

		/* Check for errors. */
		if (status & (BD_ENET_RX_LG | BD_ENET_RX_SH | BD_ENET_RX_NO |
			   BD_ENET_RX_CR | BD_ENET_RX_OV)) {
			dev->stats.rx_errors++;
			if (status & (BD_ENET_RX_LG | BD_ENET_RX_SH)) {
				/* Frame too long or too short. */
				dev->stats.rx_length_errors++;
			}
			if (status & BD_ENET_RX_NO)	/* Frame alignment */
				dev->stats.rx_frame_errors++;
			if (status & BD_ENET_RX_CR)	/* CRC Error */
				dev->stats.rx_crc_errors++;
			if (status & BD_ENET_RX_OV)	/* FIFO overrun */
				dev->stats.rx_fifo_errors++;
		}

		/* Report late collisions as a frame error.
		 * On this error, the BD is closed, but we don't know what we
		 * have in the buffer.  So, just drop this frame on the floor.
		 */
		if (status & BD_ENET_RX_CL) {
			dev->stats.rx_errors++;
			dev->stats.rx_frame_errors++;
			goto rx_processing_done;
		}

		/* Process the incoming frame. */
		dev->stats.rx_packets++;
		pkt_len = bdp->cbd_datlen;
		dev->stats.rx_bytes += pkt_len;
		data = (__u8*)__va(bdp->cbd_bufaddr);

	        dma_unmap_single(NULL, bdp->cbd_bufaddr, bdp->cbd_datlen,
        			DMA_FROM_DEVICE);
#ifdef CONFIG_ARCH_MXS
		swap_buffer(data, pkt_len);
#endif
		/* This does 16 byte alignment, exactly what we need.
		 * The packet length includes FCS, but we don't want to
		 * include that when passing upstream as it messes up
		 * bridging applications.
		 */
		skb = dev_alloc_skb(pkt_len - 4 + NET_IP_ALIGN);

		if (unlikely(!skb)) {
			printk("%s: Memory squeeze, dropping packet.\n",
					dev->name);
			dev->stats.rx_dropped++;
		} else {
			skb_reserve(skb, NET_IP_ALIGN);
			skb_put(skb, pkt_len - 4);	/* Make room */
			skb_copy_to_linear_data(skb, data, pkt_len - 4);
			/* 1588 messeage TS handle */
			if (fep->ptimer_present)
				fec_ptp_store_rxstamp(fpp, skb, bdp);
			skb->protocol = eth_type_trans(skb, dev);
			netif_rx(skb);
		}

        	bdp->cbd_bufaddr = dma_map_single(NULL, data, bdp->cbd_datlen,
			DMA_FROM_DEVICE);
rx_processing_done:
		/* Clear the status flags for this buffer */
		status &= ~BD_ENET_RX_STATS;

		/* Mark the buffer empty */
		status |= BD_ENET_RX_EMPTY;
		bdp->cbd_sc = status;
#ifdef CONFIG_ENHANCED_BD
		bdp->cbd_esc = BD_ENET_RX_INT;
		bdp->cbd_prot = 0;
		bdp->cbd_bdu = 0;
#endif

		/* Update BD pointer to next entry */
		if (status & BD_ENET_RX_WRAP)
			bdp = fep->rx_bd_base;
		else
			bdp++;
		/* Doing this here will keep the FEC running while we process
		 * incoming frames.  On a heavily loaded network, we should be
		 * able to keep up at the expense of system resources.
		 */
		writel(0, fep->hwp + FEC_R_DES_ACTIVE);
	}
	fep->cur_rx = bdp;

	spin_unlock(&fep->hw_lock);
}

/* ------------------------------------------------------------------------- */
static void __inline__ fec_get_mac(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	unsigned char *iap, tmpaddr[ETH_ALEN];
	static int index;

#ifdef CONFIG_M5272
	if (FEC_FLASHMAC) {
		/*
		 * Get MAC address from FLASH.
		 * If it is all 1's or 0's, use the default.
		 */
		iap = (unsigned char *)FEC_FLASHMAC;
		if ((iap[0] == 0) && (iap[1] == 0) && (iap[2] == 0) &&
		    (iap[3] == 0) && (iap[4] == 0) && (iap[5] == 0))
			iap = fec_mac_default;
		if ((iap[0] == 0xff) && (iap[1] == 0xff) && (iap[2] == 0xff) &&
		    (iap[3] == 0xff) && (iap[4] == 0xff) && (iap[5] == 0xff))
			iap = fec_mac_default;
	}
#else
	if (is_valid_ether_addr(fec_mac_default)) {
		iap = fec_mac_default;
	}
#endif
	else {
		*((unsigned long *) &tmpaddr[0]) = be32_to_cpu(readl(fep->hwp + FEC_ADDR_LOW));
		*((unsigned short *) &tmpaddr[4]) = be16_to_cpu(readl(fep->hwp + FEC_ADDR_HIGH) >> 16);
		iap = &tmpaddr[0];
	}

	memcpy(dev->dev_addr, iap, ETH_ALEN);

	/* Adjust MAC if using default MAC address */
	if (iap == fec_mac_default) {
		dev->dev_addr[ETH_ALEN-1] = fec_mac_default[ETH_ALEN-1] + index;
		index++;
}
}

/* ------------------------------------------------------------------------- */

/*
 * Phy section
 */
static void fec_enet_adjust_link(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct phy_device *phy_dev = fep->phy_dev;
	unsigned long flags;

	int status_change = 0;

	spin_lock_irqsave(&fep->hw_lock, flags);

	/* Prevent a state halted on mii error */
	if (fep->mii_timeout && phy_dev->state == PHY_HALTED) {
		phy_dev->state = PHY_RESUMING;
		goto spin_unlock;
	}

	/* Duplex link change */
	if (phy_dev->link) {
		if (fep->full_duplex != phy_dev->duplex) {
			fec_restart(dev, phy_dev->duplex);
			status_change = 1;
		}
	}

	/* Link on or off change */
	if (phy_dev->link != fep->link) {
		fep->link = phy_dev->link;
		if (phy_dev->link) {
			fec_restart(dev, phy_dev->duplex);

			/* if link becomes up and tx be stopped, start it */
			if (netif_queue_stopped(dev)) {
				netif_start_queue(dev);
				netif_wake_queue(dev);
			}
		}
		else
			fec_stop(dev);
		status_change = 1;
	}

spin_unlock:
	spin_unlock_irqrestore(&fep->hw_lock, flags);

	if (status_change)
		phy_print_status(phy_dev);
}

static int fec_enet_mdio_read(struct mii_bus *bus, int mii_id, int regnum)
{
	struct fec_enet_private *fep = bus->priv;
	unsigned long time_left;

	/* This is required to force the driver to think that the non-existant
	 * PHY has link, only when boards use the Marvell switch
	 */
	if(mii_id == 0x18) {
		switch(regnum) {
			case 0x0: return 0x3100;
			case 0x1: return 0x782d;
			case 0x4: return 0x1e1;
			case 0x5: return 0xc5e1;
		}
	}

	fep->mii_timeout = 0;
	init_completion(&fep->mdio_done);

	/* start a read op */
	writel(FEC_MMFR_ST | FEC_MMFR_OP_READ |
		FEC_MMFR_PA(mii_id) | FEC_MMFR_RA(regnum) |
		FEC_MMFR_TA, fep->hwp + FEC_MII_DATA);

	/* wait for end of transfer */
	time_left = wait_for_completion_timeout(&fep->mdio_done,
		usecs_to_jiffies(FEC_MII_TIMEOUT));
	if (time_left == 0) {
		fep->mii_timeout = 1;
		printk(KERN_ERR "FEC: MDIO read timeout\n");
		return -ETIMEDOUT;
	}
	/* return value */
	return FEC_MMFR_DATA(readl(fep->hwp + FEC_MII_DATA));
}

static int fec_enet_mdio_write(struct mii_bus *bus, int mii_id, int regnum,
			   u16 value)
{
	struct fec_enet_private *fep = bus->priv;
	unsigned long time_left;

	/* If this is a switch, we want to just exit and not do a write */
	if(mii_id == 0x18) return 0;

	fep->mii_timeout = 0;
	init_completion(&fep->mdio_done);

	/* start a write op */
	writel(FEC_MMFR_ST | FEC_MMFR_OP_WRITE |
		FEC_MMFR_PA(mii_id) | FEC_MMFR_RA(regnum) |
		FEC_MMFR_TA | FEC_MMFR_DATA(value),
		fep->hwp + FEC_MII_DATA);

	/* wait for end of transfer */
	time_left = wait_for_completion_timeout(&fep->mdio_done,
		usecs_to_jiffies(FEC_MII_TIMEOUT));
	if (time_left == 0) {
		fep->mii_timeout = 1;
		printk(KERN_ERR "FEC: MDIO write timeout\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int fec_enet_mdio_reset(struct mii_bus *bus)
{
	return 0;
}

static int fec_enet_mii_probe(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct phy_device *phy_dev = NULL;
	int phy_addr;
	int fec_index = fep->pdev->id > 0 ? fep->pdev->id : 0;

	fep->phy_dev = NULL;

	/* find the phy, assuming fec index corresponds to addr */
	for (phy_addr = 0; phy_addr < PHY_MAX_ADDR; phy_addr++) {
		if (fep->mii_bus->phy_map[phy_addr]) {
			if (fec_index--)
				continue;
			phy_dev = fep->mii_bus->phy_map[phy_addr];
			break;
		}
	}

	if(phy_addr == 0x18)
	  hasphy = 0;

	if (!phy_dev) {
		printk(KERN_ERR "%s: no PHY found\n", dev->name);
		return -ENODEV;
	}

	/* attach the mac to the phy */
	phy_dev = phy_connect(dev, dev_name(&phy_dev->dev),
			     &fec_enet_adjust_link, 0,
			     fep->phy_interface);
	if (IS_ERR(phy_dev)) {
		printk(KERN_ERR "%s: Could not attach to PHY\n", dev->name);
		return PTR_ERR(phy_dev);
	}

	/* mask with MAC supported features */
	phy_dev->supported &= PHY_BASIC_FEATURES;
	phy_dev->advertising = phy_dev->supported;

	fep->phy_dev = phy_dev;
	fep->link = 0;
	fep->full_duplex = 0;

	printk(KERN_INFO "%s: Freescale FEC PHY driver [%s] "
		"(mii_bus:phy_addr=%s, irq=%d)\n", dev->name,
		fep->phy_dev->drv->name, dev_name(&fep->phy_dev->dev),
		fep->phy_dev->irq);

	return 0;
}

static struct mii_bus *fec_enet_mii_init(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct fec_enet_private *fep = netdev_priv(dev);
	int err = -ENXIO, i;

	fep->mii_timeout = 0;

	/*
	 * Set MII speed to 2.5 MHz (= clk_get_rate() / 2 * phy_speed)
	 */
	fep->phy_speed = DIV_ROUND_UP(clk_get_rate(fep->clk), 5000000) << 1;
#ifdef CONFIG_ARCH_MXS
	/* Can't get phy(8720) ID when set to 2.5M on MX28, lower it*/
	fep->phy_speed <<= 2;
#endif
	writel(fep->phy_speed, fep->hwp + FEC_MII_SPEED);

	fep->mii_bus = mdiobus_alloc();
	if (fep->mii_bus == NULL) {
		err = -ENOMEM;
		goto err_out;
	}

	fep->mii_bus->name = "fec_enet_mii_bus";
	fep->mii_bus->read = fec_enet_mdio_read;
	fep->mii_bus->write = fec_enet_mdio_write;
	fep->mii_bus->reset = fec_enet_mdio_reset;
	snprintf(fep->mii_bus->id, MII_BUS_ID_SIZE, "%x", pdev->id);
	fep->mii_bus->priv = fep;
	fep->mii_bus->parent = &pdev->dev;

	fep->mii_bus->irq = kmalloc(sizeof(int) * PHY_MAX_ADDR, GFP_KERNEL);
	if (!fep->mii_bus->irq) {
		err = -ENOMEM;
		goto err_out_free_mdiobus;
	}

	for (i = 0; i < PHY_MAX_ADDR; i++)
		fep->mii_bus->irq[i] = PHY_POLL;

	platform_set_drvdata(dev, fep->mii_bus);

	if (mdiobus_register(fep->mii_bus))
		goto err_out_free_mdio_irq;

	return fep->mii_bus;

err_out_free_mdio_irq:
	kfree(fep->mii_bus->irq);
err_out_free_mdiobus:
	mdiobus_free(fep->mii_bus);
err_out:
	return ERR_PTR(err);
}

static void fec_enet_mii_remove(struct fec_enet_private *fep)
{
	if (fep->phy_dev)
		phy_disconnect(fep->phy_dev);
	mdiobus_unregister(fep->mii_bus);
	kfree(fep->mii_bus->irq);
	mdiobus_free(fep->mii_bus);
}

static int fec_enet_get_settings(struct net_device *dev,
				  struct ethtool_cmd *cmd)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct phy_device *phydev = fep->phy_dev;

	if (!phydev)
		return -ENODEV;

	return phy_ethtool_gset(phydev, cmd);
}

static int fec_enet_set_settings(struct net_device *dev,
				 struct ethtool_cmd *cmd)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct phy_device *phydev = fep->phy_dev;

	if (!phydev)
		return -ENODEV;

	return phy_ethtool_sset(phydev, cmd);
}

static void fec_enet_get_drvinfo(struct net_device *dev,
				 struct ethtool_drvinfo *info)
{
	struct fec_enet_private *fep = netdev_priv(dev);

	strcpy(info->driver, fep->pdev->dev.driver->name);
	strcpy(info->version, "Revision: 1.0");
	strcpy(info->bus_info, dev_name(&dev->dev));
}

static struct ethtool_ops fec_enet_ethtool_ops = {
	.get_settings		= fec_enet_get_settings,
	.set_settings		= fec_enet_set_settings,
	.get_drvinfo		= fec_enet_get_drvinfo,
	.get_link		= ethtool_op_get_link,
};

static int fec_enet_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct fec_ptp_private *priv = fep->ptp_priv;
	struct phy_device *phydev = fep->phy_dev;
	int retVal = 0;

	if (!netif_running(dev))
		return -EINVAL;

	if (!phydev)
		return -ENODEV;

	if ((cmd >= PTP_ENBL_TXTS_IOCTL) &&
			(cmd <= PTP_FLUSH_TIMESTAMP)) {
		if (fep->ptimer_present)
			retVal = fec_ptp_ioctl(priv, rq, cmd);
		else
			retVal = -ENODEV;
	} else
		retVal = phy_mii_ioctl(phydev, rq, cmd);

	return retVal;
}

static void fec_enet_free_buffers(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	int i;
	struct sk_buff *skb;
	struct bufdesc	*bdp;

	bdp = fep->rx_bd_base;
	for (i = 0; i < RX_RING_SIZE; i++) {
		skb = fep->rx_skbuff[i];

		if (bdp->cbd_bufaddr)
			dma_unmap_single(&dev->dev, bdp->cbd_bufaddr,
					FEC_ENET_RX_FRSIZE, DMA_FROM_DEVICE);
		if (skb)
			dev_kfree_skb(skb);
		bdp++;
	}

	bdp = fep->tx_bd_base;
	for (i = 0; i < TX_RING_SIZE; i++)
		kfree(fep->tx_bounce[i]);
}

static int fec_enet_alloc_buffers(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	int i;
	struct sk_buff *skb;
	struct bufdesc	*bdp;

	bdp = fep->rx_bd_base;
	for (i = 0; i < RX_RING_SIZE; i++) {
		skb = dev_alloc_skb(FEC_ENET_RX_FRSIZE);
		if (!skb) {
			fec_enet_free_buffers(dev);
			return -ENOMEM;
		}
		fep->rx_skbuff[i] = skb;

		bdp->cbd_bufaddr = dma_map_single(&dev->dev, skb->data,
				FEC_ENET_RX_FRSIZE, DMA_FROM_DEVICE);
		bdp->cbd_sc = BD_ENET_RX_EMPTY;
#ifdef CONFIG_ENHANCED_BD
		bdp->cbd_esc = BD_ENET_RX_INT;
#endif
		bdp++;
	}

	/* Set the last buffer to wrap. */
	bdp--;
	bdp->cbd_sc |= BD_SC_WRAP;

	bdp = fep->tx_bd_base;
	for (i = 0; i < TX_RING_SIZE; i++) {
		fep->tx_bounce[i] = kmalloc(FEC_ENET_TX_FRSIZE, GFP_KERNEL);

		bdp->cbd_sc = 0;
		bdp->cbd_bufaddr = 0;
#ifdef CONFIG_ENHANCED_BD
		bdp->cbd_esc = BD_ENET_TX_INT;
#endif
		bdp++;
	}

	/* Set the last buffer to wrap. */
	bdp--;
	bdp->cbd_sc |= BD_SC_WRAP;

	return 0;
}

static int
fec_enet_open(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	int ret;

	/* I should reset the ring buffers here, but I don't yet know
	 * a simple way to do that.
	 */
	clk_enable(fep->clk);
	ret = fec_enet_alloc_buffers(dev);
	if (ret)
		return ret;

	/* Probe and connect to PHY when open the interface */
	ret = fec_enet_mii_probe(dev);
	if (ret) {
		fec_enet_free_buffers(dev);
		return ret;
	}
	phy_start(fep->phy_dev);
	fec_restart(dev, fep->phy_dev->duplex);
	fep->opened = 1;
	return 0;
}

static int
fec_enet_close(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);

	/* Don't know what to do yet. */
	fep->opened = 0;
	fec_stop(dev);

	if (fep->phy_dev) {
		phy_stop(fep->phy_dev);
		phy_disconnect(fep->phy_dev);
	}
        fec_enet_free_buffers(dev);
	clk_disable(fep->clk);

	return 0;
}

/* Set or clear the multicast filter for this adaptor.
 * Skeleton taken from sunlance driver.
 * The CPM Ethernet implementation allows Multicast as well as individual
 * MAC address filtering.  Some of the drivers check to make sure it is
 * a group multicast address, and discard those that are not.  I guess I
 * will do the same for now, but just remove the test if you want
 * individual filtering as well (do the upper net layers want or support
 * this kind of feature?).
 */

#define HASH_BITS	6		/* #bits in hash */
#define CRC32_POLY	0xEDB88320

static void set_multicast_list(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct netdev_hw_addr *ha;
	unsigned int i, bit, data, crc, tmp;
	unsigned char hash;

	if (dev->flags & IFF_PROMISC) {
		tmp = readl(fep->hwp + FEC_R_CNTRL);
		tmp |= 0x8;
		writel(tmp, fep->hwp + FEC_R_CNTRL);
		return;
	}

	tmp = readl(fep->hwp + FEC_R_CNTRL);
	tmp &= ~0x8;
	writel(tmp, fep->hwp + FEC_R_CNTRL);

	if (dev->flags & IFF_ALLMULTI) {
		/* Catch all multicast addresses, so set the
		 * filter to all 1's
		 */
		writel(0xffffffff, fep->hwp + FEC_GRP_HASH_TABLE_HIGH);
		writel(0xffffffff, fep->hwp + FEC_GRP_HASH_TABLE_LOW);

		return;
	}

	/* Clear filter and add the addresses in hash register
	 */
	writel(0, fep->hwp + FEC_GRP_HASH_TABLE_HIGH);
	writel(0, fep->hwp + FEC_GRP_HASH_TABLE_LOW);

	netdev_for_each_mc_addr(ha, dev) {
		/* Only support group multicast for now */
		if (!(ha->addr[0] & 1))
			continue;

		/* calculate crc32 value of mac address */
		crc = 0xffffffff;

		for (i = 0; i < dev->addr_len; i++) {
			data = ha->addr[i];
			for (bit = 0; bit < 8; bit++, data >>= 1) {
				crc = (crc >> 1) ^
				(((crc ^ data) & 1) ? CRC32_POLY : 0);
			}
		}

		/* only upper 6 bits (HASH_BITS) are used
		 * which point to specific bit in he hash registers
		 */
		hash = (crc >> (32 - HASH_BITS)) & 0x3f;

		if (hash > 31) {
			tmp = readl(fep->hwp + FEC_GRP_HASH_TABLE_HIGH);
			tmp |= 1 << (hash - 32);
			writel(tmp, fep->hwp + FEC_GRP_HASH_TABLE_HIGH);
		} else {
			tmp = readl(fep->hwp + FEC_GRP_HASH_TABLE_LOW);
			tmp |= 1 << hash;
			writel(tmp, fep->hwp + FEC_GRP_HASH_TABLE_LOW);
		}
	}
}

/* Set a MAC change in hardware. */
static int
fec_set_mac_address(struct net_device *dev, void *p)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);

	writel(dev->dev_addr[3] | (dev->dev_addr[2] << 8) |
		(dev->dev_addr[1] << 16) | (dev->dev_addr[0] << 24),
		fep->hwp + FEC_ADDR_LOW);
	writel((dev->dev_addr[5] << 16) | (dev->dev_addr[4] << 24),
		fep->hwp + FEC_ADDR_HIGH);
	return 0;
}

static const struct net_device_ops fec_netdev_ops = {
	.ndo_open		= fec_enet_open,
	.ndo_stop		= fec_enet_close,
	.ndo_start_xmit		= fec_enet_start_xmit,
	.ndo_set_multicast_list = set_multicast_list,
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_tx_timeout		= fec_timeout,
	.ndo_set_mac_address	= fec_set_mac_address,
	.ndo_do_ioctl           = fec_enet_ioctl,
};

static int fec_mac_addr_setup(char *mac_addr)
{
	char *ptr, *p = mac_addr;
	unsigned long tmp;
	int i = 0, ret = 0;

	while (p && (*p) && i < 6) {
		ptr = strchr(p, ':');
		if (ptr)
			*ptr++ = '\0';

		if (strlen(p)) {
			ret = strict_strtoul(p, 16, &tmp);
			if (ret < 0 || tmp > 0xff)
				break;
			fec_mac_default[i++] = tmp;
		}
		p = ptr;
	}

	return 0;
}

__setup("fec_mac=", fec_mac_addr_setup);

 /*
  * XXX:  We need to clean up on failure exits here.
  *
  * index is only used in legacy code
  */
static int fec_enet_init(struct net_device *dev, int index)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct bufdesc *cbd_base;
	struct bufdesc *bdp;
	int i;

	/* Allocate memory for buffer descriptors. */
	cbd_base = dma_alloc_coherent(NULL, PAGE_SIZE, &fep->bd_dma,
			GFP_KERNEL);
	if (!cbd_base) {
		printk("FEC: allocate descriptor memory failed?\n");
		return -ENOMEM;
	}

	spin_lock_init(&fep->hw_lock);

	fep->index = index;
	fep->hwp = (void __iomem *)dev->base_addr;
	fep->netdev = dev;

	/* Set the Ethernet address */
	fec_get_mac(dev);

	/* Set receive and transmit descriptor base. */
	fep->rx_bd_base = cbd_base;
	fep->tx_bd_base = cbd_base + RX_RING_SIZE;

	/* The FEC Ethernet specific entries in the device structure */
	dev->watchdog_timeo = TX_TIMEOUT;
	dev->netdev_ops = &fec_netdev_ops;
	dev->ethtool_ops = &fec_enet_ethtool_ops;

	/* Initialize the receive buffer descriptors. */
	bdp = fep->rx_bd_base;
	for (i = 0; i < RX_RING_SIZE; i++) {

		/* Initialize the BD for every fragment in the page. */
		bdp->cbd_sc = 0;
		bdp++;
	}

	/* Set the last buffer to wrap */
	bdp--;
	bdp->cbd_sc |= BD_SC_WRAP;

	/* ...and the same for transmit */
	bdp = fep->tx_bd_base;
	for (i = 0; i < TX_RING_SIZE; i++) {

		/* Initialize the BD for every fragment in the page. */
		bdp->cbd_sc = 0;
		bdp->cbd_bufaddr = 0;
		bdp++;
	}

	/* Set the last buffer to wrap */
	bdp--;
	bdp->cbd_sc |= BD_SC_WRAP;

	fec_restart(dev, 0);

	return 0;
}

/* This function is called to start or restart the FEC during a link
 * change.  This only happens when switching between half and full
 * duplex.
 */
static void
fec_restart(struct net_device *dev, int duplex)
{
	struct fec_enet_private *fep = netdev_priv(dev);
	struct fec_platform_data *pdata = fep->pdev->dev.platform_data;
	int i;
	uint ret = 0;
	u32 temp_mac[2];
	unsigned long reg;
	int val;

#ifdef CONFIG_ARCH_MXS
	if (pdata && pdata->init && hasphy)
		ret = pdata->init();
#endif

	/* Whack a reset.  We should wait for this. */
	writel(1, fep->hwp + FEC_ECNTRL);
	udelay(10);

	/* Reset fec will reset MAC to zero, reconfig it again */
	memcpy(&temp_mac, dev->dev_addr, ETH_ALEN);
	writel(cpu_to_be32(temp_mac[0]), fep->hwp + FEC_ADDR_LOW);
	writel(cpu_to_be32(temp_mac[1]), fep->hwp + FEC_ADDR_HIGH);

	/* Clear any outstanding interrupt. */
	writel(0xffc00000, fep->hwp + FEC_IEVENT);

	/* Reset all multicast.	*/
	writel(0, fep->hwp + FEC_GRP_HASH_TABLE_HIGH);
	writel(0, fep->hwp + FEC_GRP_HASH_TABLE_LOW);
#ifndef CONFIG_M5272
	writel(0, fep->hwp + FEC_HASH_TABLE_HIGH);
	writel(0, fep->hwp + FEC_HASH_TABLE_LOW);
#endif

	/* Set maximum receive buffer size. */
	writel(PKT_MAXBLR_SIZE, fep->hwp + FEC_R_BUFF_SIZE);

	/* Set receive and transmit descriptor base. */
	writel(fep->bd_dma, fep->hwp + FEC_R_DES_START);
	writel((unsigned long)fep->bd_dma + sizeof(struct bufdesc) * RX_RING_SIZE,
			fep->hwp + FEC_X_DES_START);

	fep->dirty_tx = fep->cur_tx = fep->tx_bd_base;
	fep->cur_rx = fep->rx_bd_base;

	/* Reset SKB transmit buffers. */
	fep->skb_cur = fep->skb_dirty = 0;
	for (i = 0; i <= TX_RING_MOD_MASK; i++) {
		if (fep->tx_skbuff[i]) {
			dev_kfree_skb_any(fep->tx_skbuff[i]);
			fep->tx_skbuff[i] = NULL;
		}
	}

	/* Enable MII mode */
	if (duplex) {
		/* MII enable / FD enable */
		writel(OPT_FRAME_SIZE | 0x04, fep->hwp + FEC_R_CNTRL);
		writel(0x04, fep->hwp + FEC_X_CNTRL);
	} else {
		/* MII enable / No Rcv on Xmit */
		writel(OPT_FRAME_SIZE | 0x06, fep->hwp + FEC_R_CNTRL);
		writel(0x0, fep->hwp + FEC_X_CNTRL);
	}
	fep->full_duplex = duplex;

#ifdef CONFIG_ARCH_MXS

	reg = readl(fep->hwp + FEC_R_CNTRL);

	/* Enable flow control and length check */
	reg |= (0x40000000 | 0x00000020);

	/* Check MII or RMII */
	if (fep->phy_interface == PHY_INTERFACE_MODE_RMII)
		reg |= 0x00000100;
	else
		reg &= ~0x00000100;

	/* Check 10M or 100M */
	if (fep->phy_dev && fep->phy_dev->speed == SPEED_100)
		reg &= ~0x00000200; /* 100M */
	else
		reg |= 0x00000200;  /* 10M */

	writel(reg, fep->hwp + FEC_R_CNTRL);

#endif
	/* Set MII speed */
	writel(fep->phy_speed, fep->hwp + FEC_MII_SPEED);

	if (fep->ptimer_present) {
		/* Set Timer count */
		ret = fec_ptp_start(fep->ptp_priv);
		if (ret) {
			fep->ptimer_present = 0;
			reg = 0x0;
		} else
#ifdef CONFIG_ARCH_MX28
			reg = 0x00000010;
#else
			reg = 0x0;
#endif
	} else
		reg = 0x0;

#ifdef FEC_MIIGSK_ENR
	if (fep->phy_interface == PHY_INTERFACE_MODE_RMII) {
		/* disable the gasket and wait */
		writel(0, fep->hwp + FEC_MIIGSK_ENR);
		while (readl(fep->hwp + FEC_MIIGSK_ENR) & 4)
			udelay(1);

		/* configure the gasket: RMII, 50 MHz, no loopback, no echo */
		val = 1;
		if (fep->phy_dev && fep->phy_dev->speed == SPEED_10)
			val |= 1 << 6;
		writel(val, fep->hwp + FEC_MIIGSK_CFGR);

		/* re-enable the gasket */
		writel(2, fep->hwp + FEC_MIIGSK_ENR);
	}
#endif

	/* And last, enable the transmit and receive processing */
	reg |= 0x00000002;
	writel(reg, fep->hwp + FEC_ECNTRL);
	writel(0, fep->hwp + FEC_R_DES_ACTIVE);

	/* Enable interrupts we wish to service */
	writel(FEC_DEFAULT_IMASK, fep->hwp + FEC_IMASK);
}

static void
fec_stop(struct net_device *dev)
{
	struct fec_enet_private *fep = netdev_priv(dev);

	/* We cannot expect a graceful transmit stop without link !!! */
	if (fep->link) {
		writel(1, fep->hwp + FEC_X_CNTRL); /* Graceful transmit stop */
		udelay(10);
		if (!(readl(fep->hwp + FEC_IEVENT) & FEC_ENET_GRA))
			printk("fec_stop : Graceful transmit stop did not complete !\n");
	}

	/* Whack a reset.  We should wait for this. */
	writel(1, fep->hwp + FEC_ECNTRL);
	udelay(10);

#ifdef CONFIG_ARCH_MXS
	/* FIXME: we have to enable enet to keep mii interrupt works. */
	writel((0x1 << 1), fep->hwp + FEC_ECNTRL);

	/* Check MII or RMII */
	if (fep->phy_interface == PHY_INTERFACE_MODE_RMII)
		writel(readl(fep->hwp + FEC_R_CNTRL) | 0x100,
					fep->hwp + FEC_R_CNTRL);
	else
		writel(readl(fep->hwp + FEC_R_CNTRL) & ~0x100,
					fep->hwp + FEC_R_CNTRL);
#endif
	/* Clear outstanding MII command interrupts. */
	writel(FEC_ENET_MII, fep->hwp + FEC_IEVENT);
	writel(fep->phy_speed, fep->hwp + FEC_MII_SPEED);
	if (fep->ptimer_present)
		fec_ptp_stop(fep->ptp_priv);
	writel(FEC_DEFAULT_IMASK, fep->hwp + FEC_IMASK);

	netif_stop_queue(dev);
	fep->link = 0;
}

static int __devinit
fec_probe(struct platform_device *pdev)
{
	struct fec_enet_private *fep;
	struct fec_platform_data *pdata;
	struct net_device *ndev;
	int i, irq, ret = 0;
	struct resource *r;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r)
		return -ENXIO;

	r = request_mem_region(r->start, resource_size(r), pdev->name);
	if (!r)
		return -EBUSY;

	/* Init network device */
	ndev = alloc_etherdev(sizeof(struct fec_enet_private));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, &pdev->dev);

	/* setup board info structure */
	fep = netdev_priv(ndev);
	memset(fep, 0, sizeof(*fep));

	ndev->base_addr = (unsigned long)ioremap(r->start, resource_size(r));
	fep->pdev = pdev;

	if (!ndev->base_addr) {
		ret = -ENOMEM;
		goto failed_ioremap;
	}

	platform_set_drvdata(pdev, ndev);

	pdata = pdev->dev.platform_data;
	if (pdata)
		fep->phy_interface = pdata->phy;

	if (pdata && !is_valid_ether_addr(fec_mac_default))
		memcpy(fec_mac_default, pdata->mac, sizeof(fec_mac_default));

	/* This device has up to three irqs on some platforms */
	for (i = 0; i < 3; i++) {
		irq = platform_get_irq(pdev, i);
		if (i && irq < 0)
			break;
		ret = request_irq(irq, fec_enet_interrupt, IRQF_DISABLED, pdev->name, ndev);
		if (ret) {
			while (i >= 0) {
				irq = platform_get_irq(pdev, i);
				free_irq(irq, ndev);
				i--;
			}
			goto failed_irq;
		}
	}

	fep->clk = clk_get(&pdev->dev, "fec_clk");
	if (IS_ERR(fep->clk)) {
		ret = PTR_ERR(fep->clk);
		goto failed_clk;
	}
	clk_enable(fep->clk);

	/* PHY reset should be done during clock on */
	if (pdata && pdata->init && hasphy)
		ret = pdata->init();
	if (ret)
		goto failed_platform_init;
	/*
	 * The priority for getting MAC address is:
	 * (1) kernel command line fec_mac = xx:xx:xx...
	 * (2) platform data mac field got from fuse etc
	 * (3) bootloader set the FEC mac register
	 */

	if (pdata && !is_valid_ether_addr(fec_mac_default) &&
		pdata->mac && is_valid_ether_addr(pdata->mac))
		memcpy(fec_mac_default, pdata->mac,
					sizeof(fec_mac_default));

	ret = fec_enet_init(ndev, 0);
	if (ret)
		goto failed_init;

	if (pdev->id == 0) {
		fec_mii_bus = fec_enet_mii_init(pdev);
		if (IS_ERR(fec_mii_bus)) {
			ret = -ENOMEM;
		goto failed_mii_init;
		}
	} else {
		fep->mii_bus = fec_mii_bus;
	}

	if (fec_ptp_malloc_priv(&(fep->ptp_priv))) {
		if (fep->ptp_priv) {
			fep->ptp_priv->hwp = fep->hwp;
			ret = fec_ptp_init(fep->ptp_priv, pdev->id);
			if (ret)
				printk(KERN_WARNING
				       "IEEE1588: ptp-timer is unavailable\n");
			else
				fep->ptimer_present = 1;
		} else
			printk(KERN_ERR "IEEE1588: failed to malloc memory\n");
	}

	ret = register_netdev(ndev);
	if (ret)
		goto failed_register;

	clk_disable(fep->clk);

	return 0;

failed_register:
	fec_enet_mii_remove(fep);
	if (fep->ptimer_present)
		fec_ptp_cleanup(fep->ptp_priv);
	kfree(fep->ptp_priv);
failed_mii_init:
failed_init:
	clk_disable(fep->clk);
	clk_put(fep->clk);
failed_clk:
	for (i = 0; i < 3; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq > 0)
			free_irq(irq, ndev);
	}
failed_irq:
	iounmap((void __iomem *)ndev->base_addr);
failed_ioremap:
failed_platform_init:
	free_netdev(ndev);

	return ret;
}

static int __devexit
fec_drv_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct fec_enet_private *fep = netdev_priv(ndev);
	struct fec_platform_data *pdata = pdev->dev.platform_data;

	platform_set_drvdata(pdev, NULL);

	fec_stop(ndev);
	fec_enet_mii_remove(fep);
	if (pdata && pdata->uninit)
		pdata->uninit();
	clk_disable(fep->clk);
	clk_put(fep->clk);
	iounmap((void __iomem *)ndev->base_addr);
	if (fep->ptimer_present)
		fec_ptp_cleanup(fep->ptp_priv);
	kfree(fep->ptp_priv);
	unregister_netdev(ndev);
	free_netdev(ndev);
	return 0;
}

static int
fec_suspend(struct platform_device *dev, pm_message_t state)
{
	struct net_device *ndev = platform_get_drvdata(dev);
	struct fec_enet_private *fep;

	if (ndev) {
		fep = netdev_priv(ndev);
		if (netif_running(ndev)) {
			netif_device_detach(ndev);
			fec_stop(ndev);
			netif_carrier_off(ndev);
			clk_disable(fep->clk);
		}
	}
	return 0;
}

#ifdef CONFIG_ARCH_MXS

suspend_state_t mxs_pm_get_target(void);

#endif


static int
fec_resume(struct platform_device *dev)
{
	struct net_device *ndev = platform_get_drvdata(dev);
  struct fec_platform_data *pdata;
	struct fec_enet_private *fep;

	if (ndev) {
		fep = netdev_priv(ndev);
    pdata = fep->pdev->dev.platform_data;
		if (netif_running(ndev)) {
			clk_enable(fep->clk);
			fec_restart(ndev, fep->full_duplex);
			netif_device_attach(ndev);
		}
    else {
#ifdef CONFIG_ARCH_MXS
			if (mxs_pm_get_target() == PM_SUSPEND_MEM)  {
				clk_enable(fep->clk);
				/* PHY reset should be done during clock on */
				if (pdata && pdata->init && hasphy)
					pdata->init();
				fec_restart(ndev, 0);
				clk_disable(fep->clk);
      }
#endif
    }
	}
	return 0;
}

static struct platform_driver fec_driver = {
	.driver	= {
		.name    = "fec",
		.owner	 = THIS_MODULE,
	},
	.probe   = fec_probe,
	.remove  = __devexit_p(fec_drv_remove),
	.suspend = fec_suspend,
	.resume  = fec_resume,
};

static int __init
fec_enet_module_init(void)
{
	printk(KERN_INFO "FEC Ethernet Driver\n");

	return platform_driver_register(&fec_driver);
}

static void __exit
fec_enet_cleanup(void)
{
	platform_driver_unregister(&fec_driver);
}

module_exit(fec_enet_cleanup);
module_init(fec_enet_module_init);

MODULE_LICENSE("GPL");
