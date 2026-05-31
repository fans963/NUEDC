#pragma once

// Protocol decoder — copied from nuedc_slave project.
// Wire format: 0x5A | u32_le_size | FlatBuffer_body | 0xA5

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nuedc::protocol {

static constexpr uint8_t FRAME_MAGIC1 = 0x5A;
static constexpr uint8_t FRAME_MAGIC2 = 0xA5;
static constexpr size_t MAX_FRAME_LEN = 512;

// H must provide: void on_frame(const uint8_t *data, size_t len)
// on_frame() receives the size-prefixed buffer (size_prefix + body)
// so callers can use flatbuffers::GetSizePrefixedRoot() directly.
template <typename H>
class ProtocolDecoder {
public:
    explicit constexpr ProtocolDecoder(H& handler) : handler_(handler) {}

    void feed(const uint8_t* data, size_t len)
    {
        for (size_t i = 0; i < len; i++) {
            uint8_t b = data[i];
            switch (state_) {
            case State::WAIT_HEAD:
                if (b == FRAME_MAGIC1) {
                    size_idx_ = 0;
                    state_ = State::READ_SIZE;
                }
                break;

            case State::READ_SIZE:
                frame_buf_[size_idx_++] = b;
                if (size_idx_ == 4) {
                    uint32_t body_len;
                    std::memcpy(&body_len, frame_buf_, 4);
                    if (body_len == 0 || body_len > MAX_FRAME_LEN) {
                        state_ = State::WAIT_HEAD;
                    } else {
                        payload_len_ = body_len;
                        payload_idx_ = 0;
                        state_ = State::READ_BODY;
                    }
                }
                break;

            case State::READ_BODY:
                frame_buf_[4 + payload_idx_++] = b;
                if (payload_idx_ == payload_len_)
                    state_ = State::CHECK_TAIL;
                break;

            case State::CHECK_TAIL:
                if (b == FRAME_MAGIC2)
                    handler_.on_frame(frame_buf_, 4 + payload_len_);
                state_ = State::WAIT_HEAD;
                break;
            }
        }
    }

private:
    enum class State : uint8_t {
        WAIT_HEAD,
        READ_SIZE,
        READ_BODY,
        CHECK_TAIL
    };

    H& handler_;
    State state_ = State::WAIT_HEAD;
    uint8_t size_idx_ = 0;
    uint32_t payload_len_ = 0;
    uint32_t payload_idx_ = 0;
    uint8_t frame_buf_[4 + MAX_FRAME_LEN]{};
};

} // namespace nuedc::protocol
