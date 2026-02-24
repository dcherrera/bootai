/*
 * iwn_hw.c — Intel Wireless N hardware access for BootAI (Phase 1+2)
 *
 * MMIO primitives, NIC lock, APM init/stop, EEPROM read, NIC config,
 * and iwn_attach(). All register access goes through
 * EFI_PCI_IO_PROTOCOL.Mem.Read/Write on BAR0.
 *
 * Reference: OpenBSD sys/dev/pci/if_iwn.c (ISC license)
 */

#include "iwn_internal.h"

/* ---- MMIO primitives ---- */

uint32_t iwn_read(iwn_dev *dev, uint32_t reg) {
    uint32_t val;
    dev->pci_io->Mem.Read(dev->pci_io, EfiPciIoWidthUint32, 0, reg, 1, &val);
    return val;
}

void iwn_write(iwn_dev *dev, uint32_t reg, uint32_t val) {
    dev->pci_io->Mem.Write(dev->pci_io, EfiPciIoWidthUint32, 0, reg, 1, &val);
}

void iwn_write_1(iwn_dev *dev, uint32_t reg, uint8_t val) {
    dev->pci_io->Mem.Write(dev->pci_io, EfiPciIoWidthUint8, 0, reg, 1, &val);
}

void iwn_setbits(iwn_dev *dev, uint32_t reg, uint32_t mask) {
    iwn_write(dev, reg, iwn_read(dev, reg) | mask);
}

void iwn_clrbits(iwn_dev *dev, uint32_t reg, uint32_t mask) {
    iwn_write(dev, reg, iwn_read(dev, reg) & ~mask);
}

/* ---- Peripheral (NIC-internal) register access ---- */

uint32_t iwn_prph_read(iwn_dev *dev, uint32_t addr) {
    iwn_write(dev, IWN_PRPH_RADDR, IWN_PRPH_DWORD | addr);
    /* Compiler barrier — ensure address write commits before data read */
    __asm__ volatile("" ::: "memory");
    return iwn_read(dev, IWN_PRPH_RDATA);
}

void iwn_prph_write(iwn_dev *dev, uint32_t addr, uint32_t val) {
    iwn_write(dev, IWN_PRPH_WADDR, IWN_PRPH_DWORD | addr);
    __asm__ volatile("" ::: "memory");
    iwn_write(dev, IWN_PRPH_WDATA, val);
}

void iwn_prph_setbits(iwn_dev *dev, uint32_t addr, uint32_t mask) {
    iwn_prph_write(dev, addr, iwn_prph_read(dev, addr) | mask);
}

void iwn_prph_clrbits(iwn_dev *dev, uint32_t addr, uint32_t mask) {
    iwn_prph_write(dev, addr, iwn_prph_read(dev, addr) & ~mask);
}

/* ---- NIC lock/unlock (required for peripheral register access) ---- */

int iwn_nic_lock(iwn_dev *dev) {
    iwn_setbits(dev, IWN_GP_CNTRL, IWN_GP_CNTRL_MAC_ACCESS_REQ);

    /* Poll until MAC_ACCESS_ENA=1 and SLEEP=0 */
    for (int i = 0; i < 1000; i++) {
        uint32_t val = iwn_read(dev, IWN_GP_CNTRL);
        if ((val & (IWN_GP_CNTRL_MAC_ACCESS_ENA | IWN_GP_CNTRL_SLEEP)) ==
            IWN_GP_CNTRL_MAC_ACCESS_ENA)
            return 0;
        gBS->Stall(10);
    }
    return -1;
}

void iwn_nic_unlock(iwn_dev *dev) {
    iwn_clrbits(dev, IWN_GP_CNTRL, IWN_GP_CNTRL_MAC_ACCESS_REQ);
}

/* ---- Clock wait ---- */

static int iwn_clock_wait(iwn_dev *dev) {
    iwn_setbits(dev, IWN_GP_CNTRL, IWN_GP_CNTRL_INIT_DONE);

    for (int i = 0; i < 2500; i++) {
        if (iwn_read(dev, IWN_GP_CNTRL) & IWN_GP_CNTRL_MAC_CLOCK_READY)
            return 0;
        gBS->Stall(10);
    }
    return -1;
}

/* ---- Hardware prepare (take NIC from Intel AMT) ---- */

static int iwn_hw_prepare(iwn_dev *dev) {
    /* Set NIC_READY */
    iwn_setbits(dev, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_NIC_READY);

    /* Poll for NIC_READY */
    for (int i = 0; i < 5; i++) {
        if (iwn_read(dev, IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_NIC_READY)
            return 0;
        gBS->Stall(10);
    }

    /* Force PREPARE */
    iwn_setbits(dev, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_PREPARE);

    /* Poll for PREPARE_DONE */
    for (int i = 0; i < 15000; i++) {
        if (iwn_read(dev, IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_PREPARE_DONE)
            break;
        gBS->Stall(10);
    }
    iwn_clrbits(dev, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_PREPARE);

    /* Retry NIC_READY */
    iwn_setbits(dev, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_NIC_READY);
    for (int i = 0; i < 5; i++) {
        if (iwn_read(dev, IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_NIC_READY)
            return 0;
        gBS->Stall(10);
    }
    return -1;
}

/* ---- Find PCIe capability offset in config space ---- */

static void iwn_find_pcie_cap(iwn_dev *dev) {
    uint8_t cap_off = 0;
    dev->pci_io->Pci.Read(dev->pci_io, EfiPciIoWidthUint8, 0x34, 1, &cap_off);

    while (cap_off) {
        uint8_t cap_id = 0;
        dev->pci_io->Pci.Read(dev->pci_io, EfiPciIoWidthUint8, cap_off, 1, &cap_id);
        if (cap_id == 0x10) { /* PCI Express */
            dev->pcie_cap_off = cap_off;
            return;
        }
        dev->pci_io->Pci.Read(dev->pci_io, EfiPciIoWidthUint8, cap_off + 1, 1, &cap_off);
    }
    dev->pcie_cap_off = 0;
}

/* ---- APM init (power-on sequence) ---- */

int iwn_apm_init(iwn_dev *dev) {
    /* GIO_CHICKEN workarounds */
    iwn_setbits(dev, IWN_GIO_CHICKEN,
                IWN_GIO_CHICKEN_DIS_L0S_TIMER | IWN_GIO_CHICKEN_L1A_NO_L0S_RX);

    /* HPET threshold */
    iwn_setbits(dev, IWN_DBG_HPET_MEM, 0xffff0000);

    /* HAP_WAKE_L1A */
    iwn_setbits(dev, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_HAP_WAKE_L1A);

    /* PCIe ASPM L1 check */
    if (dev->pcie_cap_off) {
        uint16_t link_ctrl = 0;
        dev->pci_io->Pci.Read(dev->pci_io, EfiPciIoWidthUint16,
                               dev->pcie_cap_off + 0x10, 1, &link_ctrl);
        if (link_ctrl & 0x02) /* ASPM L1 enabled */
            iwn_clrbits(dev, IWN_GIO, IWN_GIO_L0S_ENA);
        else
            iwn_setbits(dev, IWN_GIO, IWN_GIO_L0S_ENA);
    }

    /* Wait for clock */
    if (iwn_clock_wait(dev) != 0) {
        efi_print_ascii("  iwn: clock timeout\r\n");
        return -1;
    }

    /* Enable DMA clock, disable L1A */
    if (iwn_nic_lock(dev) != 0) {
        efi_print_ascii("  iwn: nic lock timeout (apm)\r\n");
        return -1;
    }
    iwn_prph_write(dev, IWN_APMG_CLK_EN, IWN_APMG_CLK_CTRL_DMA_CLK_RQT);
    gBS->Stall(20);
    iwn_prph_setbits(dev, IWN_APMG_PCI_STT, IWN_APMG_PCI_STT_L1A_DIS);
    iwn_nic_unlock(dev);

    return 0;
}

/* ---- APM stop ---- */

static void iwn_apm_stop_master(iwn_dev *dev) {
    iwn_setbits(dev, IWN_RESET, IWN_RESET_STOP_MASTER);

    for (int i = 0; i < 100; i++) {
        if (iwn_read(dev, IWN_RESET) & IWN_RESET_MASTER_DISABLED)
            return;
        gBS->Stall(10);
    }
}

void iwn_apm_stop(iwn_dev *dev) {
    iwn_apm_stop_master(dev);

    /* SW reset */
    iwn_setbits(dev, IWN_RESET, IWN_RESET_SW);

    /* Clear INIT_DONE */
    iwn_clrbits(dev, IWN_GP_CNTRL, IWN_GP_CNTRL_INIT_DONE);
}

/* ---- EEPROM lock/unlock ---- */

static int iwn_eeprom_lock(iwn_dev *dev) {
    iwn_setbits(dev, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_EEPROM_LOCKED);

    for (int i = 0; i < 100; i++) {
        if (iwn_read(dev, IWN_HW_IF_CONFIG) & IWN_HW_IF_CONFIG_EEPROM_LOCKED)
            return 0;
        gBS->Stall(10);
    }
    return -1;
}

static void iwn_eeprom_unlock(iwn_dev *dev) {
    iwn_clrbits(dev, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_EEPROM_LOCKED);
}

/* ---- OTP init (6000 series) ---- */

static int iwn_init_otprom(iwn_dev *dev) {
    if (iwn_clock_wait(dev) != 0)
        return -1;

    if (iwn_nic_lock(dev) != 0)
        return -1;

    iwn_prph_setbits(dev, IWN_APMG_PS, IWN_APMG_PS_RESET_REQ);
    gBS->Stall(5);
    iwn_prph_clrbits(dev, IWN_APMG_PS, IWN_APMG_PS_RESET_REQ);
    iwn_nic_unlock(dev);

    /* Enable auto clock gate disable for OTP access */
    iwn_setbits(dev, IWN_RESET, IWN_RESET_LINK_PWR_MGMT_DIS);

    /* Clear EEPROM_GP IF_OWNER */
    iwn_clrbits(dev, IWN_EEPROM_GP, IWN_EEPROM_GP_IF_OWNER);

    /* Clear OTP ECC error bits */
    iwn_clrbits(dev, IWN_OTP_GP,
                IWN_OTP_GP_ECC_CORR_STTS | IWN_OTP_GP_ECC_UNCORR_STTS);

    return 0;
}

/* ---- EEPROM word read ---- */

int iwn_read_prom_data(iwn_dev *dev, uint32_t addr, void *data, int count) {
    uint8_t *out = (uint8_t *)data;
    int has_otp = (dev->flags & IWN_FLAG_HAS_OTPROM);

    addr += dev->prom_base;

    for (int i = 0; i < count; i += 2) {
        /* Write address to EEPROM register */
        iwn_write(dev, IWN_EEPROM, (addr + i / 2) << 2);

        /* Poll for READ_VALID */
        int valid = 0;
        for (int j = 0; j < 10; j++) {
            uint32_t val = iwn_read(dev, IWN_EEPROM);
            if (val & IWN_EEPROM_READ_VALID) {
                out[i]     = (val >> 16) & 0xff;
                if (i + 1 < count)
                    out[i + 1] = (val >> 24) & 0xff;
                valid = 1;
                break;
            }
            gBS->Stall(5);
        }
        if (!valid) {
            efi_print_ascii("  iwn: EEPROM read timeout\r\n");
            return -1;
        }

        /* Check OTP ECC errors */
        if (has_otp) {
            uint32_t otp_gp = iwn_read(dev, IWN_OTP_GP);
            if (otp_gp & IWN_OTP_GP_ECC_UNCORR_STTS) {
                efi_print_ascii("  iwn: OTP ECC uncorrectable error\r\n");
                return -1;
            }
            if (otp_gp & IWN_OTP_GP_ECC_CORR_STTS) {
                /* Correctable — clear and continue */
                iwn_clrbits(dev, IWN_OTP_GP, IWN_OTP_GP_ECC_CORR_STTS);
            }
        }
    }
    return 0;
}

/* ---- Print helpers ---- */

static const char hex_chars[] = "0123456789abcdef";

void print_hex8(uint8_t val) {
    char buf[3];
    buf[0] = hex_chars[(val >> 4) & 0xf];
    buf[1] = hex_chars[(val >> 0) & 0xf];
    buf[2] = '\0';
    efi_print_ascii(buf);
}

void print_dec(uint32_t val) {
    if (val == 0) { efi_print_ascii("0"); return; }
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    efi_print_ascii(&buf[i]);
}

static void print_mac(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        if (i > 0) efi_print_ascii(":");
        print_hex8(mac[i]);
    }
}

/* ---- NIC config (radio/antenna config for 5000+ series) ---- */
/* Reference: OpenBSD iwn5000_nic_config() */

void iwn5000_nic_config(iwn_dev *dev) {
    uint32_t val;

    /* Set RADIO_SI and MAC_SI bits (required for 5000+ series) */
    val = iwn_read(dev, IWN_HW_IF_CONFIG);
    val |= IWN_HW_IF_CONFIG_RADIO_SI;
    val |= IWN_HW_IF_CONFIG_MAC_SI;
    iwn_write(dev, IWN_HW_IF_CONFIG, val);

    /* Enable early power-off disable */
    if (iwn_nic_lock(dev) == 0) {
        iwn_prph_setbits(dev, IWN_APMG_PS, IWN_APMG_PS_EARLY_PWROFF_DIS);
        iwn_nic_unlock(dev);
    }
}

/* ---- Main attach function ---- */

EFI_STATUS iwn_attach(iwn_dev *dev) {
    /* Disable all interrupts */
    iwn_write(dev, IWN_INT_MASK, 0);
    iwn_write(dev, IWN_INT, 0xffffffff);
    iwn_write(dev, IWN_FH_INT, 0xffffffff);
    iwn_write_1(dev, IWN_INT_PERIODIC, IWN_INT_PERIODIC_DIS);

    /* Read HW revision */
    uint32_t hw_rev = iwn_read(dev, IWN_HW_REV);
    dev->hw_type = (hw_rev & IWN_HW_REV_TYPE_MASK) >> IWN_HW_REV_TYPE_SHIFT;

    efi_print_ascii("  HW type: ");
    print_dec(dev->hw_type);

    /* Set chip-specific defaults based on hw_type */
    switch (dev->hw_type) {
    case IWN_HW_REV_TYPE_5100:
    case IWN_HW_REV_TYPE_5150:
        dev->txchainmask = 0x1;
        dev->rxchainmask = 0x3;
        efi_print_ascii(" (5100/5150 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_5300:
    case IWN_HW_REV_TYPE_5350:
        dev->txchainmask = 0x7;
        dev->rxchainmask = 0x7;
        efi_print_ascii(" (5300/5350 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_1000:
        dev->txchainmask = 0x1;
        dev->rxchainmask = 0x1;
        efi_print_ascii(" (1000 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_6000:
        dev->txchainmask = 0x6;
        dev->rxchainmask = 0x6;
        efi_print_ascii(" (6000 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_6050:
        dev->txchainmask = 0x6;
        dev->rxchainmask = 0x6;
        efi_print_ascii(" (6050 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_6005:
        dev->txchainmask = 0x6;
        dev->rxchainmask = 0x6;
        efi_print_ascii(" (6005 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_2030:
    case IWN_HW_REV_TYPE_2000:
        dev->txchainmask = 0x1;
        dev->rxchainmask = 0x3;
        efi_print_ascii(" (2000/2030 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_105:
    case IWN_HW_REV_TYPE_135:
        dev->txchainmask = 0x1;
        dev->rxchainmask = 0x1;
        efi_print_ascii(" (105/135 series)\r\n");
        break;
    case IWN_HW_REV_TYPE_4965:
        dev->txchainmask = 0x3;
        dev->rxchainmask = 0x3;
        efi_print_ascii(" (4965 series)\r\n");
        break;
    default:
        dev->txchainmask = 0x7;
        dev->rxchainmask = 0x7;
        efi_print_ascii(" (unknown)\r\n");
        break;
    }

    /* Find PCIe capability for ASPM checks */
    iwn_find_pcie_cap(dev);

    /* Hardware prepare (take from Intel AMT) */
    if (iwn_hw_prepare(dev) != 0) {
        efi_print_ascii("  iwn: hw_prepare failed\r\n");
        return EFI_DEVICE_ERROR;
    }

    /* Check for OTP (6000+ series) */
    if (iwn_read(dev, IWN_OTP_GP) & IWN_OTP_GP_DEV_SEL_OTP) {
        dev->flags |= IWN_FLAG_HAS_OTPROM;
        efi_print_ascii("  OTP: yes\r\n");
    }

    /* APM init (power on) */
    if (iwn_apm_init(dev) != 0) {
        efi_print_ascii("  iwn: APM init failed\r\n");
        return EFI_DEVICE_ERROR;
    }

    /* Check ROM signature */
    uint32_t eeprom_gp = iwn_read(dev, IWN_EEPROM_GP);
    if ((eeprom_gp & 0x7) == 0) {
        efi_print_ascii("  iwn: bad ROM signature\r\n");
        iwn_apm_stop(dev);
        return EFI_DEVICE_ERROR;
    }

    /* Lock EEPROM */
    if (iwn_eeprom_lock(dev) != 0) {
        efi_print_ascii("  iwn: EEPROM lock failed\r\n");
        iwn_apm_stop(dev);
        return EFI_DEVICE_ERROR;
    }

    /* Init OTP if needed */
    if (dev->flags & IWN_FLAG_HAS_OTPROM) {
        if (iwn_init_otprom(dev) != 0) {
            efi_print_ascii("  iwn: OTP init failed\r\n");
            iwn_eeprom_unlock(dev);
            iwn_apm_stop(dev);
            return EFI_DEVICE_ERROR;
        }
    }

    /* ---- Read EEPROM data ---- */

    /* MAC address (6 bytes at word offset 0x015) */
    if (iwn_read_prom_data(dev, IWN_EEPROM_MAC, dev->mac_addr, 6) != 0)
        goto fail;

    /* SKU capabilities */
    uint16_t sku_cap = 0;
    if (iwn_read_prom_data(dev, IWN_EEPROM_SKU_CAP, &sku_cap, 2) != 0)
        goto fail;
    if (sku_cap & IWN_EEPROM_SKU_CAP_11N)
        dev->flags |= IWN_FLAG_HAS_11N;

    /* RF config */
    if (iwn_read_prom_data(dev, IWN_EEPROM_RFCFG, &dev->rfcfg, 2) != 0)
        goto fail;
    dev->txchainmask = IWN_RFCFG_TXANTMSK(dev->rfcfg);
    dev->rxchainmask = IWN_RFCFG_RXANTMSK(dev->rfcfg);

    /* Regulatory domain (5000+ series: indirect via reg base pointer) */
    uint16_t reg_base = 0;
    if (dev->hw_type != IWN_HW_REV_TYPE_4965) {
        if (iwn_read_prom_data(dev, IWN5000_EEPROM_REG, &reg_base, 2) != 0)
            goto fail;
        /* Read domain string (4 bytes at reg_base + IWN5000_EEPROM_DOMAIN) */
        if (iwn_read_prom_data(dev, reg_base + IWN5000_EEPROM_DOMAIN,
                               dev->eeprom_domain, 4) != 0)
            goto fail;
    } else {
        /* 4965: direct read */
        if (iwn_read_prom_data(dev, IWN4965_EEPROM_DOMAIN,
                               dev->eeprom_domain, 4) != 0)
            goto fail;
    }

    /* Calibration data (5000+ series: indirect via cal base pointer) */
    if (dev->hw_type != IWN_HW_REV_TYPE_4965) {
        uint16_t cal_base = 0;
        if (iwn_read_prom_data(dev, IWN5000_EEPROM_CAL, &cal_base, 2) != 0)
            goto fail;

        /* Calibration header (version) */
        struct iwn5000_eeprom_calib_hdr cal_hdr;
        if (iwn_read_prom_data(dev, cal_base, &cal_hdr, sizeof(cal_hdr)) != 0)
            goto fail;
        dev->calib_ver = cal_hdr.version;

        /* Crystal calibration */
        if (iwn_read_prom_data(dev, cal_base + IWN5000_EEPROM_CRYSTAL,
                               &dev->eeprom_crystal, 4) != 0)
            goto fail;
    }

    /* ---- Cleanup ---- */
    iwn_eeprom_unlock(dev);
    iwn_apm_stop(dev);

    /* ---- Print results ---- */
    efi_print_ascii("  MAC: ");
    print_mac(dev->mac_addr);
    efi_print_ascii("\r\n");

    efi_print_ascii("  Domain: ");
    /* Domain is 2 ASCII chars + padding */
    char domain_str[3] = { dev->eeprom_domain[0], dev->eeprom_domain[1], '\0' };
    efi_print_ascii(domain_str);
    efi_print_ascii("\r\n");

    efi_print_ascii("  RF config: type=");
    print_dec(IWN_RFCFG_TYPE(dev->rfcfg));
    efi_print_ascii(" step=");
    print_dec(IWN_RFCFG_STEP(dev->rfcfg));
    efi_print_ascii(" dash=");
    print_dec(IWN_RFCFG_DASH(dev->rfcfg));
    efi_print_ascii(" tx=0x");
    print_hex8(dev->txchainmask);
    efi_print_ascii(" rx=0x");
    print_hex8(dev->rxchainmask);
    efi_print_ascii("\r\n");

    if (dev->hw_type != IWN_HW_REV_TYPE_4965) {
        efi_print_ascii("  Calibration ver: ");
        print_dec(dev->calib_ver);
        efi_print_ascii("\r\n");
    }

    if (dev->flags & IWN_FLAG_HAS_11N)
        efi_print_ascii("  802.11n: yes\r\n");

    return EFI_SUCCESS;

fail:
    iwn_eeprom_unlock(dev);
    iwn_apm_stop(dev);
    efi_print_ascii("  iwn: EEPROM read failed\r\n");
    return EFI_DEVICE_ERROR;
}
