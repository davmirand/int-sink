#include "process.h"

#include "types/int.h"
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

#include "meta.h"
#include "helpers/endian.h"

struct raw_int {
    struct int14_shim_t shim;
    struct telemetry_report_v10_t telemetry_report;
    __u32 data[249];
};

static __u32 packet_pop_int(struct xdp_md *ctx, struct raw_int *buffer);
static __u16 int_checksum(struct raw_int *buffer);

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, __u32);
    __type(value, struct raw_int);
    __uint(max_entries, 1);
} int_buffer SEC(".maps"); // Can replace with just the output buffer

SEC("xdp")
int test_int(struct xdp_md *ctx)
{
    meta_push(ctx, DSCP_INT << 2);
    return process_int(ctx);
}

__u32 process_int(struct xdp_md *ctx)
{
    struct raw_int *int_data;
    __u32 result;

    __u32 key = 0;

    // Check DSCP/TOS from IP header

    result = meta_pop(ctx);

    result >>= 2;

    if ((result & DSCP_INT) ^ DSCP_INT)
        return NONFATAL_ERR;

    // Prepare buffer

    int_data = bpf_map_lookup_elem(&int_buffer, &key);

    if(!int_data)
        return FATAL_ERR;

    result = packet_pop_int(ctx, int_data);

    if (result)
        return result;

    // Successfully copied to buffer

    // 3 Values need to be sent back up
    // DSCP - almost completely unimportant

    result = meta_push(ctx, int_data->shim.DSCP << 2);

    if (result)
        return FATAL_ERR;

    // Size delta - VERY IMPORTANT
    // CSUM delta - VERY IMPORTANT

    union meta_info info;

    info.data.csum_delta = ~int_checksum(int_data);
    info.data.size_delta = -(((__u16)int_data->shim.len) << 2);
    
    result = meta_push(ctx, info.combined_data);

    if (result)
        return FATAL_ERR;

    return NO_ERR;
}

static __u32 packet_pop_int(struct xdp_md *ctx, struct raw_int *buffer)
{
    __u32 *buf = (void*)buffer;
    void *pkt = (void*)(long)ctx->data;
    void *end = (void*)(long)ctx->data_end;

    // Parsing

    struct int14_shim_t *shim = pkt;
    __u32 *pos = pkt;

    if (shim + 1 > end)
        return NONFATAL_ERR;
    
    __u32 size = shim->len;

    if ((size * sizeof(*buf)) < sizeof(struct int14_shim_t))
        return NONFATAL_ERR;

    if ( ( pos + size ) > end)
        return NONFATAL_ERR;

    // End parsing

    // Copy from packet to buffer
    #pragma unroll
    for(int i = 0; i < sizeof(*buffer) / sizeof(*buf); i++)
    {   
        if ((pos + i + 1) > end || (buf + i + 1) > (buffer + 1) || i >= size )
        {
            break;
        }
        buf[i] = pos[i];
    }

    // Shrinking packet
    if (bpf_xdp_adjust_head(ctx, size * sizeof(*buf)))
        return FATAL_ERR;

    return NO_ERR;
}

static __u16 int_checksum(struct raw_int *buffer)
{
    __u32 size = buffer->shim.len;

    __u64 sum = 0;

    __u32 *buf = (void*)buffer;

    #pragma unroll
    for(int i = 0; i < sizeof(*buffer) / sizeof(*buf); i++)
    {   
        if ((buf + i + 1) > (buffer + 1) || i >= size)
        {
            break;
        }
        sum += buf[i];
    }

    sum = (sum >> 32) + (sum & 0xFFFFFFFF); // Fold into 32 bits
    sum = (sum >> 32) + (sum & 0xFFFFFFFF); 
    sum = (sum >> 16) + (sum & 0xFFFF); // Fold into 16 bits
    sum = (sum >> 16) + (sum & 0xFFFF);
    return ntohs(sum);
}