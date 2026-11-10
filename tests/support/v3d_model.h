#pragma once
#include <vantage/drivers/v3d.h>

/* Host model of the Pi 4 V3D 4.2 block behind HwOps: firmware power/clock
 * properties, the reset line and AXI bridges, identity, the MMU with a TLB,
 * and a decoder that executes the driver's clear/present control lists.
 *
 * Memory is modelled pessimistically. The device only ever sees a RAM shadow
 * that changes when the driver cleans the CPU cache, and the CPU only sees
 * device output after it invalidates. Device addresses go through the TLB
 * snapshot taken at the last flush. A job stays in flight for a few counter
 * polls; touching its buffers, caches or registers meanwhile aborts the test.
 * This is a software contract check, not V3D microarchitecture. */
#define V3D_MODEL_REGIONS 8

typedef struct {
    unsigned char* cpu;
    unsigned char* ram;
    uint64_t physical;
    size_t size;
} V3dModelRegion;

typedef struct V3dModel {
    /* Fault injection, set by tests at any time. */
    int owner;
    uint32_t fail_tag;        /* This property tag returns fail_result. */
    int fail_result;
    uint32_t absent_tag;      /* This property tag answers "off/missing". */
    int bridge_stuck;         /* Bridge acknowledgements never change. */
    int reset_stuck;          /* The reset line cannot be asserted. */
    uint32_t ident[4];        /* Core IDENT0/1, hub IDENT1/2. */
    int csd_busy, tlb_stuck, l2t_stuck, bad_table_readback;
    int stall_bin;            /* The binner never completes. */
    int stall_render;         /* Rendering stops halfway and never completes. */
    int fault_render;         /* Rendering stops halfway with an MMU fault. */
    int corrupt;              /* One stored pixel is wrong. */
    int overrun;              /* Stores ignore the frame height. */
    uint32_t clock_step, counter_reset;

    /* Observations. */
    unsigned int bin_jobs, render_jobs, tlb_flushes, resets, property_calls;
    unsigned int register_writes, cache_calls;
    int domain_on, clock_on, mmu_faults;

    /* Device state. */
    V3dModelRegion region[V3D_MODEL_REGIONS];
    unsigned int regions;
    uint32_t clock, pm_grafx, bridge[2];
    uint32_t mmu_ctl, mmuc, table_base, illegal, gmp_cfg;
    uint32_t hub_int, core_int, bfc, rfc;
    uint32_t qma, qms, qts, qba[2];
    uint32_t tlb[0x800];
    int tlb_valid;
    /* In-flight job: 0 none, 1 binning, 2 rendering. */
    int active, polls, completes;
    uint32_t snapshot;
    uint32_t job_target, job_bytes, job_writes_target;
} V3dModel;

/* table and arena are the buffers later passed to v3d_init. */
void v3d_model_init(V3dModel* model, void* table, void* arena);
/* Lend another CPU buffer a physical address so the device may be given it. */
void v3d_model_add(V3dModel* model, void* memory, size_t size, uint64_t physical);
void v3d_model_free(V3dModel* model);
HwOps v3d_model_ops(V3dModel* model);
/* No job in flight and the present window unmapped in table, RAM and TLB. */
int v3d_model_quiet(const V3dModel* model);
/* Core held in reset with both bridges stopped and the firmware domain off. */
int v3d_model_off(const V3dModel* model);
