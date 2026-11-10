#include "internal.h"

/* Bounded flattened-device-tree reader. All reads are bytewise: discovery runs
 * before the MMU, when unaligned multi-byte Device accesses can fault. */
typedef struct { const uint8_t* data; size_t length; } Bytes;
typedef struct {
    Bytes name, reg, device_type, status, size, alignment, alloc_ranges, ranges;
    Bytes initrd_start, initrd_end;
    uint32_t ac, sc, parent_ac, parent_sc;
    int disabled, children, reserved_parent, reserved_child;
    int no_map, reusable, ac_seen, sc_seen;
} Node;

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int equal(Bytes bytes, const char* text) {
    size_t i;
    for (i = 0; i < bytes.length; i++) {
        if (!text[i] || bytes.data[i] != (uint8_t)text[i]) return 0;
    }
    return text[bytes.length] == 0;
}

static int string_equal(Bytes bytes, const char* text) {
    if (!bytes.data || !bytes.length || bytes.data[bytes.length - 1]) return 0;
    bytes.length--;
    return equal(bytes, text);
}

static int prefix(Bytes bytes, const char* text) {
    size_t i;
    for (i = 0; text[i]; i++) if (i == bytes.length || bytes.data[i] != (uint8_t)text[i]) return 0;
    return 1;
}

static int string_at(Bytes bytes, size_t offset, size_t limit, Bytes* result, size_t* end) {
    size_t i;
    if (offset > bytes.length) return 0;
    for (i = 0; i <= limit && i < bytes.length - offset; i++) {
        if (!bytes.data[offset + i]) {
            result->data = bytes.data + offset;
            result->length = i;
            *end = offset + i + 1;
            return 1;
        }
    }
    return 0;
}

static int scalar(Bytes value, uint32_t cells, uint64_t* result) {
    if (cells == 1 && value.length == 4) {
        *result = be32(value.data);
        return 1;
    }
    if (cells == 2 && value.length == 8) {
        *result = ((uint64_t)be32(value.data) << 32) | be32(value.data + 4);
        return 1;
    }
    return 0;
}

static int inactive(const Node* node) {
    return node->disabled || (node->status.data &&
        !string_equal(node->status, "okay") && !string_equal(node->status, "ok"));
}

static int set(Bytes* slot, Bytes value) {
    if (slot->data) return 0;
    *slot = value;
    return 1;
}

static int tuple_stride(Bytes bytes, uint32_t ac, uint32_t sc, size_t* stride) {
    if (ac < 1 || ac > 2 || sc < 1 || sc > 2) return 0;
    *stride = (size_t)(ac + sc) * 4;
    return bytes.data && bytes.length && !(bytes.length % *stride);
}

static int tuple(Bytes bytes, size_t offset, uint32_t ac, uint32_t sc,
                  uint64_t* start, uint64_t* size) {
    Bytes a = {bytes.data + offset, (size_t)ac * 4};
    Bytes s = {bytes.data + offset + (size_t)ac * 4, (size_t)sc * 4};
    return scalar(a, ac, start) && scalar(s, sc, size);
}

static int complete(const Node* node, size_t depth, MmDiscovery* info) {
    size_t stride, offset;
    uint64_t start, size;
    MmRange range;
    if (inactive(node)) return 1;
    if (depth == 2 && (string_equal(node->device_type, "memory") ||
        equal(node->name, "memory") || prefix(node->name, "memory@"))) {
        if (!string_equal(node->device_type, "memory") ||
            !tuple_stride(node->reg, node->parent_ac, node->parent_sc, &stride)) return 0;
        for (offset = 0; offset < node->reg.length; offset += stride) {
            if (!tuple(node->reg, offset, node->parent_ac, node->parent_sc, &start, &size)) return 0;
            if (!size) continue;
            if (!mm_range_make(start, size, &range)) return 0;
            if (start < MM_RAM_LIMIT) {
                if (range.end > MM_RAM_LIMIT) range.end = MM_RAM_LIMIT;
                if (!mm_ranges_insert(&info->ram, range)) return 0;
            }
        }
    }
    if (node->reserved_parent && (node->ac != node->parent_ac || node->sc != node->parent_sc ||
        !node->ranges.data || node->ranges.length)) return 0;
    if (node->reserved_child) {
        if (node->no_map && node->reusable) return 0;
        if (node->reg.data) {
            if (!tuple_stride(node->reg, node->parent_ac, node->parent_sc, &stride)) return 0;
            for (offset = 0; offset < node->reg.length; offset += stride) {
                if (!tuple(node->reg, offset, node->parent_ac, node->parent_sc, &start, &size)) return 0;
                if (!size) continue;
                if (!mm_range_make(start, size, &range) || !mm_ranges_insert(&info->reserved, range) ||
                    (node->no_map && !mm_ranges_insert(&info->no_map, range))) return 0;
            }
        } else {
            MmDynamic* dynamic;
            if (info->dynamic_count == MM_MAX_DYNAMIC) return 0;
            dynamic = &info->dynamic[info->dynamic_count];
            if (!scalar(node->size, node->parent_sc, &dynamic->size) || !dynamic->size) return 0;
            dynamic->alignment = MM_PAGE_SIZE;
            dynamic->no_map = node->no_map;
            if (node->alignment.data && !scalar(node->alignment, node->parent_sc, &dynamic->alignment)) return 0;
            if (!dynamic->alignment || (dynamic->alignment & (dynamic->alignment - 1))) return 0;
            if (node->alloc_ranges.data) {
                if (!tuple_stride(node->alloc_ranges, node->parent_ac, node->parent_sc, &stride)) return 0;
                for (offset = 0; offset < node->alloc_ranges.length; offset += stride) {
                    if (!tuple(node->alloc_ranges, offset, node->parent_ac, node->parent_sc, &start, &size)) return 0;
                    if (!size) continue;
                    if (dynamic->allowed_count == 8 || !mm_range_make(start, size, &range)) return 0;
                    dynamic->allowed[dynamic->allowed_count++] = range;
                }
                if (!dynamic->allowed_count) return 0;
            }
            info->dynamic_count++;
        }
    }
    if (depth == 2 && equal(node->name, "chosen")) {
        if (node->initrd_start.data || node->initrd_end.data) {
            uint64_t end;
            if (!scalar(node->initrd_start, (uint32_t)(node->initrd_start.length / 4), &start) ||
                !scalar(node->initrd_end, (uint32_t)(node->initrd_end.length / 4), &end) || end <= start ||
                !mm_range_make(start, end - start, &range) || !mm_ranges_insert(&info->reserved, range)) return 0;
        }
    }
    return 1;
}

void mm_discovery_init(MmDiscovery* info) {
    mm_bytes_zero(info, sizeof(*info));
    info->ram.entries = info->ram_storage;
    info->reserved.entries = info->reserved_storage;
    info->no_map.entries = info->no_map_storage;
    info->ram.capacity = info->reserved.capacity = info->no_map.capacity = MM_MAX_RANGES;
}

int mm_dtb_size(const uint8_t* header, size_t length, size_t* result) {
    uint32_t size;
    if (!header || length < 40 || be32(header) != 0xd00dfeedU) return 0;
    size = be32(header + 4);
    if (size < 40 || size > MM_MAX_DTB_SIZE) return 0;
    *result = size;
    return 1;
}

static int property(Node* n, Bytes name, Bytes value) {
    if (equal(name, "#address-cells") || equal(name, "#size-cells")) {
        int address = equal(name, "#address-cells");
        int* seen = address ? &n->ac_seen : &n->sc_seen;
        uint32_t* cells = address ? &n->ac : &n->sc;
        if (*seen || value.length != 4) return 0;
        *cells = be32(value.data);
        *seen = 1;
        return 1;
    }
    if (equal(name, "reg")) return set(&n->reg, value);
    if (equal(name, "device_type") || equal(name, "status")) {
        Bytes text;
        size_t end;
        if (!string_at(value, 0, 31, &text, &end) || end != value.length) return 0;
        return set(equal(name, "status") ? &n->status : &n->device_type, value);
    }
    if (equal(name, "size")) return set(&n->size, value);
    if (equal(name, "alignment")) return set(&n->alignment, value);
    if (equal(name, "alloc-ranges")) return set(&n->alloc_ranges, value);
    if (equal(name, "ranges")) return set(&n->ranges, value);
    if (equal(name, "linux,initrd-start")) return set(&n->initrd_start, value);
    if (equal(name, "linux,initrd-end")) return set(&n->initrd_end, value);
    if (equal(name, "no-map") || equal(name, "reusable")) {
        int* flag = equal(name, "no-map") ? &n->no_map : &n->reusable;
        if (*flag || value.length) return 0;
        *flag = 1;
        return 1;
    }
    /* Do not silently ignore restrictions supplied for a crash kernel. */
    return !equal(name, "linux,usable-memory") && !equal(name, "linux,usable-memory-range");
}

int mm_discover(const uint8_t* blob, size_t length, MmDiscovery* info) {
    size_t total, structure_start, strings_start, reserve_start, structure_size, strings_size;
    size_t cursor, depth = 0;
    uint32_t version, compatible;
    int root_seen = 0;
    MmRange structure_range, strings_range;
    Bytes structure, strings;
    Node nodes[32];
    if (!info || info->dtb_size || !mm_dtb_size(blob, length, &total) || length < total) return 0;
    version = be32(blob + 20);
    compatible = be32(blob + 24);
    if (version < 17 || compatible > 17 || compatible > version) return 0;
    structure_start = be32(blob + 8);
    strings_start = be32(blob + 12);
    reserve_start = be32(blob + 16);
    structure_size = be32(blob + 36);
    strings_size = be32(blob + 32);
    if (structure_start < 40 || strings_start < 40 || reserve_start < 40 ||
        structure_start > total || structure_size > total - structure_start ||
        strings_start > total || strings_size > total - strings_start ||
        (structure_start & 3) || (structure_size & 3) || (reserve_start & 7) ||
        !mm_range_make(structure_start, structure_size, &structure_range)) return 0;
    strings_range.start = strings_start;
    strings_range.end = strings_start + strings_size;
    if (mm_range_intersects(structure_range, strings_range)) return 0;
    structure.data = blob + structure_start;
    structure.length = structure_size;
    strings.data = blob + strings_start;
    strings.length = strings_size;
    cursor = reserve_start;
    for (;;) {
        MmRange record, reservation;
        Bytes value;
        uint64_t start, size;
        if (cursor > total || total - cursor < 16) return 0;
        record.start = cursor;
        record.end = cursor + 16;
        if (mm_range_intersects(record, structure_range) || mm_range_intersects(record, strings_range)) return 0;
        value.data = blob + cursor;
        value.length = 8;
        if (!scalar(value, 2, &start)) return 0;
        value.data += 8;
        if (!scalar(value, 2, &size)) return 0;
        cursor += 16;
        if (!start && !size) break;
        if (!mm_range_make(start, size, &reservation) || !mm_ranges_insert(&info->reserved, reservation)) return 0;
    }
    cursor = 0;
    while (cursor < structure.length) {
        uint32_t token;
        if (structure.length - cursor < 4) return 0;
        token = be32(structure.data + cursor);
        cursor += 4;
        if (token == 1) {
            Bytes name;
            size_t end;
            Node* node;
            if (depth == 32 || (!depth && root_seen) || !string_at(structure, cursor, 255, &name, &end)) return 0;
            cursor = (end + 3) & ~(size_t)3;
            node = &nodes[depth];
            mm_bytes_zero(node, sizeof(*node));
            node->name = name;
            node->ac = node->parent_ac = 2;
            node->sc = node->parent_sc = 1;
            if (!depth) {
                if (name.length) return 0;
                root_seen = 1;
            } else {
                Node* parent = &nodes[depth - 1];
                parent->children = 1;
                node->parent_ac = parent->ac;
                node->parent_sc = parent->sc;
                node->disabled = inactive(parent);
                node->reserved_parent = depth == 1 && equal(name, "reserved-memory");
                node->reserved_child = parent->reserved_parent;
            }
            depth++;
        } else if (token == 2) {
            if (!depth || !complete(&nodes[depth - 1], depth, info)) return 0;
            depth--;
        } else if (token == 3) {
            uint32_t len, name_offset;
            Bytes name, value;
            size_t end;
            if (!depth || nodes[depth - 1].children || structure.length - cursor < 8) return 0;
            len = be32(structure.data + cursor);
            name_offset = be32(structure.data + cursor + 4);
            cursor += 8;
            if (len > structure.length - cursor) return 0;
            value.data = structure.data + cursor;
            value.length = len;
            cursor = (cursor + len + 3) & ~(size_t)3;
            if (!string_at(strings, name_offset, 31, &name, &end) || !property(&nodes[depth - 1], name, value)) return 0;
        } else if (token == 9) {
            if (depth || !root_seen || cursor != structure.length || !info->ram.count) return 0;
            info->dtb_size = total;
            return 1;
        } else if (token != 4) return 0;
        if (cursor > structure.length) return 0;
    }
    return 0;
}

int mm_discovery_finish(MmDiscovery* info, MmRanges* available) {
    size_t i, j;
    MmRange range;
    if (!info->dtb_size || available->count) return 0;
    for (i = 0; i < info->ram.count; i++) {
        if (!mm_ranges_insert(available, info->ram.entries[i])) return 0;
    }
    for (i = 0; i < info->reserved.count; i++) {
        if (!mm_range_outward(info->reserved.entries[i], MM_PAGE_SIZE, &range) || !mm_ranges_remove(available, range)) return 0;
    }
    for (i = 0; i < info->no_map.count; i++) {
        if (!mm_range_outward(info->no_map.entries[i], MM_BLOCK_SIZE, &range) || !mm_ranges_remove(available, range)) return 0;
    }
    for (i = 0; i < info->dynamic_count; i++) {
        const MmDynamic* d = &info->dynamic[i];
        uint64_t size, address = 0;
        uint64_t minimum = d->no_map ? MM_BLOCK_SIZE : MM_PAGE_SIZE;
        uint64_t alignment = d->alignment > minimum ? d->alignment : minimum;
        MmRange all = {0, MM_RAM_LIMIT};
        int found = 0;
        if (!mm_align_up(d->size, minimum, &size)) return 0;
        for (j = 0; j < (d->allowed_count ? d->allowed_count : 1); j++) {
            if (mm_ranges_alloc(available, size, alignment, d->allowed_count ? d->allowed[j] : all, &address)) {
                found = 1;
                break;
            }
        }
        if (!found || !mm_range_make(address, size, &range) || !mm_ranges_insert(&info->reserved, range) ||
            (d->no_map && !mm_ranges_insert(&info->no_map, range))) return 0;
    }
    info->dynamic_count = 0;
    return 1;
}
