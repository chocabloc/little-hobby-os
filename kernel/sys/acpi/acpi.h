#pragma once

#include "boot/stivale2.h"

typedef struct [[gnu::packed]] {
    char sign[8];
    uint8_t chksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;

    // intoduced in version 2.0
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t chksum_ext;
    uint8_t reserved[3];
} rsdp_t;

typedef struct [[gnu::packed]] {
    char sign[4];
    uint32_t length;
    uint8_t rev;
    uint8_t chksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_rev;
    uint32_t creator_id;
    uint32_t creator_rev;
} acpi_sdt_hdr;

typedef struct [[gnu::packed]] {
    // the header
    acpi_sdt_hdr hdr;

    // the data
    uint8_t data[];
} acpi_sdt;

// acpi generic address structure
typedef struct [[gnu::packed]] {
    uint8_t addr_space_id;
    uint8_t reg_bit_width;
    uint8_t reg_bit_offset;
    uint8_t reserved;
    uint64_t address;
} acpi_gas_t;

#define SDT_SIGN_MADT "APIC"
#define SDT_SIGN_BGRT "BGRT"
#define SDT_SIGN_HPET "HPET"

void acpi_init(stv2_struct_tag_rsdp*);
acpi_sdt* acpi_get_sdt(const char* sign);
