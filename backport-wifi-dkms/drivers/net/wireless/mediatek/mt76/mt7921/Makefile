# SPDX-License-Identifier: ISC

obj-$(CPTCFG_MT7921_COMMON) += mt7921-common.o
obj-$(CPTCFG_MT7921E) += mt7921e.o
obj-$(CPTCFG_MT7921S) += mt7921s.o
obj-$(CPTCFG_MT7921U) += mt7921u.o

CFLAGS_trace.o := -I$(src)

mt7921-common-y := mac.o mcu.o main.o init.o debugfs.o trace.o
mt7921-common-$(CPTCFG_NL80211_TESTMODE) += testmode.o
mt7921e-y := pci.o pci_mac.o pci_mcu.o dma.o
mt7921s-y := sdio.o sdio_mac.o sdio_mcu.o
mt7921u-y := usb.o usb_mac.o
