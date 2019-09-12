#ifdef _WIN32
#else
#include <sys/socket.h>
#endif

#include <cstdint>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <queue>

#include "conn.hh"
#include "debug.hh"
#include "reader.hh"
#include "queue.hh"
#include "send.hh"
#include "writer.hh"

#include "formats/hevc.hh"

#define PTR_DIFF(a, b)  ((ptrdiff_t)((char *)(a) - (char *)(b)))

#define haszero64_le(v) (((v) - 0x0101010101010101) & ~(v) & 0x8080808080808080UL)
#define haszero32_le(v) (((v) - 0x01010101)         & ~(v) & 0x80808080UL)

#define haszero64_be(v) (((v) - 0x1010101010101010) & ~(v) & 0x0808080808080808UL)
#define haszero32_be(v) (((v) - 0x10101010)         & ~(v) & 0x08080808UL)

extern rtp_error_t __hevc_receiver_optimistic(kvz_rtp::reader *reader);
extern rtp_error_t __hevc_receiver(kvz_rtp::reader *reader);

static inline unsigned __find_hevc_start(uint32_t value)
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t u = (value >> 16) & 0xffff;
    uint16_t l = (value >>  0) & 0xffff;

    bool t1 = (l == 0);
    bool t2 = ((u & 0xff) == 0x01);
    bool t3 = (u == 0x0100);
    bool t4 = (((l >> 8) & 0xff) == 0);
#else
    uint16_t u = (value >>  0) & 0xffff;
    uint16_t l = (value >> 16) & 0xffff;

    bool t1 = (l == 0);
    bool t2 = (((u >> 8) & 0xff) == 0x01);
    bool t3 = (u == 0x0001);
    bool t4 = ((l & 0xff) == 0);
#endif

    if (t1) {
        /* 0x00000001 */
        if (t3)
            return 4;

        /* "value" definitely has a start code (0x000001XX), but at this
         * point we can't know for sure whether it's 3 or 4 bytes long.
         *
         * Return 5 to indicate that start length could not be determined
         * and that caller must check previous dword's last byte for 0x00 */
        if (t2)
            return 5;
    } else if (t4 && t3) {
        /* 0xXX000001 */
        return 4;
    }

    return 0;
}

/* NOTE: the area 0 - len (ie data[0] - data[len - 1]) must be addressable!
 * Do not add offset to "data" ptr before passing it to __get_hevc_start()! */
static ssize_t __get_hevc_start(uint8_t *data, size_t len, size_t offset, uint8_t& start_len)
{
    int found     = 0;
    bool prev_z   = false;
    bool cur_z    = false;
    size_t pos    = offset;
    uint8_t *ptr  = data + offset;
    uint8_t *tmp  = nullptr;
    uint8_t lb    = 0;
    uint32_t prev = UINT32_MAX;

    uint64_t prefetch = UINT64_MAX;
    uint32_t value    = UINT32_MAX;
    unsigned ret      = 0;

    /* We can get rid of the bounds check when looping through
     * non-zero 8 byte chunks by setting the last byte to zero.
     *
     * This added zero will make the last 8 byte zero check to fail
     * and when we get out of the loop we can check if we've reached the end */
    lb = data[len - 1];
    data[len - 1] = 0;

    while (pos < len) {
        prefetch = *(uint64_t *)ptr;

#if __BYTE_ORDER == __LITTLE_ENDIAN
        if (!prev_z && !(cur_z = haszero64_le(prefetch))) {
#else
        if (!prev_z && !(cur_z = haszero64_be(prefetch))) {
#endif
            /* pos is not used in the following loop so it makes little sense to
             * update it on every iteration. Faster way to do the loop is to save
             * ptr's current value before loop, update only ptr in the loop and when
             * the loop is exited, calculate the difference between tmp and ptr to get
             * the number of iterations done * 8 */
            tmp = ptr;

            do {
                ptr      += 8;
                prefetch  = *(uint64_t *)ptr;
#if __BYTE_ORDER == __LITTLE_ENDIAN
                cur_z     = haszero64_le(prefetch);
#else
                cur_z     = haszero64_be(prefetch);
#endif
            } while (!cur_z);

            pos += PTR_DIFF(ptr, tmp);

            if (pos >= len)
                break;
        }

        value = *(uint32_t *)ptr;

        if (cur_z)
#if __BYTE_ORDER == __LITTLE_ENDIAN
            cur_z = haszero32_le(value);
#else
            cur_z = haszero32_be(value);
#endif

        if (!prev_z && !cur_z)
            goto end;

        /* Previous dword had zeros but this doesn't. The only way there might be a start code
         * is if the most significant byte of current dword is 0x01 */
        if (prev_z && !cur_z) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
            /* previous dword: 0xXX000000 or 0xXXXX0000 and current dword 0x01XXXXXX */
            if (((value  >> 0) & 0xff) == 0x01 && ((prev >> 16) & 0xffff) == 0) {
                start_len = (((prev >>  8) & 0xffffff) == 0) ? 4 : 3;
#else
            if (((value >> 24) & 0xff) == 0x01 && ((prev >>  0) & 0xffff) == 0) {
                start_len = (((prev >>  0) & 0xffffff) == 0) ? 4 : 3;
#endif
                data[len - 1] = lb;
                return pos + 1;
            }
        }


        {
            if ((ret = start_len = __find_hevc_start(value)) > 0) {
                if (ret == 5) {
                    ret = 3;
#if __BYTE_ORDER == __LITTLE_ENDIAN
                    start_len = (((prev >> 24) & 0xff) == 0) ? 4 : 3;
#else
                    start_len = (((prev >>  0) & 0xff) == 0) ? 4 : 3;
#endif
                }

                data[len - 1] = lb;
                return pos + ret;
            }

#if __BYTE_ORDER == __LITTLE_ENDIAN
            uint16_t u = (value >> 16) & 0xffff;
            uint16_t l = (value >>  0) & 0xffff;
            uint16_t p = (prev  >> 16) & 0xffff;

            bool t1 = ((p & 0xffff) == 0);
            bool t2 = (((p >> 8) & 0xff) == 0);
            bool t4 = (l == 0x0100);
            bool t5 = (l == 0x0000 && u == 0x01);
#else
            uint16_t u = (value >>  0) & 0xffff;
            uint16_t l = (value >> 16) & 0xffff;
            uint16_t p = (prev  >>  0) & 0xffff;

            bool t1 = ((p & 0xffff) == 0);
            bool t2 = ((p & 0xff) == 0);
            bool t4 = (l == 0x0001);
            bool t5 = (l == 0x0000 && u == 0x01);
#endif
            if (t1 && t4) {
                /* previous dword 0xxxxx0000 and current dword is 0x0001XXXX */
                if (t4) {
                    start_len = 4;
                    data[len - 1] = lb;
                    return pos + 2;
                }
            /* Previous dwod was 0xXXXXXX00 */
            } else if (t2) {
                /* Current dword is 0x000001XX */
                if (t5) {
                    start_len = 4;
                    data[len - 1] = lb;
                    return pos + 3;
                }

                /* Current dword is 0x0001XXXX */
                else if (t4) {
                    start_len = 3;
                    data[len - 1] = lb;
                    return pos + 2;
                }
            }

        }
end:
        prev_z = cur_z;
        pos += 4;
        ptr += 4;
        prev = value;
    }

    data[len - 1] = lb;
    return -1;
}

static rtp_error_t __push_hevc_frame(
    kvz_rtp::connection *conn, kvz_rtp::frame_queue *fqueue,
    uint8_t *data, size_t data_len,
    bool more
)
{
    uint8_t nalType  = (data[0] >> 1) & 0x3F;
    rtp_error_t ret  = RTP_OK;
    size_t data_left = data_len;
    size_t data_pos  = 0;

#ifdef __linux__
    /* Smaller-than-MTU frames can be enqueued without flushing the queue before return
     * because they don't store any extra info to __push_hevc_frame()'s stack.
     *
     * Larger frames on the other hand require that once all the data ("data" ptr)
     * has been processed, the frame queue must be flushed because the fragment headers
     * are stored to __push_hevc_frame()'s stack and on return that memory is not addressable */
    if (data_len <= MAX_PAYLOAD) {
        if ((ret = fqueue->enqueue_message(conn, data, data_len)) != RTP_OK)
            return ret;
        return more ? RTP_NOT_READY : RTP_OK;
    }

    /* All fragment units share the same NAL and FU headers and these headers can be saved
     * to this function's stack. Each fragment is given an unique RTP header when enqueue_message()
     * is called because each fragment has its own sequence number */
    std::vector<std::pair<size_t, uint8_t *>> buffers;

    uint8_t nal_header[kvz_rtp::frame::HEADER_SIZE_HEVC_NAL] = {
        49 << 1, /* fragmentation unit */
        1,       /* TID */
    };

    /* one for first frag, one for all the middle frags and one for the last frag */
    uint8_t fu_headers[3 * kvz_rtp::frame::HEADER_SIZE_HEVC_FU] = {
        (uint8_t)((1 << 7) | nalType),
        nalType,
        (uint8_t)((1 << 6) | nalType)
    };

    buffers.push_back(std::make_pair(sizeof(nal_header), nal_header));
    buffers.push_back(std::make_pair(sizeof(uint8_t),    &fu_headers[0]));
    buffers.push_back(std::make_pair(MAX_PAYLOAD,        nullptr));

    data_pos   = kvz_rtp::frame::HEADER_SIZE_HEVC_NAL;
    data_left -= kvz_rtp::frame::HEADER_SIZE_HEVC_NAL;

    while (data_left > MAX_PAYLOAD) {
        buffers.at(2).first  = MAX_PAYLOAD;
        buffers.at(2).second = &data[data_pos];

        if ((ret = fqueue->enqueue_message(conn, buffers)) != RTP_OK)
            return ret;

        data_pos  += MAX_PAYLOAD;
        data_left -= MAX_PAYLOAD;

        /* from now on, use the FU header meant for middle fragments */
        buffers.at(1).second = &fu_headers[1];
    }

    /* use the FU header meant for the last fragment */
    buffers.at(1).second = &fu_headers[2];

    buffers.at(2).first  = data_left;
    buffers.at(2).second = &data[data_pos];

    if ((ret = fqueue->enqueue_message(conn, buffers)) != RTP_OK) {
        LOG_ERROR("Failed to send HEVC frame!");
        fqueue->empty_queue();
        return ret;
    }

    return fqueue->flush_queue(conn);
#else
    if (data_len <= MAX_PAYLOAD) {
        LOG_DEBUG("send unfrag size %zu, type %u", data_len, nalType);
        return kvz_rtp::generic::push_frame(conn, data, data_len, 0);
    }

    const size_t HEADER_SIZE =
        kvz_rtp::frame::HEADER_SIZE_RTP +
        kvz_rtp::frame::HEADER_SIZE_HEVC_NAL +
        kvz_rtp::frame::HEADER_SIZE_HEVC_FU;

    uint8_t buffer[HEADER_SIZE + MAX_PAYLOAD];

    conn->fill_rtp_header(buffer);

    buffer[kvz_rtp::frame::HEADER_SIZE_RTP + 0]  = 49 << 1;            /* fragmentation unit */
    buffer[kvz_rtp::frame::HEADER_SIZE_RTP + 1]  = 1;                  /* TID */
    buffer[kvz_rtp::frame::HEADER_SIZE_RTP +
           kvz_rtp::frame::HEADER_SIZE_HEVC_NAL] = (1 << 7) | nalType; /* Start bit + NAL type */

    data_pos   = kvz_rtp::frame::HEADER_SIZE_HEVC_NAL;
    data_left -= kvz_rtp::frame::HEADER_SIZE_HEVC_NAL;

    while (data_left > MAX_PAYLOAD) {
        memcpy(&buffer[HEADER_SIZE], &data[data_pos], MAX_PAYLOAD);

        if ((ret = kvz_rtp::send::send_frame(conn, buffer, sizeof(buffer))) != RTP_OK)
            return RTP_GENERIC_ERROR;

        conn->update_rtp_sequence(buffer);

        data_pos  += MAX_PAYLOAD;
        data_left -= MAX_PAYLOAD;

        /* Clear extra bits */
        buffer[kvz_rtp::frame::HEADER_SIZE_RTP +
               kvz_rtp::frame::HEADER_SIZE_HEVC_NAL] = nalType;
    }

    buffer[kvz_rtp::frame::HEADER_SIZE_RTP +
           kvz_rtp::frame::HEADER_SIZE_HEVC_NAL] |= (1 << 6); /* set E bit to signal end of data */

    memcpy(&buffer[HEADER_SIZE], &data[data_pos], data_left);

    return kvz_rtp::send::send_frame(conn, buffer, HEADER_SIZE + data_left);
#endif
}

rtp_error_t kvz_rtp::hevc::push_frame(kvz_rtp::connection *conn, uint8_t *data, size_t data_len, int flags)
{
    (void)flags;

#ifdef __linux__
    /* find first start code */
    uint8_t start_len = 0;
    int offset        = __get_hevc_start(data, data_len, 0, start_len);
    int prev_offset   = offset;
    size_t r_off      = 0;
    rtp_error_t ret   = RTP_GENERIC_ERROR;

    if (data_len < MAX_PAYLOAD) {
        r_off = (offset < 0) ? 0 : offset; /* TODO: this looks ugly */
        return kvz_rtp::generic::push_frame(conn, data + r_off, data_len - r_off, flags);
    }

    kvz_rtp::frame_queue *fqueue = conn->get_frame_queue();
    fqueue->init_queue(conn);

    while (offset != -1) {
        offset = __get_hevc_start(data, data_len, offset, start_len);

        if (offset != -1) {
            ret = __push_hevc_frame(conn, fqueue, &data[prev_offset], offset - prev_offset - start_len, true);

            if (ret != RTP_NOT_READY)
                goto error;

            prev_offset = offset;
        }
    }

    if ((ret = __push_hevc_frame(conn, fqueue, &data[prev_offset], data_len - prev_offset, false)) == RTP_OK)
        return RTP_OK;

error:
    fqueue->empty_queue();
    return ret;
#else
    uint8_t start_len;
    int32_t prev_offset = 0;
    int offset = __get_hevc_start(data, ata_len, 0, start_len);
    prev_offset = offset;

    while (offset != -1) {
        offset = __get_hevc_start(data, data_len, offset, start_len);

        if (offset > 4 && offset != -1) {
            if (__push_hevc_frame(conn, nullptr, &data[prev_offset], offset - prev_offset - start_len, false) == -1)
                return RTP_GENERIC_ERROR;

            prev_offset = offset;
        }
    }

    if (prev_offset == -1)
        prev_offset = 0;

    return __push_hevc_frame(conn, nullptr, &data[prev_offset], data_len - prev_offset, false);
#endif
}

rtp_error_t kvz_rtp::hevc::frame_receiver(kvz_rtp::reader *reader)
{
#ifdef __RTP_USE_OPTIMISTIC_RECEIVER__
    return __hevc_receiver_optimistic(reader);
#else
    return __hevc_receiver(reader);
#endif
}