#include "v3d_model.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "V3D model line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

#define HUB 0xfec00000ULL
#define CORE 0xfec04000ULL
#define PM_GRAFX 0xfe10010cULL
#define ASB_S 0xfec11008ULL
#define ASB_M 0xfec1100cULL
#define RESET_RELEASED 64U
#define TLB_FIRST (V3D_GPU_BASE >> 12)
#define TLB_PAGES 0x800U
#define TARGET_FIRST ((V3D_TARGET_BASE - V3D_GPU_BASE) >> 12)
#define TARGET_PAGES 0x200U
#define LISTS 0x3000U
#define TILE_ALLOC 0x10000U
#define TILE_STATE 0x20000U
#define SURFACE 0x40000U
#define PTE_FLAGS 0x30000000U
#define POLLS_IN_FLIGHT 3

static uint32_t die(const char* reason) {
    fprintf(stderr, "V3D model: %s\n", reason);
    exit(1);
}

static uint32_t get16(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const unsigned char* p) { return get16(p) | (get16(p + 2) << 16); }

static V3dModelRegion* by_cpu(V3dModel* m, const void* memory) {
    const unsigned char* p = memory;
    for (unsigned int i = 0; i < m->regions; i++)
        if (p >= m->region[i].cpu && p < m->region[i].cpu + m->region[i].size) return &m->region[i];
    return NULL;
}

static V3dModelRegion* by_physical(V3dModel* m, uint64_t physical) {
    for (unsigned int i = 0; i < m->regions; i++)
        if (physical >= m->region[i].physical && physical < m->region[i].physical + m->region[i].size)
            return &m->region[i];
    return NULL;
}

static const uint32_t* table_window(const unsigned char* table) {
    return (const uint32_t*)(const void*)(table + (size_t)TLB_FIRST * 4);
}

static int powered(const V3dModel* m) {
    return m->domain_on && (m->pm_grafx & RESET_RELEASED) && !(m->bridge[0] & 3U) && !(m->bridge[1] & 3U);
}

/* Device-side address translation through the TLB snapshot. */
static int locate(V3dModel* m, uint32_t address, V3dModelRegion** region, size_t* offset) {
    uint32_t page = address >> 12;
    uint32_t entry = page >= TLB_FIRST && page < TLB_FIRST + TLB_PAGES ? m->tlb[page - TLB_FIRST] : 0;
    if (!m->tlb_valid || (entry & PTE_FLAGS) != PTE_FLAGS) {
        m->mmu_faults++;
        m->hub_int |= 0x10U;
        m->mmu_ctl |= 1U << 20;
        return 0;
    }
    uint64_t physical = (uint64_t)(entry & 0xffffffU) << 12;
    *region = by_physical(m, physical);
    /* A mapping may only name memory the driver was lent, never its table. */
    CHECK(*region && *region != &m->region[0]);
    *offset = (size_t)(physical - (*region)->physical) + (address & 4095U);
    return 1;
}

static int device_copy(V3dModel* m, uint32_t address, unsigned char* data, size_t bytes, int write) {
    while (bytes) {
        V3dModelRegion* region;
        size_t offset, chunk = 4096U - (address & 4095U);
        if (chunk > bytes) chunk = bytes;
        if (!locate(m, address, &region, &offset)) return 0;
        if (write) memcpy(region->ram + offset, data, chunk);
        else memcpy(data, region->ram + offset, chunk);
        address += (uint32_t)chunk; data += chunk; bytes -= chunk;
    }
    return 1;
}

/* CPU and RAM views agree: cleaned before the device reads, and no dirty
 * line left that could later overwrite what the device writes. */
static void check_synced(V3dModel* m, uint32_t address, size_t bytes) {
    while (bytes) {
        V3dModelRegion* region;
        size_t offset, chunk = 4096U - (address & 4095U);
        if (chunk > bytes) chunk = bytes;
        CHECK(locate(m, address, &region, &offset));
        CHECK(!memcmp(region->cpu + offset, region->ram + offset, chunk));
        address += (uint32_t)chunk; bytes -= chunk;
    }
}

static uint32_t hash_bytes(uint32_t hash, const unsigned char* data, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) hash = (hash ^ data[i]) * 16777619U;
    return hash;
}

/* CPU view of everything the in-flight job owns. */
static uint32_t job_hash(V3dModel* m) {
    const V3dModelRegion* table = &m->region[0], *arena = &m->region[1];
    uint32_t hash = hash_bytes(2166136261U, arena->cpu, LISTS);
    hash = hash_bytes(hash, arena->cpu + SURFACE, m->job_bytes + 4096U);
    hash = hash_bytes(hash, (const unsigned char*)table_window(table->cpu), TLB_PAGES * 4);
    for (uint32_t done = 0; m->job_writes_target && done < m->job_bytes; done += 4096U) {
        V3dModelRegion* region;
        size_t offset;
        CHECK(locate(m, V3D_TARGET_BASE + done, &region, &offset));
        hash = hash_bytes(hash, region->cpu + offset, 4096U);
    }
    return hash;
}

/* After a failed job the CPU may observe whatever the device already wrote. */
static void expose_partial_output(V3dModel* m) {
    for (uint32_t done = 0; done < m->job_bytes; done += 4096U) {
        V3dModelRegion* region;
        size_t offset;
        int faults = m->mmu_faults;
        uint32_t hub = m->hub_int, control = m->mmu_ctl;
        if (locate(m, m->job_target + done, &region, &offset)) memcpy(region->cpu + offset, region->ram + offset, 4096U);
        m->mmu_faults = faults; m->hub_int = hub; m->mmu_ctl = control;
    }
    m->job_bytes = 0;
}

static void core_reset(V3dModel* m) {
    if (m->active == 2) expose_partial_output(m);
    m->active = 0;
    m->mmu_ctl = m->mmuc = m->table_base = m->illegal = m->gmp_cfg = 0;
    m->hub_int = m->core_int = 0;
    m->bfc = m->rfc = m->counter_reset;
    m->qma = m->qms = m->qts = m->qba[0] = m->qba[1] = 0;
    m->tlb_valid = 0;
    m->resets++;
}

static void refresh_tlb(V3dModel* m) {
    const V3dModelRegion* table = &m->region[0];
    CHECK(!m->active && (m->mmu_ctl & 1U) && (uint64_t)m->table_base << 12 == table->physical);
    /* Entries must have reached RAM before the device re-reads them. */
    CHECK(!memcmp(table_window(table->cpu), table_window(table->ram), TLB_PAGES * 4));
    memcpy(m->tlb, table_window(table->ram), TLB_PAGES * 4);
    m->tlb_valid = 1;
    m->tlb_flushes++;
}

static void start_job(V3dModel* m, int kind, int completes) {
    m->active = kind;
    m->polls = 0;
    m->completes = completes;
    m->snapshot = job_hash(m);
}

static void check_submission(V3dModel* m) {
    const V3dModelRegion* table = &m->region[0];
    CHECK(!m->active && (m->mmu_ctl & 1U) && (m->mmuc & 1U) && m->tlb_valid && !(m->gmp_cfg & 2U));
    CHECK(!memcmp(m->tlb, table_window(table->ram), TLB_PAGES * 4));
    CHECK(!memcmp(m->tlb, table_window(table->cpu), TLB_PAGES * 4));
    CHECK(!memcmp(m->region[1].cpu, m->region[1].ram, LISTS));
    /* The scratch page must be arena memory that no mapping can reach. */
    CHECK(m->illegal >> 31);
    uint64_t scratch = (uint64_t)(m->illegal & 0xffffffU) << 12;
    CHECK(by_physical(m, scratch) == &m->region[1]);
    for (unsigned int i = 0; i < TLB_PAGES; i++) CHECK((m->tlb[i] & 0xffffffU) != (scratch >> 12) || !m->tlb[i]);
}

static void submit_bin(V3dModel* m, uint32_t end) {
    unsigned char list[64], fill[256];
    uint32_t size = end - m->qba[0];
    check_submission(m);
    CHECK(m->qba[0] == V3D_GPU_BASE && size > 9 && size <= sizeof(list));
    CHECK(device_copy(m, m->qba[0], list, size, 0));
    CHECK(list[0] == 120 && list[9] == 119 && list[10] == 0 && list[11] == 19 && list[12] == 92);
    CHECK(list[size - 2] == 6 && list[size - 1] == 4);
    uint32_t width = get16(list + 5) + 1, height = get16(list + 7) + 1;
    uint32_t tiles = ((width + 63) / 64) * ((height + 63) / 64);
    CHECK(m->qma == V3D_GPU_BASE + TILE_ALLOC && m->qms == 0x10000U && m->qts == (V3D_GPU_BASE + TILE_STATE + 2));
    m->job_bytes = width * height * 4;
    m->job_writes_target = 0;
    m->job_target = V3D_GPU_BASE + SURFACE;
    memset(fill, 0xb1, sizeof(fill));
    for (uint32_t tile = 0; tile < tiles; tile++) {
        CHECK(device_copy(m, V3D_GPU_BASE + TILE_ALLOC + tile * 64, fill, 64, 1));
        CHECK(device_copy(m, V3D_GPU_BASE + TILE_STATE + tile * 256, fill, 256, 1));
    }
    m->bin_jobs++;
    start_job(m, 1, !m->stall_bin);
}

static const unsigned char* expect(const unsigned char* p, const unsigned char* end, unsigned int opcode, unsigned int bytes) {
    CHECK(p + bytes <= end && *p == opcode);
    return p + bytes;
}

static void submit_render(V3dModel* m, uint32_t end) {
    unsigned char list[4096], tile[64], row[64 * 4];
    uint32_t size = end - m->qba[1];
    check_submission(m);
    CHECK(m->qba[1] == V3D_GPU_BASE + 0x1000U && size > 64 && size <= sizeof(list));
    CHECK(device_copy(m, m->qba[1], list, size, 0));
    const unsigned char* p = list, *stop = list + size;
    CHECK(p[1] == 0);
    uint32_t width = get16(p + 2), height = get16(p + 4);
    p = expect(p, stop, 121, 9);
    CHECK(p[1] == 3);
    uint32_t color = get32(p + 2);
    p = expect(p, stop, 121, 9);
    CHECK(p[1] == 0x81); p = expect(p, stop, 121, 9);
    CHECK(p[1] == 2); p = expect(p, stop, 121, 9);
    CHECK(p[1] == 4); p = expect(p, stop, 126, 2);
    CHECK(get32(p + 1) == V3D_GPU_BASE + TILE_ALLOC); p = expect(p, stop, 123, 5);
    uint32_t columns = (width + 63) / 64, rows = (height + 63) / 64;
    CHECK(p[3] == columns && p[4] == rows && get32(p + 5) == (columns | (rows << 12)));
    p = expect(p, stop, 122, 9);
    for (unsigned int i = 0; i < 2; i++) {
        CHECK(!p[1] && !p[2] && !p[3]); p = expect(p, stop, 124, 4);
        p = expect(p, stop, 26, 1);
        CHECK(p[1] == 8); p = expect(p, stop, 29, 13);
        if (!i) { CHECK(p[1] == 3); p = expect(p, stop, 25, 2); }
        p = expect(p, stop, 27, 1);
    }
    p = expect(p, stop, 19, 1);
    uint32_t tile_start = get32(p + 1), tile_bytes = get32(p + 5) - tile_start;
    p = expect(p, stop, 20, 9);
    const unsigned char* coordinates = p;
    for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < columns; x++) {
        CHECK(p[1] == x && p[2] == y); p = expect(p, stop, 23, 3);
    }
    p = expect(p, stop, 13, 1);
    CHECK(p == stop && width * height * 4 == m->job_bytes);

    CHECK(tile_start == V3D_GPU_BASE + 0x2000U && tile_bytes >= 19 && tile_bytes <= sizeof(tile));
    CHECK(device_copy(m, tile_start, tile, tile_bytes, 0));
    p = tile; stop = tile + tile_bytes;
    p = expect(p, stop, 125, 1);
    int load = *p == 30;
    if (load) {
        CHECK(p[1] == 0 && get16(p + 2) == (27U << 4) && (get32(p + 4) >> 4) == width * 4);
        CHECK(get32(p + 9) == V3D_GPU_BASE + SURFACE);
        p = expect(p, stop, 30, 13);
    }
    p = expect(p, stop, 26, 1);
    CHECK(p[1] == 0 && get16(p + 2) == (27U << 4) && (get32(p + 4) >> 4) == width * 4);
    uint32_t destination = get32(p + 9);
    p = expect(p, stop, 29, 13);
    CHECK(p[1] == 3); p = expect(p, stop, 25, 2);
    p = expect(p, stop, 27, 1);
    p = expect(p, stop, 18, 1);
    CHECK(p == stop);

    /* A clear owns only the arena; a present owns exactly its target pages. */
    uint32_t target_pages = load ? m->job_bytes >> 12 : 0;
    CHECK(destination == (load ? V3D_TARGET_BASE : V3D_GPU_BASE + SURFACE));
    for (uint32_t page = 0; page < TARGET_PAGES; page++)
        CHECK(!(m->tlb[TARGET_FIRST + page]) == (page >= target_pages));
    if (load) check_synced(m, V3D_GPU_BASE + SURFACE, m->job_bytes);
    check_synced(m, destination, m->job_bytes + (load ? 0 : 4096U));
    m->job_target = destination;
    m->job_writes_target = (uint32_t)load;

    int partial = m->stall_render || m->fault_render;
    uint32_t tiles = columns * rows, limit = partial ? tiles / 2 : tiles;
    for (uint32_t index = 0; index < limit; index++) {
        uint32_t x = coordinates[index * 3 + 1] * 64U, y = coordinates[index * 3 + 2] * 64U;
        uint32_t span = width - x < 64 ? width - x : 64, bottom = y + 64;
        if (!m->overrun && bottom > height) bottom = height;
        for (uint32_t line = y; line < bottom; line++) {
            uint32_t offset = (line * width + x) * 4;
            if (load) CHECK(device_copy(m, V3D_GPU_BASE + SURFACE + offset, row, span * 4, 0));
            else for (uint32_t i = 0; i < span; i++) memcpy(row + i * 4, &color, 4);
            if (!device_copy(m, destination + offset, row, span * 4, 1)) { partial = 1; break; }
        }
    }
    if (m->corrupt) {
        CHECK(device_copy(m, destination + 20, row, 1, 0));
        row[0] ^= 1;
        CHECK(device_copy(m, destination + 20, row, 1, 1));
    }
    if (m->fault_render) { m->hub_int |= 0x10U; m->mmu_ctl |= 1U << 20; }
    start_job(m, 2, !partial);
}

static void poll(V3dModel* m, int kind) {
    if (m->active != kind || !m->completes || ++m->polls < POLLS_IN_FLIGHT) return;
    /* The CPU must not have written anything the job owned. */
    CHECK(job_hash(m) == m->snapshot);
    if (kind == 1) m->bfc = (m->bfc + 1) & 255U;
    else { m->rfc = (m->rfc + 1) & 255U; m->render_jobs++; m->job_bytes = 0; }
    m->active = 0;
}

static uint32_t read32(void* context, uintptr_t address) {
    V3dModel* m = context;
    if (address == PM_GRAFX) return m->pm_grafx;
    if (address == ASB_S) return m->bridge[0];
    if (address == ASB_M) return m->bridge[1];
    if (!powered(m)) return 0;
    switch (address) {
    case CORE + 0x00: return m->ident[0];
    case CORE + 0x04: return m->ident[1];
    case HUB + 0x0c: return m->ident[2];
    case HUB + 0x10: return m->ident[3];
    case CORE + 0x900: return m->csd_busy ? 2U : 0;
    case HUB + 0x400: return 0;
    case HUB + 0x1200: return m->mmu_ctl | (m->tlb_stuck ? 0x80U : 0);
    case HUB + 0x1000: return m->mmuc & 1U;
    case HUB + 0x1204: return m->table_base ^ (m->bad_table_readback ? 1U : 0);
    case HUB + 0x50: return m->hub_int;
    case CORE + 0x50: return m->core_int;
    case CORE + 0x30: return m->l2t_stuck ? 1U : 0;
    case CORE + 0x134: poll(m, 1); return m->bfc;
    case CORE + 0x138: poll(m, 2); return m->rfc;
    case CORE + 0x800: return 0;
    /* Idle-named status bits; the driver must not treat them as faults. */
    case CORE + 0xf20: return 0x7000U;
    default: return die("unexpected register read");
    }
}

static void write32(void* context, uintptr_t address, uint32_t value) {
    V3dModel* m = context;
    m->register_writes++;
    if (address == PM_GRAFX) {
        CHECK((value >> 24) == 0x5a);
        value &= 0xffffffU;
        if (m->reset_stuck) value |= m->pm_grafx & RESET_RELEASED;
        if ((m->pm_grafx & RESET_RELEASED) && !(value & RESET_RELEASED)) core_reset(m);
        m->pm_grafx = value;
        return;
    }
    if (address == ASB_S || address == ASB_M) {
        uint32_t* bridge = &m->bridge[address == ASB_M];
        CHECK((value >> 24) == 0x5a);
        uint32_t stop = value & 1U, acknowledged = m->bridge_stuck ? (*bridge >> 1) & 1U : stop;
        *bridge = stop | (acknowledged << 1);
        return;
    }
    CHECK(powered(m));
    /* Only the shutdown sequence may touch a device with a job in flight. */
    if (m->active) CHECK(address == CORE + 0x60 || address == HUB + 0x60 || address == CORE + 0x804);
    switch (address) {
    case CORE + 0x60: case HUB + 0x60: CHECK(value == ~0U); break;
    case CORE + 0x58: m->core_int &= ~value; break;
    case HUB + 0x58: m->hub_int &= ~value; break;
    case CORE + 0x34: CHECK(!value); break;
    case CORE + 0x38: CHECK(value == ~0U); break;
    case HUB + 0x1204: m->table_base = value; break;
    case HUB + 0x1200:
        m->mmu_ctl = (value & ~0x84U) | (m->mmu_ctl & ~value & ((1U << 27) | (1U << 20) | (1U << 12)));
        if (value & 4U) refresh_tlb(m);
        break;
    case HUB + 0x1230: m->illegal = value; break;
    case HUB + 0x1000: m->mmuc = value; break;
    case CORE + 0x30: CHECK(value == 1U); break;
    case CORE + 0x24: CHECK(value == 0x0f0f0f0fU); break;
    case CORE + 0x170: m->qma = value; break;
    case CORE + 0x174: m->qms = value; break;
    case CORE + 0x15c: m->qts = value; break;
    case CORE + 0x160: m->qba[0] = value; break;
    case CORE + 0x164: m->qba[1] = value; break;
    case CORE + 0x168: submit_bin(m, value); break;
    case CORE + 0x16c: submit_render(m, value); break;
    case CORE + 0x804:
        m->gmp_cfg = value;
        if (value & 2U) { if (m->active == 2) expose_partial_output(m); m->active = 0; }
        break;
    default: (void)die("unexpected register write");
    }
}

static uint32_t clock_us(void* context) {
    V3dModel* m = context;
    m->clock += m->clock_step;
    return m->clock;
}

static int owned(void* context) { return ((V3dModel*)context)->owner; }

static void cache(void* context, void* memory, size_t bytes, enum HwCache operation) {
    V3dModel* m = context;
    V3dModelRegion* region = by_cpu(m, memory);
    CHECK(memory && bytes && region && !m->active);
    size_t first = (size_t)((unsigned char*)memory - region->cpu), end = first + bytes;
    CHECK(end <= region->size);
    first &= ~(size_t)63;
    end = (end + 63) & ~(size_t)63;
    if (end > region->size) end = region->size;
    if (operation == HW_INVALIDATE) memcpy(region->cpu + first, region->ram + first, end - first);
    else memcpy(region->ram + first, region->cpu + first, end - first);
    m->cache_calls++;
}

static uint64_t physical(void* context, const void* memory) {
    V3dModel* m = context;
    V3dModelRegion* region = by_cpu(m, memory);
    return region ? region->physical + (uint64_t)((const unsigned char*)memory - region->cpu) : 0;
}

static int property(void* context, uint32_t tag, uint32_t* words, unsigned int count) {
    V3dModel* m = context;
    m->property_calls++;
    CHECK(count == 2 && words);
    if (tag == m->fail_tag) return m->fail_result;
    int absent = tag == m->absent_tag;
    if (tag == 0x00038030U) {
        CHECK(words[0] == 11 && words[1] <= 1);
        if (absent && words[1]) words[1] = 2;
        else m->domain_on = (int)words[1];
    } else if (tag == 0x00038001U) {
        CHECK(words[0] == 5 && words[1] == 1 && m->domain_on);
        if (absent) words[1] = 2;
        else m->clock_on = 1;
    } else if (tag == 0x00030002U) {
        CHECK(words[0] == 5);
        words[1] = absent ? 0 : 500000000U;
    } else (void)die("unexpected property tag");
    return HW_OK;
}

void v3d_model_add(V3dModel* model, void* memory, size_t size, uint64_t address) {
    CHECK(model->regions < V3D_MODEL_REGIONS && memory && size);
    V3dModelRegion* region = &model->region[model->regions++];
    region->cpu = memory;
    region->ram = malloc(size);
    CHECK(region->ram);
    memcpy(region->ram, memory, size);
    region->physical = address;
    region->size = size;
}

void v3d_model_init(V3dModel* model, void* table, void* arena) {
    memset(model, 0, sizeof(*model));
    model->owner = 1;
    model->clock_step = 1000;
    model->counter_reset = 255;
    model->ident[0] = 0x04443356U; model->ident[1] = 2;
    model->ident[2] = 0x124U; model->ident[3] = 0x100U;
    model->bridge[0] = model->bridge[1] = 3;
    model->bfc = model->rfc = model->counter_reset;
    v3d_model_add(model, table, V3D_TABLE_BYTES, 0x1000000U);
    v3d_model_add(model, arena, V3D_ARENA_BYTES, 0x2000000U);
}

void v3d_model_free(V3dModel* model) {
    for (unsigned int i = 0; i < model->regions; i++) free(model->region[i].ram);
    model->regions = 0;
}

HwOps v3d_model_ops(V3dModel* model) {
    HwOps io = {model, read32, write32, clock_us, owned, cache, physical, property};
    return io;
}

int v3d_model_quiet(const V3dModel* model) {
    const uint32_t* cpu = table_window(model->region[0].cpu), *ram = table_window(model->region[0].ram);
    if (model->active) return 0;
    for (unsigned int page = 0; page < TARGET_PAGES; page++)
        if (cpu[TARGET_FIRST + page] || ram[TARGET_FIRST + page] ||
            (model->tlb_valid && model->tlb[TARGET_FIRST + page])) return 0;
    return 1;
}

int v3d_model_off(const V3dModel* model) {
    return !model->domain_on && !(model->pm_grafx & RESET_RELEASED) &&
           model->bridge[0] == 3 && model->bridge[1] == 3 && !model->active;
}
