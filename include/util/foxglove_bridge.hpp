#pragma once

// Foxglove WebSocket bridge — uses official foxglove-sdk.
//
// YAML config:
//   foxglove:
//     host: "0.0.0.0"
//     port: 8765
//     channels:
//       /chassis/velocity: { type: double, hz: 50 }
//       /camera/image:     { type: image,  hz: 10 }

#include "core/component.hpp"
#include "util/throttle.hpp"

#include <foxglove/channel.hpp>
#include <foxglove/foxglove.hpp>
#include <foxglove/messages.hpp>
#include <foxglove/websocket.hpp>

#include <opencv2/core.hpp>

#include <charconv>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace nuedcs::util {

class FoxgloveBridge final : public core::Component {
public:
    explicit FoxgloveBridge(ryml::NodeRef config) {
        auto c = core::Config{config};
        host_ = c["host"].str("0.0.0.0");
        port_ = static_cast<int>(c["port"].get<uint16_t>(8765));

        if (!config.has_child("channels") || !config["channels"].is_map()) {
            spdlog::warn("[FoxgloveBridge] no channels configured");
            return;
        }

        for (auto ch : config["channels"].children()) {
            std::string topic(ch.key().str, ch.key().len);
            auto& ch_cfg = ch;
            std::string type_str(ch_cfg["type"].val().str, ch_cfg["type"].val().len);
            float hz_f = 10.0f;
            if (ch_cfg.has_child("hz"))
                ch_cfg["hz"] >> hz_f;

            if (type_str == "float") {
                add_scalar<float>(topic, hz_f);
            } else if (type_str == "double") {
                add_scalar<double>(topic, hz_f);
            } else if (type_str == "int" || type_str == "int32") {
                add_scalar<int>(topic, hz_f);
            } else if (type_str == "image") {
                add_image(topic, hz_f);
            } else {
                spdlog::warn("[FoxgloveBridge] unknown type '{}' for {}", type_str, topic);
            }
        }
    }

    ~FoxgloveBridge() override {
        if (server_) server_->stop();
    }

    bool init() override {
        if (channels_.empty()) {
            warn("no channels, bridge disabled");
            return true;
        }

        foxglove::setLogLevel(foxglove::LogLevel::Warn);

        foxglove::WebSocketServerOptions opts;
        opts.name = "NUEDC";
        opts.host = host_;
        opts.port = port_;
        opts.capabilities = foxglove::WebSocketServerCapabilities::ClientPublish;
        opts.supported_encodings = {"json"};

        auto result = foxglove::WebSocketServer::create(std::move(opts));
        if (!result.has_value()) {
            error("failed to create server: {}", foxglove::strerror(result.error()));
            return false;
        }
        server_ = std::make_unique<foxglove::WebSocketServer>(std::move(result.value()));

        for (auto& ch : channels_)
            ch->create();

        info("ws://{}:{} started, {} channels", host_, port_, channels_.size());
        return true;
    }

    void update() override {
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

        for (auto& ch : channels_) {
            if (!ch->active()) continue;
            if (!ch->throttle.ready()) continue;
            ch->log(now_ns);
        }
    }

private:
    // ── Channel abstraction ────────────────────────────────────────────

    struct Channel {
        uint64_t id = 0;
        std::string topic;
        std::string type_name;
        nuedc::Throttle throttle;
        std::unique_ptr<foxglove::RawChannel> raw_channel;

        Channel(std::string topic, std::string type_name, float hz)
            : topic(std::move(topic)),
              type_name(std::move(type_name)), throttle(hz) {}

        virtual ~Channel() = default;
        virtual bool active() const = 0;
        virtual void create() = 0;
        virtual void log(uint64_t timestamp_ns) = 0;
    };

    // ── Scalar channel (float/double/int → JSON) ───────────────────────

    template <typename T>
    struct ScalarChannel final : Channel {
        InputInterface<T> input;
        using Channel::Channel;

        bool active() const override { return raw_channel != nullptr; }

        void create() override {
            foxglove::Schema s;
            s.name = "foxglove.TimestampedValue";
            s.encoding = "jsonschema";
            auto json = R"({"type":"object","properties":{"value":{"type":"number"}}})";
            s.data = reinterpret_cast<const std::byte*>(json);
            s.data_len = ::strlen(json);
            auto r = foxglove::RawChannel::create(topic, "json", std::move(s));
            if (r.has_value()) {
                raw_channel = std::make_unique<foxglove::RawChannel>(std::move(r.value()));
                id = raw_channel->id();
            }
        }

        void log(uint64_t ts_ns) override {
            auto* ptr = static_cast<const T*>(input.raw_ptr());
            if (!ptr) return;
            char buf[32];
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf) - 1, *ptr);
            *end = '\0';
            std::string payload = R"({"value":)" + std::string(buf, end - buf) + "}";
            raw_channel->log(reinterpret_cast<const std::byte*>(payload.data()),
                             payload.size(), ts_ns);
        }
    };

    template <typename T>
    void add_scalar(const std::string& topic, float hz) {
        auto ch = std::make_unique<ScalarChannel<T>>(topic, typeid(T).name(), hz);
        register_input(topic, ch->input, false);
        channels_.push_back(std::move(ch));
    }

    // ── Image channel (cv::Mat → RawImage, SDK handles schema) ─────────

    struct ImageChannel final : Channel {
        InputInterface<cv::Mat> input;
        std::unique_ptr<foxglove::messages::RawImageChannel> ra_channel;

        using Channel::Channel;

        bool active() const override { return ra_channel != nullptr; }

        void create() override {
            auto r = foxglove::messages::RawImageChannel::create(topic);
            if (r.has_value()) {
                ra_channel = std::make_unique<foxglove::messages::RawImageChannel>(
                    std::move(r.value()));
                id = ra_channel->id();
            } else {
                spdlog::warn("[FoxgloveBridge] image channel create failed for {}: {}",
                             topic, foxglove::strerror(r.error()));
            }
        }

        void log(uint64_t ts_ns) override {
            auto* mat = static_cast<const cv::Mat*>(input.raw_ptr());
            if (!mat) return;
            if (mat->empty()) return;

            foxglove::messages::RawImage msg;
            msg.width    = static_cast<uint32_t>(mat->cols);
            msg.height   = static_cast<uint32_t>(mat->rows);
            msg.step     = static_cast<uint32_t>(mat->cols * mat->channels());
            msg.encoding = "bgr8";
            msg.frame_id = "";

            size_t n = mat->total() * mat->elemSize();
            msg.data.resize(n);
            std::memcpy(msg.data.data(), mat->data, n);

            foxglove::messages::Timestamp ts;
            ts.sec  = static_cast<uint32_t>(ts_ns / 1'000'000'000);
            ts.nsec = static_cast<uint32_t>(ts_ns % 1'000'000'000);
            msg.timestamp = ts;

            ra_channel->log(msg);
        }
    };

    void add_image(const std::string& topic, float hz) {
        auto ch = std::make_unique<ImageChannel>(topic, "cv::Mat", hz);
        register_input(topic, ch->input, false);
        channels_.push_back(std::move(ch));
    }

    // ── Members ────────────────────────────────────────────────────────

    std::string host_ = "0.0.0.0";
    int port_ = 8765;

    std::vector<std::unique_ptr<Channel>> channels_;

    std::unique_ptr<foxglove::WebSocketServer> server_;
};

} // namespace nuedcs::util
