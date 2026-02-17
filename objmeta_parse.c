/*
 * Object Metadata (Object Audio Metadata) Parser
 * Parses object metadata payloads extracted from E-AC-3 EMDF containers
 * per ETSI TS 103 420 clause 5.5.2
 *
 * Extracts: object count, positions (X/Y/Z), gain, priority, size
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ---- Bitstream reader ---- */
typedef struct {
    const uint8_t *buf;
    int size;
    int bit_pos;
} BitReader;

static void br_init(BitReader *br, const uint8_t *buf, int size) {
    br->buf = buf; br->size = size; br->bit_pos = 0;
}

static int br_bits_left(const BitReader *br) {
    return br->size * 8 - br->bit_pos;
}

static uint32_t br_read(BitReader *br, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        int byte_idx = br->bit_pos >> 3;
        int bit_idx  = 7 - (br->bit_pos & 7);
        if (byte_idx < br->size)
            val = (val << 1) | ((br->buf[byte_idx] >> bit_idx) & 1);
        else
            val <<= 1;
        br->bit_pos++;
    }
    return val;
}

static int32_t br_read_signed(BitReader *br, int n) {
    uint32_t val = br_read(br, n);
    /* Sign extend */
    if (val & (1u << (n - 1)))
        val |= ~((1u << n) - 1);
    return (int32_t)val;
}

static void br_skip(BitReader *br, int n) { br->bit_pos += n; }

static uint32_t variable_bits(BitReader *br, int n_bits) {
    uint32_t value = 0;
    int read_more;
    do {
        value += br_read(br, n_bits);
        read_more = br_read(br, 1);
        if (read_more) {
            value <<= n_bits;
            value += (1 << n_bits);
        }
    } while (read_more);
    return value;
}

static uint32_t variable_bits_max(BitReader *br, int n_bits, int max_groups) {
    uint32_t value = 0;
    int num_group = 1;
    value += br_read(br, n_bits);
    int read_more = br_read(br, 1);
    if (max_groups > num_group) {
        if (read_more) {
            value <<= n_bits;
            value += (1u << n_bits);
        }
        while (read_more) {
            value += br_read(br, n_bits);
            read_more = br_read(br, 1);
            num_group++;
            if (num_group >= max_groups) break;
            if (read_more) {
                value <<= n_bits;
                value += (1u << n_bits);
            }
        }
    }
    return value;
}

/* ---- Object state ---- */
typedef struct {
    int active;
    float x, y, z;        /* 3D position: x,y in [0,1], z in [-1,1] */
    float gain_db;         /* gain in dB */
    float priority;        /* [0, 1] */
    float width, depth, height;  /* object size */
    int screen_ref;
} ObjectState;

/* ---- object metadata parser context ---- */
typedef struct {
    int object_count;
    int b_dyn_object_only;
    int b_lfe_present;
    int b_alternate_object_data_present;
    ObjectState objects[128];
} ObjMetaContext;

/* Parse program_assignment */
static void parse_program_assignment(BitReader *br, ObjMetaContext *ctx) {
    ctx->b_dyn_object_only = br_read(br, 1);
    if (ctx->b_dyn_object_only) {
        ctx->b_lfe_present = br_read(br, 1);
    } else {
        /* Non-dynamic-object-only program: has bed/ISF config */
        /* content_description_mask */
        uint32_t content_desc_mask = br_read(br, 4);
        if (content_desc_mask & 0x8) { /* b_bed_instance_present */
            uint32_t num_bed_instances = br_read(br, 3) + 1;
            for (uint32_t i = 0; i < num_bed_instances; i++) {
                int b_use_default_bed_instance_assignment = br_read(br, 1);
                if (!b_use_default_bed_instance_assignment) {
                    /* bed_instance_assignment */
                    uint32_t bed_ch_count = br_read(br, 4) + 1;
                    for (uint32_t ch = 0; ch < bed_ch_count; ch++) {
                        br_read(br, 4); /* output_target_idx */
                        br_read(br, 4); /* codec_ch_idx */
                    }
                }
            }
        }
        if (content_desc_mask & 0x4) { /* b_isf_objects_present */
            uint32_t num_isf_objects = br_read(br, 3) + 1;
            for (uint32_t i = 0; i < num_isf_objects; i++) {
                br_read(br, 4); /* isf_object_codec_ch_idx */
            }
        }
        if (content_desc_mask & 0x2) { /* b_dyn_objects_present */
            uint32_t num_dyn_objects = br_read(br, 5);
            if (num_dyn_objects == 0x1F) num_dyn_objects += br_read(br, 7);
            /* dyn_objects_count = num_dyn_objects (already decoded) */
        }
        if (content_desc_mask & 0x1) { /* b_reserved */
            /* skip reserved data */
        }
        ctx->b_lfe_present = br_read(br, 1);
    }
}

/* Parse md_update_info */
static int parse_md_update_info(BitReader *br, int *num_blocks) {
    uint32_t sample_offset_code = br_read(br, 2);
    if (sample_offset_code == 1) {
        br_read(br, 2); /* sample_offset_idx */
    } else if (sample_offset_code == 2) {
        br_read(br, 5); /* sample_offset_bits */
    }

    uint32_t num_obj_info_blocks_bits = br_read(br, 3);
    *num_blocks = num_obj_info_blocks_bits + 1;

    /* block_update_info for each block */
    for (int blk = 0; blk < *num_blocks; blk++) {
        br_read(br, 6); /* block_offset_factor_bits */
        uint32_t ramp_dur_code = br_read(br, 2);
        if (ramp_dur_code == 3) {
            int b_use_idx = br_read(br, 1);
            if (b_use_idx) {
                br_read(br, 4); /* ramp_duration_idx */
            } else {
                br_read(br, 11); /* ramp_duration_bits */
            }
        }
    }
    return 0;
}

/* Parse object_basic_info */
static void parse_object_basic_info(BitReader *br, ObjMetaContext *ctx, int obj_idx,
                                     int status_idx) {
    int b_gain_update = 1, b_priority_update = 1;

    if (status_idx == 0x01) {
        /* Full update: both gain and priority present */
        b_gain_update = 1;
        b_priority_update = 1;
    } else {
        /* Selective update */
        b_priority_update = br_read(br, 1);
        b_gain_update = br_read(br, 1);
    }

    if (b_gain_update) {
        uint32_t gain_idx = br_read(br, 2);
        switch (gain_idx) {
            case 0: ctx->objects[obj_idx].gain_db = 0.0f; break;
            case 1: ctx->objects[obj_idx].gain_db = -100.0f; break; /* -inf */
            case 2: {
                uint32_t gain_bits = br_read(br, 6);
                if (gain_bits <= 14)
                    ctx->objects[obj_idx].gain_db = (float)(15 - (int)gain_bits);
                else
                    ctx->objects[obj_idx].gain_db = (float)(14 - (int)gain_bits);
                break;
            }
            case 3: /* reuse previous */ break;
        }
    }

    if (b_priority_update) {
        int b_default = br_read(br, 1);
        if (!b_default) {
            uint32_t prio_bits = br_read(br, 5);
            ctx->objects[obj_idx].priority = (float)prio_bits / 32.0f;
        } else {
            ctx->objects[obj_idx].priority = 1.0f;
        }
    }
}

/* Parse object_render_info */
static void parse_object_render_info(BitReader *br, ObjMetaContext *ctx,
                                      int obj_idx, int blk, int status_idx) {
    int ri[4] = {1, 1, 1, 1}; /* position, zone, size, screen_ref */

    if (status_idx != 0x01) {
        /* Selective update */
        for (int i = 0; i < 4; i++)
            ri[i] = br_read(br, 1);
    }

    /* Position update */
    if (ri[3]) { /* Note: ri[3] controls position in the spec's bit ordering */
        int b_diff = 0;
        if (blk > 0) {
            b_diff = br_read(br, 1);
        }

        if (b_diff) {
            int32_t dx = br_read_signed(br, 3);
            int32_t dy = br_read_signed(br, 3);
            int32_t dz = br_read_signed(br, 3);
            /* Apply delta to previous raw value, then renormalize */
            ctx->objects[obj_idx].x += (float)dx / 64.0f;
            ctx->objects[obj_idx].y += (float)dy / 64.0f;
            ctx->objects[obj_idx].z += (float)dz / 16.0f;
            /* Clamp */
            if (ctx->objects[obj_idx].x < 0) ctx->objects[obj_idx].x = 0;
            if (ctx->objects[obj_idx].x > 1) ctx->objects[obj_idx].x = 1;
            if (ctx->objects[obj_idx].y < 0) ctx->objects[obj_idx].y = 0;
            if (ctx->objects[obj_idx].y > 1) ctx->objects[obj_idx].y = 1;
            if (ctx->objects[obj_idx].z < -1) ctx->objects[obj_idx].z = -1;
            if (ctx->objects[obj_idx].z > 1) ctx->objects[obj_idx].z = 1;
        } else {
            uint32_t x_bits = br_read(br, 6);
            uint32_t y_bits = br_read(br, 6);
            uint32_t z_sign = br_read(br, 1);
            uint32_t z_bits = br_read(br, 4);
            ctx->objects[obj_idx].x = (float)x_bits / 64.0f;
            ctx->objects[obj_idx].y = (float)y_bits / 64.0f;
            ctx->objects[obj_idx].z = (z_sign ? 1.0f : -1.0f) * (float)z_bits / 16.0f;
        }

        /* distance */
        int b_dist = br_read(br, 1);
        if (b_dist) {
            int b_inf = br_read(br, 1);
            if (!b_inf) {
                br_read(br, 4); /* distance_factor_idx */
            }
        }
    }

    /* Zone constraints */
    if (ri[1]) {
        br_read(br, 3); /* zone_constraints_idx */
        br_read(br, 1); /* b_enable_elevation */
    }

    /* Size */
    if (ri[2]) {
        uint32_t size_idx = br_read(br, 2);
        if (size_idx == 0) {
            ctx->objects[obj_idx].width = 0;
            ctx->objects[obj_idx].depth = 0;
            ctx->objects[obj_idx].height = 0;
        } else if (size_idx == 1) {
            uint32_t sz = br_read(br, 5);
            float s = (float)sz / 32.0f;
            ctx->objects[obj_idx].width = s;
            ctx->objects[obj_idx].depth = s;
            ctx->objects[obj_idx].height = s;
        } else if (size_idx == 2) {
            ctx->objects[obj_idx].width  = (float)br_read(br, 5) / 32.0f;
            ctx->objects[obj_idx].depth  = (float)br_read(br, 5) / 32.0f;
            ctx->objects[obj_idx].height = (float)br_read(br, 5) / 32.0f;
        }
    }

    /* Screen reference (only if position was updated) */
    if (ri[3]) {
        ctx->objects[obj_idx].screen_ref = br_read(br, 1);
        if (ctx->objects[obj_idx].screen_ref) {
            br_read(br, 3); /* screen_factor_bits */
            br_read(br, 2); /* depth_factor_idx */
        }
    }

    /* Snap */
    br_read(br, 1); /* b_object_snap */
}

/* Parse object_info_block */
static void parse_object_info_block(BitReader *br, ObjMetaContext *ctx,
                                     int obj_idx, int blk,
                                     int b_object_in_bed_or_isf) {
    int b_not_active = br_read(br, 1);

    int basic_status = 0;
    if (b_not_active) {
        basic_status = 0;
        ctx->objects[obj_idx].active = 0;
    } else {
        ctx->objects[obj_idx].active = 1;
        if (blk == 0) {
            basic_status = 1; /* full update on first block */
        } else {
            basic_status = br_read(br, 2);
        }
    }

    if (basic_status == 1 || basic_status == 3) {
        parse_object_basic_info(br, ctx, obj_idx, basic_status);
    }

    int render_status = 0;
    if (b_not_active) {
        render_status = 0;
    } else {
        if (!b_object_in_bed_or_isf) {
            if (blk == 0) {
                render_status = 1;
            } else {
                render_status = br_read(br, 2);
            }
        }
    }

    if (render_status == 1 || render_status == 3) {
        parse_object_render_info(br, ctx, obj_idx, blk, render_status);
    }

    /* Additional table data */
    int b_add = br_read(br, 1);
    if (b_add) {
        uint32_t add_size = br_read(br, 4) + 1;
        br_skip(br, add_size * 8); /* skip additional data + padding */
    }
}

/* Parse object_element (oa_element_id_idx == 1) */
static void parse_object_element(BitReader *br, ObjMetaContext *ctx) {
    int num_blocks = 0;
    parse_md_update_info(br, &num_blocks);

    int b_reserved_not_present = br_read(br, 1);
    if (!b_reserved_not_present) {
        br_read(br, 5); /* reserved */
    }

    /* For each object, parse all blocks */
    for (int j = 0; j < ctx->object_count; j++) {
        for (int blk = 0; blk < num_blocks; blk++) {
            parse_object_info_block(br, ctx, j, blk, 0 /* not bed/isf */);
        }
    }
}

/* Parse one object metadata payload */
static int parse_objmeta_payload(const uint8_t *payload, int payload_size,
                               ObjMetaContext *ctx, int frame_num, int verbose) {
    BitReader br;
    br_init(&br, payload, payload_size);

    /* oa_md_version */
    uint32_t version = br_read(&br, 2);
    if (version == 3) version += br_read(&br, 3);

    /* object_count */
    uint32_t obj_count_bits = br_read(&br, 5);
    if (obj_count_bits == 0x1F) obj_count_bits += br_read(&br, 7);
    ctx->object_count = obj_count_bits + 1;

    /* program_assignment */
    parse_program_assignment(&br, ctx);

    /* b_alternate_object_data_present */
    ctx->b_alternate_object_data_present = br_read(&br, 1);

    /* oa_element_count */
    uint32_t elem_count_bits = br_read(&br, 4);
    if (elem_count_bits == 0xF) elem_count_bits += br_read(&br, 5);
    int oa_element_count = elem_count_bits;

    if (verbose) {
        printf("  object metadata: version=%d, objects=%d, dyn_only=%d, lfe=%d, elements=%d\n",
               version, ctx->object_count, ctx->b_dyn_object_only,
               ctx->b_lfe_present, oa_element_count);
    }

    /* Parse each oa_element */
    for (int i = 0; i < oa_element_count; i++) {
        uint32_t elem_id = br_read(&br, 4);
        uint32_t elem_size = variable_bits_max(&br, 4, 4);

        if (ctx->b_alternate_object_data_present) {
            br_read(&br, 4); /* alternate_object_data_id_idx */
        }
        int b_discard = br_read(&br, 1);

        int elem_body_start = br.bit_pos;
        int elem_body_end = elem_body_start + elem_size * 8;

        if (verbose) {
            printf("  Element #%d: id=%d, size=%d bytes, discard=%d\n",
                   i, elem_id, elem_size, b_discard);
        }

        if (elem_id == 1) {
            /* object_element - contains positions */
            parse_object_element(&br, ctx);
        } else {
            /* Skip unknown element */
            br.bit_pos = elem_body_end;
        }

        /* Align to element boundary */
        if (br.bit_pos < elem_body_end) {
            br.bit_pos = elem_body_end;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    const char *input = "objcoding_objmeta_payloads.bin";
    if (argc > 1) input = argv[1];

    FILE *fin = fopen(input, "rb");
    if (!fin) { fprintf(stderr, "Cannot open %s\n", input); return 1; }
    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    uint8_t *data = malloc(file_size);
    fread(data, 1, file_size, fin);
    fclose(fin);

    printf("=== Object Metadata Position Parser ===\n");
    printf("File: %s (%ld bytes)\n\n", input, file_size);

    ObjMetaContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    long pos = 0;
    int frame_count = 0;

    while (pos + 8 <= file_size) {
        /* Read header: frame_num(4) + payload_size(4) */
        uint32_t frame_num = ((uint32_t)data[pos] << 24) | (data[pos+1] << 16) |
                             (data[pos+2] << 8) | data[pos+3];
        uint32_t payload_size = ((uint32_t)data[pos+4] << 24) | (data[pos+5] << 16) |
                                (data[pos+6] << 8) | data[pos+7];
        pos += 8;

        if (pos + payload_size > file_size) {
            printf("ERROR: payload extends beyond file at frame %d\n", frame_num);
            break;
        }

        printf("--- Frame %d (payload=%d bytes) ---\n", frame_num, payload_size);

        int verbose = (frame_count < 3); /* verbose for first few frames */
        int ret = parse_objmeta_payload(data + pos, payload_size, &ctx, frame_num, verbose);

        if (ret == 0) {
            /* Print object positions */
            printf("  Objects: %d | Positions:\n", ctx.object_count);
            for (int j = 0; j < ctx.object_count && j < 32; j++) {
                if (ctx.objects[j].active) {
                    printf("    Obj %2d: X=%.4f Y=%.4f Z=%.4f "
                           "gain=%.1fdB prio=%.2f%s\n",
                           j, ctx.objects[j].x, ctx.objects[j].y,
                           ctx.objects[j].z, ctx.objects[j].gain_db,
                           ctx.objects[j].priority,
                           ctx.objects[j].screen_ref ? " [screen]" : "");
                } else {
                    printf("    Obj %2d: [inactive]\n", j);
                }
            }
        } else {
            printf("  ERROR parsing object metadata\n");
        }

        pos += payload_size;
        frame_count++;
        printf("\n");
    }

    free(data);
    printf("=== Done: %d frames parsed ===\n", frame_count);
    return 0;
}
